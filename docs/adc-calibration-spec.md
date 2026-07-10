# Stock Panda Vent ADC calibration specification

This documents the ADC path in the stock Panda Vent `v1.0.0` image under
`analysis/segments/`. Addresses are Xtensa load addresses from that image.
The application symbols are stripped, so descriptive names below come from
the source-path, function-name, and ESP-IDF API strings referenced by each
function.

## Reproduction contract

Stock creates one **ADC1 oneshot** unit and one **line-fitting calibration**
handle, once at boot. The equivalent ESP-IDF setup is:

```c
adc_oneshot_unit_handle_t hall_adc_handle;
adc_cali_handle_t hall_adc_cali;

adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = ADC_UNIT_1,                 // numeric 0
    .clk_src = ADC_RTC_CLK_SRC_DEFAULT,    // numeric 8 in this binary
    .ulp_mode = ADC_ULP_MODE_DISABLE,      // numeric 0
};
ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &hall_adc_handle));

adc_cali_line_fitting_config_t cali_cfg = {
    .unit_id = ADC_UNIT_1,                 // numeric 0
    .atten = ADC_ATTEN_DB_12,              // numeric 3; old name DB_11
    .bitwidth = ADC_BITWIDTH_12,           // numeric 12, not DEFAULT
    .default_vref = 0,
};
adc_cali_create_scheme_line_fitting(&cali_cfg, &hall_adc_cali);

adc_oneshot_chan_cfg_t chan_cfg = {
    .atten = ADC_ATTEN_DB_12,              // numeric 3
    .bitwidth = ADC_BITWIDTH_12,           // numeric 12
};
```

`hall_adc_init` (`0x400dea18`) performs those operations in that order, then
calls `adc_oneshot_config_channel` for ADC1 channels **2, 1, 0, 3, and 7**, in
that order. Channels 0-3 are the four hall sensors; channel 7 is config detect.
There is no ADC continuous-mode setup.

The calibration call is made through the small wrapper at `0x400de8d8`:

```c
bool create_calibration(adc_unit_t unit, adc_cali_handle_t *out)
{
    adc_cali_line_fitting_config_t cfg = {0};
    cfg.unit_id = unit;
    cfg.atten = 3;
    cfg.bitwidth = 12;
    return adc_cali_create_scheme_line_fitting(&cfg, out) == ESP_OK;
}
```

The wrapper is called as `create_calibration(0, &global_cali_handle)`. Its
return value is ignored. Thus stock does not implement a raw-count fallback
if calibration creation fails. With `default_vref = 0`, the ESP-IDF routine
also returns an error when neither supported eFuse calibration source exists.
In the raw decompile, `DAT_400d0dcc` resolves to the calibration-handle
storage at `0x3ffb696c`; it is not an ADC-unit selector. `PTR_DAT_400d0df4`
resolves to the channel-config constant at `0x3f4170f8`.

Evidence:

- `hall_adc_init`: `0x400dea18`
- calibration wrapper: `0x400de8d8`
- `adc_oneshot_new_unit`: `0x4011507c`
- `adc_oneshot_config_channel`: `0x40115220`
- `adc_cali_create_scheme_line_fitting`: `0x40115718`; its name string is at
  `0x3f42a934`
- the shared channel initializer at `0x3f4170f8` is the pair `{3, 12}`

No curve-fitting symbol or call is present.

## Boot order

ESP-IDF's `main_task` at `0x4018df10` invokes the stock application entry at
`0x400d8be0`. The relevant application order is:

1. Initialize in-memory UI/RGB/WiFi/MQTT defaults. The RGB step here,
   `FUN_400dc720`, only allocates and fills state; it does not configure RMT.
2. Call `hall_adc_init` (`0x400dea18`), synchronously completing ADC1 oneshot
   creation, line-fitting creation, and all five channel configurations.
3. Load or initialize NVS settings (`FUN_400d8d50`).
4. Create the NVS, WiFi, MQTT, UDP, web, and DNS worker tasks.
5. Create the motor task (`0x400de55c`, task name `motor`).
6. Create the RGB task (`0x400dcab8`, task name `rgb`).
7. Create the idle/button task, then return from the application entry.

Consequences for the requested subsystem ordering:

- **WiFi/MQTT:** calibration is complete before either worker task is
  created, so network bring-up cannot precede it.
- **LEDC:** the motor task is created after calibration. At task entry it
  calls `motor_pwm_init` (`0x400de2b4`) for all four groups, then
  `motor_ledc_timer_init` (`0x400deae8`), then enters its control loop.
- **RMT/WS2812:** the RGB task is created after calibration (and after the
  motor task). At task entry it calls `rgb_init` (`0x400dc46c`), which creates
  and enables the two RMT TX channels, before entering the RGB loop.
- **Main loops:** both the motor and RGB loops begin only after their local
  LEDC/RMT initialization, and neither task exists when calibration runs.

The motor task is created before the RGB task, but FreeRTOS scheduling means
the relative wall-clock execution of their LEDC and RMT initializers should
not be inferred from creation order. The guaranteed relationship is that the
ADC/calibration call has already returned before either task is created.

## Lifetime and use of the calibration handle

Calibration is **once per boot**:

- `hall_adc_init` has one caller, the application entry at `0x400d8be0`.
- The line-fitting wrapper has one caller, `hall_adc_init`.
- `adc_cali_create_scheme_line_fitting` has one application call site.
- There is no delete-scheme symbol, temperature-drift path, hot-plug
  recalibration, or error-recovery recreation path in the image.

The global handle is not an inline/cached fit. Every sample follows:

```c
adc_oneshot_read(hall_adc_handle, channel, &raw);
adc_cali_raw_to_voltage(hall_adc_cali, raw, &mv);
```

`hall_get_state` (`0x400deb24`) does this for every hall read. The config
reader (`0x400de904`) does the same for every channel-7 read. These are the
only two application callers of `adc_cali_raw_to_voltage` (`0x40115404`; name
string at `0x3f42a91c`). Both call sites ignore the conversion function's
return value.

## Hall classifier

`hall_get_state` compares the calibrated integer millivolts using unsigned
range tests. The ranges below are inclusive at both ends:

| Return | Meaning | Exact test | Calibrated range |
|---:|---|---|---:|
| 0 | invalid/disconnected | `mv == 0` | exactly 0 mV |
| 2 | closed | `(uint32_t)(mv - 0x550) < 0x141` | 1360-1680 mV |
| 1 | open | `(uint32_t)(mv - 0x280) < 0x141` | 640-960 mV |
| 3 | past-closed/over-travel | `(uint32_t)(mv - 2080) < 0x173` | 2080-2450 mV |
| 4 | in transit/catch-all | none of the above | every other nonzero value |

The test order is closed, open, past-closed, catch-all. In particular, the
upper bounds are **1680, 960, and 2450 inclusive**; translating the constants
as half-open `mv < 1680`, `mv < 960`, or `mv < 2450` is an off-by-one mismatch.
The `-2080` constant is stored at `0x400d0e28` as `0xfffff7e0`.

## Config-detect classifier

`FUN_400deb88` reads ADC1 channel 7 through `FUN_400de904`, so these tests are
also against calibrated millivolts:

| Calibrated channel-7 range | Active motor groups | RGB LED count written |
|---:|---:|---:|
| 1900-2400 mV inclusive | 4 | 27 |
| 1100-1700 mV inclusive | 2 | 16 |
| 0-200 mV inclusive | 0 | 0 |

The exact tests are `(uint32_t)(mv - 1900) < 501`, then
`(uint32_t)(mv - 1100) < 601`, then `mv < 201`. Values in the gaps
201-1099, 1701-1899, or above 2400 do not select a fourth state: the function
leaves the existing group/strip configuration unchanged and returns it. This
is hysteresis-by-hold, not a catch-all band.

## ADC2 and the WS2812 failure

The application ADC call graph contains one `adc_oneshot_new_unit` call, for
numeric unit 0 (`ADC_UNIT_1`), and one set of channel configurations, for
ADC1 channels 0, 1, 2, 3, and 7. There is no application call that creates or
configures ADC2, and no ADC continuous-mode call.

The two RGB table entries select GPIO 14 and GPIO 4. `rgb_init` passes those
GPIO numbers to `rmt_new_tx_channel`, creates an encoder, and enables each RMT
channel. It does not call an ADC API. Conversely, the decompiled
`adc_cali_create_scheme_line_fitting` implementation validates the config,
allocates its handle/context, reads eFuse calibration data, and computes fit
coefficients. It does not install an interrupt handler, configure/hold a GPIO,
or call RMT. Generic ESP-IDF ADC driver strings mention ADC2 locks because the
linked driver supports both units; there is no application xref selecting
ADC2.

Therefore the stock image provides no evidence that line-fitting calibration
itself intentionally touches GPIO 4 or GPIO 14. The v0.2.4 latch-red and later
hang remain unexplained by this binary alone. The strongest actionable
difference is initialization order: stock finishes ADC oneshot and calibration
before the RGB task/RMT channels exist, while OpenVent v0.2.4 configured its
LEDC timer before creating the ADC unit/calibration. Stock also passes explicit
clock source 8 and explicit 12-bit width, whereas v0.2.4 left those fields at
their default values. Those differences are facts; claiming any one caused
the hardware failure would require a controlled hardware reproduction.

For stock parity, preserve the stock invariant: initialize ADC1 oneshot,
create line fitting, and configure channels before creating/enabling the motor
LEDC and RGB RMT tasks.
