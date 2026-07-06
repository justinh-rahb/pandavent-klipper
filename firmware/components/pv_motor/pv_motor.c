#include "pv_motor.h"
#include "pv_board.h"

#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "pv_motor";

// PWM configuration — matches stock firmware.
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_RES_BITS       LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ        30000
#define DUTY_MAX            ((1u << 10) - 1u)   // 1023
#define DUTY_KICK           0x66u               // ~10% initial kick before fade
#define FADE_UP_MS          20                  // 0→full over 20 ms
#define FADE_DOWN_MS        10                  // full→0 over 10 ms
#define DEAD_TIME_MS        500                 // wait between direction reversals
#define VERIFY_TIMEOUT_MS   200                 // hall must confirm within this window
#define RETRY_PAUSE_MS      50                  // brief stop between retries
#define MAX_RETRIES         4
#define TICK_MS             10                  // task loop period

// Hall ADC thresholds — from motor_adc.c decompilation.
#define HALL_CLOSED_LO      0x280
#define HALL_CLOSED_HI      0x3c0
#define HALL_OPEN_LO        0x550
#define HALL_OPEN_HI        0x690
#define HALL_MID_LOW_TEST   0x173
#define HALL_MID_OFFSET     (-2080)             // signed; 0xfffff7e0 as int32

typedef enum {
    DIR_NONE = 0,
    DIR_FWD,
    DIR_REV,
} dir_t;

typedef struct {
    pv_motor_target_t target;      // last commanded target (mutated by API)
    pv_motor_target_t applied;     // target the state machine is currently driving toward
    dir_t             dir;         // channel currently energised
    bool              running;
    int               retries;
    TickType_t        drive_started_tick;
    pv_motor_hall_t   hall_cached;
} group_state_t;

static int              s_active_groups = 0;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static group_state_t    s_groups[PV_MOTOR_GROUP_COUNT];
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t     s_task = NULL;

// ---------- helpers ----------

static inline ledc_channel_t chan_of(int g, dir_t d)
{
    return d == DIR_FWD ? PV_MOTOR_GROUPS[g].fwd_ledc_ch
                        : PV_MOTOR_GROUPS[g].rev_ledc_ch;
}

static inline dir_t dir_for_target(pv_motor_target_t t)
{
    if (t == PV_MOTOR_TARGET_OPEN)   return DIR_FWD;
    if (t == PV_MOTOR_TARGET_CLOSED) return DIR_REV;
    return DIR_NONE;
}

static inline pv_motor_hall_t hall_for_target(pv_motor_target_t t)
{
    if (t == PV_MOTOR_TARGET_OPEN)   return PV_HALL_OPEN;
    if (t == PV_MOTOR_TARGET_CLOSED) return PV_HALL_CLOSED;
    return PV_HALL_INVALID;
}

static pv_motor_hall_t classify_hall(int raw)
{
    if (raw == 0) return PV_HALL_INVALID;
    if (raw >= HALL_OPEN_LO   && raw < HALL_OPEN_HI)   return PV_HALL_OPEN;
    if (raw >= HALL_CLOSED_LO && raw < HALL_CLOSED_HI) return PV_HALL_CLOSED;
    // Stock: `(raw + -2080) < 0x173` → treat as unsigned wrap to pick low mid.
    if ((uint32_t)(raw + HALL_MID_OFFSET) < HALL_MID_LOW_TEST) return PV_HALL_MID_LOW;
    return PV_HALL_MID_HIGH;
}

static pv_motor_hall_t read_hall(int g)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, PV_MOTOR_GROUPS[g].hall_adc_ch, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hall read grp=%d failed: %s", g, esp_err_to_name(err));
        return PV_HALL_INVALID;
    }
    return classify_hall(raw);
}

// Cut both channels immediately (used for stop / init / dead-time entry).
static void hard_off_group(int g)
{
    ledc_set_duty(LEDC_MODE, PV_MOTOR_GROUPS[g].fwd_ledc_ch, 0);
    ledc_set_duty(LEDC_MODE, PV_MOTOR_GROUPS[g].rev_ledc_ch, 0);
    ledc_update_duty(LEDC_MODE, PV_MOTOR_GROUPS[g].fwd_ledc_ch);
    ledc_update_duty(LEDC_MODE, PV_MOTOR_GROUPS[g].rev_ledc_ch);
}

// Kick + fade the given channel from ~10% up to full duty. Caller is expected
// to have zeroed the opposite channel and observed the dead-time.
static void start_drive(int g, dir_t d)
{
    ledc_channel_t ch = chan_of(g, d);
    ledc_set_duty(LEDC_MODE, ch, DUTY_KICK);
    ledc_update_duty(LEDC_MODE, ch);
    ledc_set_fade_with_time(LEDC_MODE, ch, DUTY_MAX, FADE_UP_MS);
    ledc_fade_start(LEDC_MODE, ch, LEDC_FADE_NO_WAIT);
}

// Fade whichever channel is currently active down to 0, then hard-off both.
static void stop_drive(int g)
{
    group_state_t *st = &s_groups[g];
    if (st->dir != DIR_NONE) {
        ledc_channel_t ch = chan_of(g, st->dir);
        if (ledc_get_duty(LEDC_MODE, ch) != 0) {
            ledc_set_fade_with_time(LEDC_MODE, ch, 0, FADE_DOWN_MS);
            ledc_fade_start(LEDC_MODE, ch, LEDC_FADE_NO_WAIT);
            vTaskDelay(pdMS_TO_TICKS(FADE_DOWN_MS + 2));
        }
    }
    hard_off_group(g);
    st->dir = DIR_NONE;
    st->running = false;
}

// Enter drive state toward the target. Handles dead-time if reversing.
static void begin_drive_toward(int g, pv_motor_target_t target)
{
    group_state_t *st = &s_groups[g];
    dir_t new_dir = dir_for_target(target);
    if (new_dir == DIR_NONE) {
        stop_drive(g);
        return;
    }

    // Reversal: fully stop first + observe dead-time so we don't shoot through
    // the H-bridge or slam the mechanism.
    if (st->dir != DIR_NONE && st->dir != new_dir) {
        stop_drive(g);
        vTaskDelay(pdMS_TO_TICKS(DEAD_TIME_MS));
    } else if (st->dir == DIR_NONE) {
        // Cold start: still zero the opposite channel just in case.
        ledc_set_duty(LEDC_MODE, chan_of(g, new_dir == DIR_FWD ? DIR_REV : DIR_FWD), 0);
        ledc_update_duty(LEDC_MODE, chan_of(g, new_dir == DIR_FWD ? DIR_REV : DIR_FWD));
    }

    start_drive(g, new_dir);
    st->dir = new_dir;
    st->applied = target;
    st->running = true;
    st->drive_started_tick = xTaskGetTickCount();
}

// ---------- state machine tick (per group) ----------

static void tick_group(int g)
{
    group_state_t *st = &s_groups[g];

    // Snapshot the API-visible target under lock.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pv_motor_target_t want = st->target;
    xSemaphoreGive(s_lock);

    pv_motor_hall_t hall = read_hall(g);
    st->hall_cached = hall;

    // Stop requested.
    if (want == PV_MOTOR_TARGET_STOP) {
        if (st->running) stop_drive(g);
        st->applied = PV_MOTOR_TARGET_STOP;
        st->retries = 0;
        return;
    }

    // Already at target — nothing to do.
    if (hall == hall_for_target(want)) {
        if (st->running) stop_drive(g);
        st->applied = want;
        st->retries = 0;
        return;
    }

    // Target changed since last drive → restart in the new direction.
    if (!st->running || st->applied != want) {
        st->retries = 0;
        begin_drive_toward(g, want);
        return;
    }

    // Already driving; give the motor time to reach the target.
    TickType_t elapsed = xTaskGetTickCount() - st->drive_started_tick;
    if (elapsed < pdMS_TO_TICKS(VERIFY_TIMEOUT_MS)) return;

    // Timed out without hitting the endpoint — retry or give up.
    if (st->retries < MAX_RETRIES) {
        st->retries++;
        ESP_LOGW(TAG, "grp=%d stalled; retry %d/%d", g, st->retries, MAX_RETRIES);
        stop_drive(g);
        vTaskDelay(pdMS_TO_TICKS(RETRY_PAUSE_MS));
        begin_drive_toward(g, want);
    } else {
        ESP_LOGE(TAG, "grp=%d gave up after %d retries (hall=%d)", g, MAX_RETRIES, hall);
        stop_drive(g);
        // Leave target intact — the caller can decide to re-issue.
    }
}

static void motor_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int g = 0; g < s_active_groups; ++g) tick_group(g);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ---------- init ----------

static esp_err_t configure_ledc(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RES_BITS,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer_config");

    for (int g = 0; g < s_active_groups; ++g) {
        const pv_motor_group_t *m = &PV_MOTOR_GROUPS[g];
        ledc_channel_config_t fwd = {
            .gpio_num   = m->fwd_gpio,
            .speed_mode = LEDC_MODE,
            .channel    = m->fwd_ledc_ch,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        ledc_channel_config_t rev = fwd;
        rev.gpio_num = m->rev_gpio;
        rev.channel  = m->rev_ledc_ch;
        ESP_RETURN_ON_ERROR(ledc_channel_config(&fwd), TAG, "ledc fwd grp=%d", g);
        ESP_RETURN_ON_ERROR(ledc_channel_config(&rev), TAG, "ledc rev grp=%d", g);
    }

    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "ledc_fade_func_install");
    return ESP_OK;
}

static esp_err_t configure_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc_handle), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,   // full-scale ~3.3 V — hall thresholds assume this
    };
    for (int g = 0; g < s_active_groups; ++g) {
        ESP_RETURN_ON_ERROR(
            adc_oneshot_config_channel(s_adc_handle, PV_MOTOR_GROUPS[g].hall_adc_ch, &chan),
            TAG, "adc chan grp=%d", g);
    }
    return ESP_OK;
}

esp_err_t pv_motor_init(int active_groups)
{
    if (active_groups < 0 || active_groups > PV_MOTOR_GROUP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    s_active_groups = active_groups;
    memset(s_groups, 0, sizeof(s_groups));
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (active_groups == 0) {
        ESP_LOGI(TAG, "no active motor groups; driver idle");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(configure_ledc(), TAG, "ledc");
    ESP_RETURN_ON_ERROR(configure_adc(),  TAG, "adc");

    BaseType_t ok = xTaskCreate(motor_task, "pv_motor", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized with %d active group(s)", active_groups);
    return ESP_OK;
}

esp_err_t pv_motor_set_target(int group, pv_motor_target_t target)
{
    if (group < 0 || group >= s_active_groups) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_groups[group].target = target;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

pv_motor_hall_t pv_motor_hall(int group)
{
    if (group < 0 || group >= s_active_groups) return PV_HALL_INVALID;
    return s_groups[group].hall_cached;
}

bool pv_motor_is_running(int group)
{
    if (group < 0 || group >= s_active_groups) return false;
    return s_groups[group].running;
}
