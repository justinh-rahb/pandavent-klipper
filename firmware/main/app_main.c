#include "pv_board.h"
#include "pv_moonraker.h"
#include "pv_motor.h"
#include "pv_policy.h"
#include "pv_portal.h"
#include "pv_wifi.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv";

// TODO: read PV_ADC_CONFIG_DETECT_CH at boot and pick 0/2/4 here. For now,
// hardcode the retail-kit value and confirm empirically once hardware lands.
#define BOOT_ACTIVE_MOTOR_GROUPS 4

// #define PV_MOTOR_SMOKE_TEST 1   // uncomment to cycle group 0 open/closed

void app_main(void)
{
    ESP_LOGI(TAG, "PandaVent-Klipper booting");

    gpio_config_t btn_led = {
        .pin_bit_mask = 1ULL << PV_PIN_USER_BUTTON,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_led);

    ESP_ERROR_CHECK(pv_motor_init(BOOT_ACTIVE_MOTOR_GROUPS));
    ESP_ERROR_CHECK(pv_wifi_start());
    ESP_ERROR_CHECK(pv_moonraker_start());
    ESP_ERROR_CHECK(pv_policy_start());
    ESP_ERROR_CHECK(pv_portal_start());

#ifdef PV_MOTOR_SMOKE_TEST
    // ⚠ DEV-ONLY: cycle group 0 open → closed every 5 s. Disable before
    // flashing to a vent that's mounted in a printer.
    pv_motor_target_t target = PV_MOTOR_TARGET_OPEN;
    TickType_t last_switch = xTaskGetTickCount();
#endif

    bool led_on = false;
    for (;;) {
        gpio_set_level(PV_PIN_USER_BUTTON, led_on);
        led_on = !led_on;

#ifdef PV_MOTOR_SMOKE_TEST
        if (xTaskGetTickCount() - last_switch > pdMS_TO_TICKS(5000)) {
            target = (target == PV_MOTOR_TARGET_OPEN) ? PV_MOTOR_TARGET_CLOSED
                                                     : PV_MOTOR_TARGET_OPEN;
            pv_motor_set_target(0, target);
            ESP_LOGI(TAG, "grp 0 -> %s",
                     target == PV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED");
            last_switch = xTaskGetTickCount();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
