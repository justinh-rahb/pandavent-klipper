#include "pv_moonraker.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"

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

// ---------- mDNS discovery ----------

#define DISCOVER_TIMEOUT_MS  2000

static void discover_task(void *arg)
{
    (void)arg;
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_moonraker", "_tcp",
                                   DISCOVER_TIMEOUT_MS,
                                   PV_MOONRAKER_DISCOVER_MAX, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr: %s", esp_err_to_name(err));
    }

    // Rebuild cache under the discovery lock so a reader never sees a
    // half-populated entry.
    xSemaphoreTake(s_discover_lock, portMAX_DELAY);
    int n = 0;
    for (mdns_result_t *r = results; r != NULL && n < PV_MOONRAKER_DISCOVER_MAX; r = r->next) {
        pv_moonraker_service_t *out = &s_discover_cache[n];
        memset(out, 0, sizeof(*out));

        // hostname is optional in the record; instance_name is the friendly
        // label. Prefer hostname because it's what mainsailos et al. publish.
        const char *name = r->hostname ? r->hostname
                          : (r->instance_name ? r->instance_name : "unknown");
        // mDNS records may or may not include the .local suffix.
        if (r->hostname && strstr(r->hostname, ".local") == NULL) {
            snprintf(out->hostname, sizeof(out->hostname), "%s.local", name);
        } else {
            strncpy(out->hostname, name, sizeof(out->hostname) - 1);
        }
        out->port = r->port ? r->port : 7125;

        // First IPv4 address wins — Moonraker only serves over IPv4 in stock.
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                break;
            }
        }
        if (out->ip[0] == '\0') continue;   // skip records with no v4 addr
        n++;
    }
    s_discover_count = n;
    s_discovering    = false;
    xSemaphoreGive(s_discover_lock);

    if (results) mdns_query_results_free(results);
    ESP_LOGI(TAG, "discover done: %d printer(s)", n);
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

    // One-shot task; self-deletes when done. Own its own stack because
    // mdns_query_ptr blocks in the caller for up to DISCOVER_TIMEOUT_MS.
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
