#pragma once

// Panda Vent hardware pin map.
// Recovered from stock firmware (panda_vent_v1.0.0.bin) via Ghidra + BTT docs.
// See docs/HARDWARE_ANALYSIS.md for evidence.

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal/adc_types.h"

// Buttons
#define PV_PIN_USER_BUTTON      GPIO_NUM_12   // illuminated user button (input + LED)
#define PV_PIN_BOOT_BUTTON      GPIO_NUM_0    // BOOT / factory reset

// LED-strip auto-detect (ADC divider that reads high when the 2nd strip is populated)
#define PV_ADC_STRIP_DETECT_CH  ADC_CHANNEL_7  // = GPIO 35

// WS2812 outputs. rgb_channels[1] is only enabled when strip-detect trips.
#define PV_PIN_RGB_STRIP_0      GPIO_NUM_14
#define PV_PIN_RGB_STRIP_1      GPIO_NUM_4

// Motor groups. Stock firmware defines four; the physical vent we're targeting
// presumably wires up a subset. Confirm empirically before energising anything.
typedef struct {
    gpio_num_t     hall_gpio;       // reference only — hall reads via ADC, not GPIO
    adc_channel_t  hall_adc_ch;     // ADC1 channel used for the hall sensor
    ledc_channel_t fwd_ledc_ch;
    gpio_num_t     fwd_gpio;
    ledc_channel_t rev_ledc_ch;
    gpio_num_t     rev_gpio;
} pv_motor_group_t;

#define PV_MOTOR_GROUP_COUNT 4

static const pv_motor_group_t PV_MOTOR_GROUPS[PV_MOTOR_GROUP_COUNT] = {
    { GPIO_NUM_38, ADC_CHANNEL_2, LEDC_CHANNEL_4, GPIO_NUM_22, LEDC_CHANNEL_5, GPIO_NUM_21 },
    { GPIO_NUM_36, ADC_CHANNEL_0, LEDC_CHANNEL_0, GPIO_NUM_25, LEDC_CHANNEL_1, GPIO_NUM_26 },
    { GPIO_NUM_37, ADC_CHANNEL_1, LEDC_CHANNEL_2, GPIO_NUM_32, LEDC_CHANNEL_3, GPIO_NUM_33 },
    { GPIO_NUM_39, ADC_CHANNEL_3, LEDC_CHANNEL_6, GPIO_NUM_23, LEDC_CHANNEL_7, GPIO_NUM_19 },
};
