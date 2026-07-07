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

// Discrete hall-sensor state. Values match stock's classifier so the state
// numbers you see here are the same ones the Ghidra decomp of hall_get_state
// (FUN_400deb24) returns. Physical mapping is derived from stock's fan-on ↔
// state 1 = OPEN, fan-off ↔ state 2 = CLOSED chain in the main state machine
// (FUN_400de55c lines 36-43).
typedef enum {
    PV_HALL_INVALID  = 0,        // sensor disconnected / raw ADC == 0
    PV_HALL_OPEN     = 1,        // at OPEN endpoint   (raw ADC ~640–961)
    PV_HALL_CLOSED   = 2,        // at CLOSED endpoint (raw ADC ~1360–1680)
    PV_HALL_MID_LOW  = 3,        // past-closed / over-travel (raw ADC ~2080–2450)
    PV_HALL_MID_HIGH = 4,        // catch-all — in transit between endpoints
} pv_motor_hall_t;

// Initialize the LEDC timer + ADC1, sample the config-detect ADC (GPIO 35) to
// figure out how many motor groups are currently connected, bring those
// groups online, and spawn the driver task. The task continues to sample the
// detect ADC once per second and dynamically add/remove groups as vent units
// are hot-plugged.
esp_err_t pv_motor_init(void);

// Set the target state for one group. group index in [0, active_groups).
esp_err_t pv_motor_set_target(int group, pv_motor_target_t target);

// Get the last hall reading for one group. Non-blocking; may be a stale
// cached value between task ticks.
pv_motor_hall_t pv_motor_hall(int group);

// True while a group is actively driving toward its target.
bool pv_motor_is_running(int group);

// Currently active group count (0/2/4), as most recently determined by the
// hardware-config detect ADC.
int pv_motor_active_groups(void);
