#pragma once

// Vent policy: consumes Moonraker status, commands the motor driver. Owns the
// AUTO/MANUAL mode toggle and the temperature hysteresis for auto decisions.
//
// AUTO mode:
//   OPEN when the printer is printing OR the bed is above the "hot" threshold
//   CLOSED when the printer is idle AND the bed is below the "cold" threshold
//   (between = keep current target — hysteresis)
//
// MANUAL mode:
//   target = whatever pv_policy_set_manual_target set most recently

#include <stdbool.h>
#include "esp_err.h"
#include "pv_motor.h"

typedef enum {
    PV_POLICY_MODE_AUTO,
    PV_POLICY_MODE_MANUAL,
} pv_policy_mode_t;

esp_err_t pv_policy_start(void);

esp_err_t pv_policy_set_mode(pv_policy_mode_t mode);   // persisted to NVS
pv_policy_mode_t pv_policy_get_mode(void);

esp_err_t pv_policy_set_manual_target(pv_motor_target_t t);
pv_motor_target_t pv_policy_get_target(void);          // whatever we're commanding

esp_err_t pv_policy_clear(void);   // wipe persisted mode
