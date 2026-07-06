#pragma once

// Panda Vent hardware pin map.
// Recovered from stock firmware (panda_vent_v1.0.0.bin) via Ghidra + BTT docs.
// See docs/HARDWARE_ANALYSIS.md for evidence.

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal/adc_types.h"

// Buttons. Stock confirms both are inputs with internal pull-ups (active-low).
// GPIO 12 is a strapping pin (MTDI, picks flash voltage at boot); it must
// stay LOW during reset — the internal pull-up only engages after our code
// runs, so this is fine.
#define PV_PIN_USER_BUTTON      GPIO_NUM_12
#define PV_PIN_BOOT_BUTTON      GPIO_NUM_0

// User button LED — separate from the switch. Active-high output. Stock uses
// steady = auto mode, blink = manual mode.
#define PV_PIN_USER_BUTTON_LED  GPIO_NUM_27

// Hardware-config auto-detect. Single ADC line whose raw value picks between
// three kit configurations (see docs/HARDWARE_ANALYSIS.md):
//   raw ~1900-2400 → 4 motors + 2 RGB strips + 27 LEDs (retail 2-vent kit)
//   raw ~1100-1700 → 2 motors + 1 RGB strip + 16 LEDs (single-vent kit)
//   raw <200       → nothing connected
#define PV_ADC_CONFIG_DETECT_CH  ADC_CHANNEL_7  // = GPIO 35

// WS2812 outputs. Strip 1 is only enabled in the 2-strip config.
#define PV_PIN_RGB_STRIP_0      GPIO_NUM_14
#define PV_PIN_RGB_STRIP_1      GPIO_NUM_4

// Motor groups. All four are populated in the retail kit (2 vent units × 2
// motors each). The single-vent kit uses only groups 0/1 or 2/3 depending on
// which mainboard connector is used — pairing not yet verified.
typedef struct {
    adc_channel_t  hall_adc_ch;     // ADC1 channel for the hall sensor
    ledc_channel_t fwd_ledc_ch;
    gpio_num_t     fwd_gpio;
    ledc_channel_t rev_ledc_ch;
    gpio_num_t     rev_gpio;
} pv_motor_group_t;

#define PV_MOTOR_GROUP_COUNT 4

static const pv_motor_group_t PV_MOTOR_GROUPS[PV_MOTOR_GROUP_COUNT] = {
    { ADC_CHANNEL_2, LEDC_CHANNEL_4, GPIO_NUM_22, LEDC_CHANNEL_5, GPIO_NUM_21 },
    { ADC_CHANNEL_0, LEDC_CHANNEL_0, GPIO_NUM_25, LEDC_CHANNEL_1, GPIO_NUM_26 },
    { ADC_CHANNEL_1, LEDC_CHANNEL_2, GPIO_NUM_32, LEDC_CHANNEL_3, GPIO_NUM_33 },
    { ADC_CHANNEL_3, LEDC_CHANNEL_6, GPIO_NUM_23, LEDC_CHANNEL_7, GPIO_NUM_19 },
};
