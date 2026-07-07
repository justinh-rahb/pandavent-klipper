#pragma once

// Moonraker WebSocket client. Reads its config from NVS at boot, connects to
// `ws://<host>:<port>/websocket`, subscribes to print_stats + heater_bed, and
// caches the latest values for the vent policy to consult. Reconnects on
// disconnect. Idle (no-op) if no config is saved.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PV_MK_DISABLED,       // no config saved
    PV_MK_DISCONNECTED,   // config present, not currently connected
    PV_MK_CONNECTING,
    PV_MK_CONNECTED,      // websocket up, subscribe in flight
    PV_MK_SUBSCRIBED,     // receiving status updates
} pv_moonraker_state_t;

typedef struct {
    char     host[64];    // hostname or IP; empty string = unconfigured
    uint16_t port;        // defaults to 7125 if 0
    char     api_key[65]; // optional; empty if unused
} pv_moonraker_config_t;

typedef struct {
    pv_moonraker_state_t state;
    bool                 printing;    // print_stats.state == "printing"
    float                bed_temp;    // heater_bed.temperature (°C)
    float                bed_target;  // heater_bed.target (°C)
} pv_moonraker_status_t;

esp_err_t pv_moonraker_start(void);

// Overwrite the saved config. If the client is running, it reconnects with
// the new settings.
esp_err_t pv_moonraker_set_config(const pv_moonraker_config_t *cfg);

esp_err_t pv_moonraker_get_config(pv_moonraker_config_t *out);
esp_err_t pv_moonraker_get_status(pv_moonraker_status_t *out);

// Wipe saved Moonraker config. Used for factory reset.
esp_err_t pv_moonraker_clear_config(void);

// mDNS discovery of Moonraker services on the LAN (_moonraker._tcp). Results
// land in a small in-memory cache. Same async pattern as pv_wifi_scan_start —
// spawn, wait a couple seconds, then read the cache.
#define PV_MOONRAKER_DISCOVER_MAX 8

typedef struct {
    char     hostname[64];   // e.g. "mainsailos.local" — for display
    char     ip[16];         // dotted-quad IPv4 — what we actually use
    uint16_t port;
} pv_moonraker_service_t;

esp_err_t pv_moonraker_discover_start(void);
bool      pv_moonraker_is_discovering(void);
int       pv_moonraker_get_discovered(pv_moonraker_service_t *out, int max);
