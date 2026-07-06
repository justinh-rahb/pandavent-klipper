#pragma once

// Panda Vent WiFi manager: reads saved credentials from NVS at boot, connects
// as STA, and falls back to AP + captive portal if either the credentials are
// missing or the connection fails. Cred storage is compatible with the stock
// firmware's NVS layout (namespace "app_nvs", keys "ssid" / "password").

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PV_WIFI_STATE_INIT,
    PV_WIFI_STATE_STA_CONNECTING,
    PV_WIFI_STATE_STA_CONNECTED,
    PV_WIFI_STATE_AP_PORTAL,   // hosting captive portal for setup
} pv_wifi_state_t;

// Start the WiFi manager. Non-blocking; state transitions happen async.
esp_err_t pv_wifi_start(void);

// Persist WiFi credentials and reboot into STA mode. Intended to be called
// from the captive-portal HTTP handler after the user submits the form.
esp_err_t pv_wifi_save_creds_and_reboot(const char *ssid, const char *password);

// Wipe saved WiFi credentials.
esp_err_t pv_wifi_clear_creds(void);

pv_wifi_state_t pv_wifi_state(void);

// AP details, useful for portal DNS/HTTP setup.
#define PV_WIFI_AP_PASSWORD    "987654321"   // matches stock firmware
#define PV_WIFI_AP_SSID_PREFIX "Panda_Vent_"
#define PV_WIFI_HOSTNAME       "PandaVent"    // resolves as PandaVent.local via mDNS
