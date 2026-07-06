#pragma once

// Panda Vent motor driver: LEDC PWM + hall-ADC position feedback for up to 4
// independent motor groups. Public API is thread-safe; a single internal task
// runs the per-group state machine.
//
// Behavior mirrors the stock firmware: 30 kHz PWM, soft-start via LEDC fade,
// 500 ms dead-time between direction reversals, up to 4 retries when the hall
// doesn't confirm the target position.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PV_MOTOR_TARGET_STOP = 0,    // idle, no drive
    PV_MOTOR_TARGET_CLOSED,      // drive rev until hall reads CLOSED
    PV_MOTOR_TARGET_OPEN,        // drive fwd until hall reads OPEN
} pv_motor_target_t;

typedef enum {
    PV_HALL_INVALID  = 0,        // reading discarded (sensor disconnected)
    PV_HALL_CLOSED   = 1,        // at closed endpoint
    PV_HALL_OPEN     = 2,        // at open endpoint
    PV_HALL_MID_LOW  = 3,        // mid-travel, near closed
    PV_HALL_MID_HIGH = 4,        // mid-travel, near open
} pv_motor_hall_t;

// Initialize the LEDC timer/channels + ADC for the first `active_groups` motor
// groups (must be 0, 2, or 4 — matches stock's config-detect result). Spawns
// the driver task.
esp_err_t pv_motor_init(int active_groups);

// Set the target state for one group. group index in [0, active_groups).
esp_err_t pv_motor_set_target(int group, pv_motor_target_t target);

// Get the last hall reading for one group. Non-blocking; may be a stale
// cached value between task ticks.
pv_motor_hall_t pv_motor_hall(int group);

// True while a group is actively driving toward its target.
bool pv_motor_is_running(int group);
