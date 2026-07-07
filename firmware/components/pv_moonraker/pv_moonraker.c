#include "pv_moonraker.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <errno.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "pv_moonraker";

#define NVS_NS       "app_nvs"
#define KEY_HOST     "mk_host"
#define KEY_PORT     "mk_port"
#define KEY_APIKEY   "mk_apikey"

#define DEFAULT_PORT           7125
#define SUBSCRIBE_ID           1
#define RX_BUF_BYTES           4096
#define NETWORK_TIMEOUT_MS     10000
#define RECONNECT_TIMEOUT_MS   5000

static SemaphoreHandle_t         s_lock  = NULL;
static esp_websocket_client_handle_t s_ws = NULL;
static pv_moonraker_config_t     s_cfg   = {0};
static pv_moonraker_status_t     s_status = { .state = PV_MK_DISABLED };
static char                     *s_rx_buf = NULL;
static size_t                    s_rx_off = 0;

// mDNS discovery — its own lock so a discovery in flight can't stall the WS
// receive path.
static SemaphoreHandle_t       s_discover_lock = NULL;
static pv_moonraker_service_t  s_discover_cache[PV_MOONRAKER_DISCOVER_MAX];
static int                     s_discover_count = 0;
static bool                    s_discovering    = false;

// ---------- NVS ----------

static esp_err_t nvs_load(pv_moonraker_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }

    uint16_t p = 0;
    if (nvs_get_u16(h, KEY_PORT, &p) == ESP_OK && p > 0) out->port = p;
    else out->port = DEFAULT_PORT;

    sz = sizeof(out->api_key);
    nvs_get_str(h, KEY_APIKEY, out->api_key, &sz);   // optional
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const pv_moonraker_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, cfg->port ? cfg->port : DEFAULT_PORT);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_APIKEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- status parsing ----------

// Merge one status object (e.g. {"heater_bed":{"temperature":55.3}}) into the
// cached status. Fields not present in the update are left untouched, matching
// Moonraker's delta semantics.
static void merge_status_object(cJSON *status)
{
    if (!cJSON_IsObject(status)) return;

    cJSON *print = cJSON_GetObjectItemCaseSensitive(status, "print_stats");
    if (cJSON_IsObject(print)) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(print, "state");
        if (cJSON_IsString(state)) {
            s_status.printing = (strcmp(state->valuestring, "printing") == 0);
            ESP_LOGI(TAG, "print_stats.state=%s (printing=%d)",
                     state->valuestring, s_status.printing);
        }
    }
    cJSON *bed = cJSON_GetObjectItemCaseSensitive(status, "heater_bed");
    if (cJSON_IsObject(bed)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(bed, "temperature");
        if (cJSON_IsNumber(t)) s_status.bed_temp = (float)t->valuedouble;
        cJSON *g = cJSON_GetObjectItemCaseSensitive(bed, "target");
        if (cJSON_IsNumber(g)) s_status.bed_target = (float)g->valuedouble;
    }
}

// A complete Moonraker JSON-RPC frame has arrived. Route it.
static void handle_frame(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed");
        return;
    }

    // Subscribe response: {result:{status:{...}}}
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
        if (cJSON_IsObject(status)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            merge_status_object(status);
            s_status.state = PV_MK_SUBSCRIBED;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "subscribed; got initial status");
        }
        cJSON_Delete(root);
        return;
    }

    // notify_status_update: {method:"notify_status_update", params:[{...}, eventtime]}
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_status_update") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (cJSON_IsArray(params)) {
            cJSON *delta = cJSON_GetArrayItem(params, 0);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            merge_status_object(delta);
            xSemaphoreGive(s_lock);
        }
    }

    cJSON_Delete(root);
}

// ---------- ws events ----------

static void send_subscribe(void)
{
    // Ask for print_stats + heater_bed, no field filter.
    const char *req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"printer.objects.subscribe\","
        "\"params\":{\"objects\":{\"print_stats\":null,\"heater_bed\":null}},"
        "\"id\":1}";
    int sent = esp_websocket_client_send_text(s_ws, req, strlen(req),
                                              pdMS_TO_TICKS(NETWORK_TIMEOUT_MS));
    if (sent < 0) ESP_LOGW(TAG, "subscribe send failed");
}

// Frames can arrive split across multiple DATA events. We buffer partials and
// dispatch once payload_offset+data_len == payload_len.
static void on_data(esp_websocket_event_data_t *ev)
{
    if (ev->op_code != 0x01 && ev->op_code != 0x00) return;   // text / continuation
    if (ev->payload_len <= 0) return;

    if (ev->payload_offset == 0) s_rx_off = 0;
    if (s_rx_off + ev->data_len >= RX_BUF_BYTES) {
        ESP_LOGW(TAG, "rx buffer overflow (payload_len=%d); dropping", ev->payload_len);
        s_rx_off = 0;
        return;
    }
    memcpy(s_rx_buf + s_rx_off, ev->data_ptr, ev->data_len);
    s_rx_off += ev->data_len;

    if (ev->payload_offset + ev->data_len >= ev->payload_len) {
        handle_frame(s_rx_buf, s_rx_off);
        s_rx_off = 0;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = data;
    switch ((esp_websocket_event_id_t)id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = PV_MK_CONNECTED;
        xSemaphoreGive(s_lock);
        send_subscribe();
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(ev);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = PV_MK_DISCONNECTED;
        xSemaphoreGive(s_lock);
        break;
    default: break;
    }
}

// ---------- lifecycle ----------

static void stop_client(void)
{
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

static esp_err_t start_client(void)
{
    if (s_cfg.host[0] == '\0') {
        s_status.state = PV_MK_DISABLED;
        return ESP_OK;
    }
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%u/websocket",
             s_cfg.host, s_cfg.port ? s_cfg.port : DEFAULT_PORT);

    esp_websocket_client_config_t wc = {
        .uri                  = uri,
        .reconnect_timeout_ms = RECONNECT_TIMEOUT_MS,
        .network_timeout_ms   = NETWORK_TIMEOUT_MS,
    };
    s_ws = esp_websocket_client_init(&wc);
    if (s_ws == NULL) return ESP_FAIL;

    esp_err_t err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        stop_client();
        return err;
    }
    s_status.state = PV_MK_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}

esp_err_t pv_moonraker_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_discover_lock = xSemaphoreCreateMutex();
    if (s_discover_lock == NULL) return ESP_ERR_NO_MEM;

    s_rx_buf = malloc(RX_BUF_BYTES);
    if (s_rx_buf == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_load(&s_cfg);
    if (err != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Moonraker config saved; idle");
        s_status.state = PV_MK_DISABLED;
        return ESP_OK;
    }
    return start_client();
}

esp_err_t pv_moonraker_set_config(const pv_moonraker_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    stop_client();
    err = start_client();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pv_moonraker_get_config(pv_moonraker_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pv_moonraker_get_status(pv_moonraker_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pv_moonraker_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_PORT);
    nvs_erase_key(h, KEY_APIKEY);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// ---------- Subnet discovery ----------

// Non-blocking TCP-connect sweep of the current STA subnet. mDNS was flaky
// against typical Klipper setups (moonraker.conf doesn't advertise by default
// on every distro), so we probe the port directly instead. Anything that
// answers on `port` is treated as a Moonraker candidate — the user picks.

#define SCAN_BATCH_SIZE      32     // parallel non-blocking connects
#define SCAN_CONNECT_TO_MS   200    // per-batch select timeout

static bool get_sta_subnet(uint32_t *host_ip, uint32_t *subnet, uint32_t *broadcast)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return false;
    if (info.ip.addr == 0 || info.netmask.addr == 0) return false;

    *host_ip   = ntohl(info.ip.addr);
    uint32_t m = ntohl(info.netmask.addr);
    *subnet    = *host_ip & m;
    *broadcast = *subnet | ~m;
    return true;
}

static void record_hit(uint32_t ip_host_order, uint16_t port, int *n)
{
    if (*n >= PV_MOONRAKER_DISCOVER_MAX) return;
    pv_moonraker_service_t *out = &s_discover_cache[*n];
    memset(out, 0, sizeof(*out));
    struct in_addr a = { .s_addr = htonl(ip_host_order) };
    strncpy(out->ip, inet_ntoa(a), sizeof(out->ip) - 1);
    out->port = port;
    // No hostname source — leave blank, portal falls back to displaying the IP.
    (*n)++;
}

static void discover_task(void *arg)
{
    (void)arg;

    uint32_t host_ip = 0, subnet = 0, broadcast = 0;
    if (!get_sta_subnet(&host_ip, &subnet, &broadcast)) {
        ESP_LOGW(TAG, "no STA IP; discovery skipped");
        xSemaphoreTake(s_discover_lock, portMAX_DELAY);
        s_discover_count = 0;
        s_discovering    = false;
        xSemaphoreGive(s_discover_lock);
        vTaskDelete(NULL);
        return;
    }

    uint16_t port = s_cfg.port ? s_cfg.port : DEFAULT_PORT;
    int hits = 0;
    int attempts = 0, skipped_alloc = 0, immediate_fails = 0;
    int last_errno = 0;

    ESP_LOGI(TAG, "subnet scan: %u.%u.%u.0/24 port %u",
             (unsigned)(host_ip >> 24 & 0xFF), (unsigned)(host_ip >> 16 & 0xFF),
             (unsigned)(host_ip >> 8 & 0xFF), (unsigned)port);

    // Skip the network and broadcast addresses at each end.
    for (uint32_t ip = subnet + 1; ip < broadcast && hits < PV_MOONRAKER_DISCOVER_MAX;) {
        int      fds[SCAN_BATCH_SIZE];
        uint32_t ips[SCAN_BATCH_SIZE];
        int      n = 0;

        for (; ip < broadcast && n < SCAN_BATCH_SIZE; ip++) {
            if (ip == host_ip) continue;   // don't probe ourselves
            attempts++;

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { skipped_alloc++; continue; }
            // ioctl FIONBIO is more reliable on lwip than fcntl.
            int flag = 1;
            ioctl(fd, FIONBIO, &flag);

            struct sockaddr_in addr = {
                .sin_family      = AF_INET,
                .sin_port        = htons(port),
                .sin_addr.s_addr = htonl(ip),
            };
            int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
            // Accept anything that isn't an immediate hard failure. Notably
            // EINPROGRESS / EWOULDBLOCK / EAGAIN all mean "SYN sent, waiting".
            if (r == 0 || errno == EINPROGRESS || errno == EWOULDBLOCK ||
                errno == EAGAIN || errno == 0) {
                fds[n] = fd;
                ips[n] = ip;
                n++;
            } else {
                last_errno = errno;
                immediate_fails++;
                close(fd);
            }
        }
        if (n == 0) continue;

        fd_set wfds;
        FD_ZERO(&wfds);
        int maxfd = 0;
        for (int i = 0; i < n; ++i) {
            FD_SET(fds[i], &wfds);
            if (fds[i] > maxfd) maxfd = fds[i];
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = SCAN_CONNECT_TO_MS * 1000 };
        select(maxfd + 1, NULL, &wfds, NULL, &tv);

        for (int i = 0; i < n; ++i) {
            if (FD_ISSET(fds[i], &wfds)) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                    record_hit(ips[i], port, &hits);
                }
            }
            close(fds[i]);
        }
    }

    xSemaphoreTake(s_discover_lock, portMAX_DELAY);
    s_discover_count = hits;
    s_discovering    = false;
    xSemaphoreGive(s_discover_lock);
    ESP_LOGI(TAG, "subnet scan done: %d responder(s) on :%u"
                  " (attempts=%d, alloc_fail=%d, immediate_err=%d, last_errno=%d)",
             hits, (unsigned)port, attempts, skipped_alloc, immediate_fails, last_errno);
    vTaskDelete(NULL);
}

esp_err_t pv_moonraker_discover_start(void)
{
    if (s_discover_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_discover_lock, portMAX_DELAY);
    if (s_discovering) {
        xSemaphoreGive(s_discover_lock);
        return ESP_OK;   // coalesce
    }
    s_discovering = true;
    xSemaphoreGive(s_discover_lock);

    // One-shot task; self-deletes when done. Needs its own stack for the
    // ~256 non-blocking sockets we open in batches.
    if (xTaskCreate(discover_task, "pv_mk_disc", 4096, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_discover_lock, portMAX_DELAY);
        s_discovering = false;
        xSemaphoreGive(s_discover_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool pv_moonraker_is_discovering(void)
{
    if (s_discover_lock == NULL) return false;
    bool r;
    xSemaphoreTake(s_discover_lock, portMAX_DELAY);
    r = s_discovering;
    xSemaphoreGive(s_discover_lock);
    return r;
}

int pv_moonraker_get_discovered(pv_moonraker_service_t *out, int max)
{
    if (out == NULL || max <= 0 || s_discover_lock == NULL) return 0;
    int n;
    xSemaphoreTake(s_discover_lock, portMAX_DELAY);
    n = s_discover_count < max ? s_discover_count : max;
    memcpy(out, s_discover_cache, n * sizeof(pv_moonraker_service_t));
    xSemaphoreGive(s_discover_lock);
    return n;
}
