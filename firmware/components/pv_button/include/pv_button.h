#pragma once

// User (GPIO 12) + BOOT (GPIO 0) button handler. Both are active-low inputs
// with internal pull-ups. Debounce is 10 ms; long-press threshold is 3 s to
// match stock v1.0.0 (the only released version) — see the note in
// pv_button.c on the wiki-vs-binary discrepancy.

#include "esp_err.h"

typedef enum {
    PV_BUTTON_USER,
    PV_BUTTON_BOOT,
} pv_button_id_t;

typedef enum {
    PV_BUTTON_SHORT,      // pressed and released before the long-press threshold
    PV_BUTTON_LONG,       // held past the threshold (fires once, at the threshold)
} pv_button_event_t;

typedef void (*pv_button_cb_t)(pv_button_id_t id, pv_button_event_t ev);

// Register a single callback that receives every button event, then spawn the
// polling task. The callback runs on the button task; keep it short and
// non-blocking.
esp_err_t pv_button_start(pv_button_cb_t cb);
