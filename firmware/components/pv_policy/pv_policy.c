#include "pv_policy.h"
#include "pv_moonraker.h"
#include "pv_motor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "pv_policy";

#define NVS_NS       "app_nvs"
#define KEY_MODE     "policy_mode"

// Bed-temperature hysteresis. Open the vent above BED_OPEN_C, close below
// BED_CLOSE_C, hold current state between. Values chosen conservatively so
// residual heat after a print keeps the vent open until the chamber cools.
#define BED_OPEN_C   45.0f
#define BED_CLOSE_C  35.0f
#define TICK_MS      1000

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static pv_policy_mode_t  s_mode = PV_POLICY_MODE_AUTO;
static pv_motor_target_t s_manual_target  = PV_MOTOR_TARGET_CLOSED;
static pv_motor_target_t s_current_target = PV_MOTOR_TARGET_CLOSED;

static void apply_target(pv_motor_target_t t)
{
    int n = pv_motor_active_groups();
    for (int g = 0; g < n; ++g) pv_motor_set_target(g, t);
    s_current_target = t;
}

// AUTO decision. Returns the target we should be driving toward, given the
// current Moonraker snapshot. If we don't have reliable data, keep whatever
// we're already commanding.
static pv_motor_target_t decide_auto_target(const pv_moonraker_status_t *st)
{
    if (st->state != PV_MK_SUBSCRIBED) return s_current_target;

    if (st->printing || st->bed_temp > BED_OPEN_C) return PV_MOTOR_TARGET_OPEN;
    if (!st->printing && st->bed_temp < BED_CLOSE_C) return PV_MOTOR_TARGET_CLOSED;
    return s_current_target;   // hysteresis band
}

static void policy_task(void *arg)
{
    (void)arg;
    for (;;) {
        pv_moonraker_status_t st;
        pv_moonraker_get_status(&st);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        pv_motor_target_t want = (s_mode == PV_POLICY_MODE_MANUAL)
                                     ? s_manual_target
                                     : decide_auto_target(&st);
        if (want != s_current_target) {
            ESP_LOGI(TAG, "target -> %s (mode=%s, printing=%d, bed=%.1f)",
                     want == PV_MOTOR_TARGET_OPEN   ? "OPEN"
                   : want == PV_MOTOR_TARGET_CLOSED ? "CLOSED" : "STOP",
                     s_mode == PV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL",
                     st.printing, st.bed_temp);
            apply_target(want);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ---------- NVS ----------

static void load_mode(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t m = 0;
    if (nvs_get_u8(h, KEY_MODE, &m) == ESP_OK) {
        s_mode = (m == PV_POLICY_MODE_MANUAL) ? PV_POLICY_MODE_MANUAL
                                              : PV_POLICY_MODE_AUTO;
    }
    nvs_close(h);
}

static void save_mode(pv_policy_mode_t m)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_MODE, (uint8_t)m);
    nvs_commit(h);
    nvs_close(h);
}

// ---------- public API ----------

esp_err_t pv_policy_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    load_mode();
    if (xTaskCreate(policy_task, "pv_policy", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started in %s mode", s_mode == PV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

esp_err_t pv_policy_set_mode(pv_policy_mode_t mode)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = mode;
    save_mode(mode);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

pv_policy_mode_t pv_policy_get_mode(void) { return s_mode; }

esp_err_t pv_policy_set_manual_target(pv_motor_target_t t)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_manual_target = t;
    // If we're already in manual mode, apply immediately instead of waiting
    // for the next tick.
    if (s_mode == PV_POLICY_MODE_MANUAL && t != s_current_target) apply_target(t);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

pv_motor_target_t pv_policy_get_target(void) { return s_current_target; }

esp_err_t pv_policy_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_MODE);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
