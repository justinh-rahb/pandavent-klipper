#include "pv_board.h"
#include "pv_button.h"
#include "pv_moonraker.h"
#include "pv_motor.h"
#include "pv_policy.h"
#include "pv_portal.h"
#include "pv_wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv";

// TODO: read PV_ADC_CONFIG_DETECT_CH at boot and pick 0/2/4 here. For now,
// hardcode the retail-kit value and confirm empirically once hardware lands.
#define BOOT_ACTIVE_MOTOR_GROUPS 4

static pv_motor_target_t flip(pv_motor_target_t t)
{
    return (t == PV_MOTOR_TARGET_OPEN) ? PV_MOTOR_TARGET_CLOSED : PV_MOTOR_TARGET_OPEN;
}

// Button semantics from the stock firmware user manual:
//   USER short click, AUTO   → switch to MANUAL and reverse the vent state
//   USER short click, MANUAL → toggle the vent state
//   USER long press          → toggle AUTO ↔ MANUAL
//   BOOT long press          → factory reset (wipe NVS, reboot)
static void on_button(pv_button_id_t id, pv_button_event_t ev)
{
    if (id == PV_BUTTON_USER && ev == PV_BUTTON_SHORT) {
        pv_motor_target_t next = flip(pv_policy_get_target());
        pv_policy_set_manual_target(next);
        pv_policy_set_mode(PV_POLICY_MODE_MANUAL);
        ESP_LOGI(TAG, "USER short: MANUAL, target=%d", next);
        return;
    }
    if (id == PV_BUTTON_USER && ev == PV_BUTTON_LONG) {
        pv_policy_mode_t next = (pv_policy_get_mode() == PV_POLICY_MODE_AUTO)
                                    ? PV_POLICY_MODE_MANUAL : PV_POLICY_MODE_AUTO;
        pv_policy_set_mode(next);
        ESP_LOGI(TAG, "USER long: mode=%s", next == PV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL");
        return;
    }
    if (id == PV_BUTTON_BOOT && ev == PV_BUTTON_LONG) {
        ESP_LOGW(TAG, "BOOT long: factory reset");
        pv_wifi_clear_creds();
        pv_moonraker_clear_config();
        pv_policy_clear();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PandaVent-Klipper booting");

    ESP_ERROR_CHECK(pv_motor_init(BOOT_ACTIVE_MOTOR_GROUPS));
    ESP_ERROR_CHECK(pv_wifi_start());
    ESP_ERROR_CHECK(pv_moonraker_start());
    ESP_ERROR_CHECK(pv_policy_start());
    ESP_ERROR_CHECK(pv_portal_start());
    ESP_ERROR_CHECK(pv_button_start(on_button));

    // Everything runs off its own task; nothing left to do on the main one.
    vTaskDelete(NULL);
}
