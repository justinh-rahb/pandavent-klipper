#include "hw_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv";

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

    // Heartbeat on the button LED — confirms we're the running firmware and
    // that GPIO12 wiring matches our expectations. Any physical-button reads
    // will need this reconfigured to INPUT (or shared via press-sampling).
    bool on = false;
    while (true) {
        gpio_set_level(PV_PIN_USER_BUTTON, on);
        on = !on;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
