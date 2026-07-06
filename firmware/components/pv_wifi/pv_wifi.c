#include "pv_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "pv_wifi";

#define NVS_NS   "app_nvs"       // matches stock firmware
#define KEY_SSID "ssid"
#define KEY_PASS "password"

#define STA_MAX_RETRIES  5
#define BIT_CONNECTED    BIT0
#define BIT_FAILED       BIT1

static pv_wifi_state_t s_state = PV_WIFI_STATE_INIT;
static EventGroupHandle_t s_events = NULL;
static int s_retry = 0;

// ---------- NVS helpers ----------

static esp_err_t nvs_read_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_sz;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool load_saved_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    esp_err_t err = nvs_read_str(KEY_SSID, ssid, ssid_sz);
    if (err != ESP_OK || ssid[0] == '\0') return false;
    err = nvs_read_str(KEY_PASS, pass, pass_sz);
    if (err != ESP_OK) pass[0] = '\0';  // open network is legal
    return true;
}

// ---------- AP mode ----------

static void build_ap_ssid(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(out, out_sz, "%s%02X%02X", PV_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "starting AP + captive portal");
    ESP_ERROR_CHECK(esp_wifi_stop());

    wifi_config_t ap = {0};
    build_ap_ssid((char *)ap.ap.ssid, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, PV_WIFI_AP_PASSWORD, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len       = strlen((const char *)ap.ap.ssid);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = PV_WIFI_STATE_AP_PORTAL;
    ESP_LOGI(TAG, "AP SSID=%s password=%s", ap.ap.ssid, ap.ap.password);
    // Portal is started by app_main after pv_wifi_start returns.
}

// ---------- STA mode ----------

static void start_sta_mode(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "connecting to %s", ssid);

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    s_state = PV_WIFI_STATE_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------- Event handlers ----------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == PV_WIFI_STATE_STA_CONNECTING || s_state == PV_WIFI_STATE_STA_CONNECTED) {
            if (s_retry < STA_MAX_RETRIES) {
                s_retry++;
                ESP_LOGW(TAG, "STA disconnect; retry %d/%d", s_retry, STA_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA gave up; falling back to portal");
                xEventGroupSetBits(s_events, BIT_FAILED);
            }
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        s_state = PV_WIFI_STATE_STA_CONNECTED;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

// ---------- Public API ----------

esp_err_t pv_wifi_start(void)
{
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (err != ESP_OK) {
        return err;
    }

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_saved_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta_mode(ssid, pass);
        // Wait briefly for a decision; if STA fails, we'll switch to AP.
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(20000));
        if (bits & BIT_FAILED) start_ap_mode();
    } else {
        ESP_LOGI(TAG, "no saved WiFi credentials");
        start_ap_mode();
    }
    return ESP_OK;
}

esp_err_t pv_wifi_save_creds_and_reboot(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "saving creds for SSID=%s; rebooting", ssid);
    esp_err_t err = nvs_write_str(KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_write_str(KEY_PASS, password ? password : "");
    if (err != ESP_OK) return err;

    // Give the HTTP response a moment to flush before we reboot.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

esp_err_t pv_wifi_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

pv_wifi_state_t pv_wifi_state(void) { return s_state; }
