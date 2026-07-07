#pragma once

// OpenVent config web UI. Serves the same page in both AP and STA modes so
// initial setup and later reconfiguration use the same URL. In AP mode it
// also runs a DNS redirector so mobile OSes pop the "Sign in to network"
// prompt automatically.
//
// Call after pv_wifi_start() (and ideally after pv_moonraker_start() and
// pv_policy_start(), so pre-fill values are correct). The portal decides
// AP vs. STA behavior from pv_wifi_state().

#include "esp_err.h"

esp_err_t pv_portal_start(void);
esp_err_t pv_portal_stop(void);
