#pragma once

// User-button LED (GPIO 27, active-high). Matches stock firmware's mode
// indicator: off in auto mode, blinking in manual mode. The SOLID mode is
// exposed for future callers (e.g. a "device is on / captive portal up"
// hint) but the vent policy itself only uses OFF and BLINK.

#include "esp_err.h"

typedef enum {
    PV_STATUS_LED_OFF,
    PV_STATUS_LED_SOLID,
    PV_STATUS_LED_BLINK,
} pv_status_led_mode_t;

esp_err_t pv_status_led_start(void);
void      pv_status_led_set(pv_status_led_mode_t mode);
