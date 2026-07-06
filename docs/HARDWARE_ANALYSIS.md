# Panda Vent Hardware Analysis

> Reverse-engineered from the stock firmware binary (`panda_vent_v1.0.0.bin`)
> and official BTT documentation.

## ESP32 Module

The firmware binary confirms this is a **classic ESP32 (Xtensa dual-core LX6)** — NOT an S2/S3/C3.

Evidence from binary strings:
- HAL paths: `hal/esp32/include/hal/gpio_ll.h`, `hal/esp32/include/hal/adc_ll.h`
- Toolchain: `xtensa-esp-elf` (not `riscv32-esp-elf`)
- Port code: `port/esp32/rtc_time.c`, `port/soc/esp32/clk.c`
- Built with **ESP-IDF** v5.3.1 (not Arduino), project name `panda_vent`, compiled 2026-05-27

Firmware image segment layout (from `esptool image-info`):

| Segment | Load addr | Length | Region |
|---------|-----------|--------|--------|
| 0 | 0x3f400020 | 0x3a3c8 | DROM (rodata/strings) |
| 1 | 0x3ff80000 | 0x00004 | RTC DRAM |
| 2 | 0x3ffb0000 | 0x04394 | DRAM (initialized data) |
| 3 | 0x40080000 | 0x01880 | IRAM (vector table etc.) |
| 4 | 0x400d0020 | 0xc0304 | IROM (main code) |
| 5 | 0x40081880 | 0x16e18 | IRAM (main code) |

## GPIO Pin Map

### Confirmed (from documentation)

| Function | GPIO | Peripheral | Source |
|----------|------|-----------|--------|
| **User Button (with LED)** | GPIO 12 | GPIO input + LED output | BTT User Manual |
| **BOOT Button** | GPIO 0 | GPIO input (factory reset) | BTT User Manual |

### Confirmed (from Ghidra disassembly — see `analysis/`)

The firmware defines **4 independent motor groups**, each with its own hall
sensor + forward/reverse LEDC PWM pair. Config lives in a `motor_channel_t[4]`
array in DRAM whose per-group struct we dumped from memory:

| Group | Hall ADC ch → GPIO | Fwd LEDC ch → GPIO | Rev LEDC ch → GPIO |
|-------|--------------------|--------------------|--------------------|
| **0** | ADC1_CH2 → GPIO 38 | LEDC ch 4 → GPIO 22 | LEDC ch 5 → GPIO 21 |
| **1** | ADC1_CH0 → GPIO 36 | LEDC ch 0 → GPIO 25 | LEDC ch 1 → GPIO 26 |
| **2** | ADC1_CH1 → GPIO 37 | LEDC ch 2 → GPIO 32 | LEDC ch 3 → GPIO 33 |
| **3** | ADC1_CH3 → GPIO 39 | LEDC ch 6 → GPIO 23 | LEDC ch 7 → GPIO 19 |

WS2812 RGB output is a `rgb_channels[2]` array; the second entry is only
enabled when the strip-count detector on ADC1_CH7 reads high.

| Function | GPIO | Peripheral | Evidence |
|----------|------|-----------|----------|
| **LED Strip Count Detect** | ADC1_CH7 (GPIO 35) | ADC oneshot | `adc_oneshot_config_channel(handle, 7, ...)` in `hall_adc_init` |
| **WS2812 Strip 1** | GPIO 14 | RMT TX | `rgb_channels[0].gpio_num = 14` (dumped from 0x3ffb031c) |
| **WS2812 Strip 2** | GPIO 4  | RMT TX | `rgb_channels[1].gpio_num = 4`  (dumped from 0x3ffb0338) |

### ESP32 ADC1 Channel → GPIO Reference

| ADC1 Channel | GPIO |
|-------------|------|
| CH0 | GPIO 36 (SVP) |
| CH1 | GPIO 37 |
| CH2 | GPIO 38 |
| CH3 | GPIO 39 (SVN) |
| CH4 | GPIO 32 |
| CH5 | GPIO 33 |
| CH6 | GPIO 34 |
| CH7 | GPIO 35 |

Note: GPIO 34-39 are input-only on the ESP32.

## Motor Subsystem

Source file paths found in binary:
- `./main/motor/motor.c` — motor control logic
- `./main/motor/motor_adc.c` — hall sensor reading

Key details:
- **4 motor channel groups** (GROUP 1-4), each with independent hall sensor
- **Forward/reverse PWM** via ESP32 LEDC peripheral (`fwd_chan` / `rev_chan`)
- **Gradual startup** (soft-start ramp to prevent mechanical shock)
- Functions: `motor_pwm_init`, `motor_ledc_timer_init`, `hall_adc_init`, `hall_get_state`
- Per-group config struct is 0x24 bytes; fields at offsets 0x14/0x18/0x1c/0x20
  hold fwd/rev LEDC channel + fwd/rev GPIO. Hall ADC channel is at +0x04.

The 4-group layout suggests the hardware supports controlling multiple vent
modules from one board (or a multi-flap vent), not a single vent motor. The
number of *active* groups at runtime is stored at `PTR_DAT_400d0da4` and
determined during init — likely via strip-detect ADC or a separate
enumeration. Needs on-hardware confirmation.
- LEDC functions used: `ledc_timer_config`, `ledc_channel_config`, `ledc_set_duty`, `ledc_update_duty`, `ledc_set_fade_with_time`, `ledc_fade_start`

## RGB LED Subsystem

Source file paths found in binary:
- `./main/rgb/app_rgb.c` — LED strip control
- `./main/rgb/app_rgb_effect.c` — lighting effects engine

Key details:
- Uses **RMT peripheral** for WS2812 timing (standard ESP-IDF approach)
- Supports **multiple RMT channels**: `rgb_channels[i].channel`, `rgb_channels[i].encoder`
- Auto-detects 1 strip (16 LEDs) vs 2 strips (27 LEDs) via ADC on GPIO 35
- 7 effect modes: static, breathing, strobing, wave, marquee, color cycle, rainbow
- Functions: `rgb_init`, `rgb_light_mode`, `rgb_switch`, `rgb_sundry`, `sys_rgb_mode`, `rmt_new_led_strip_encoder`, `rmt_transmit`

## Communication Subsystem (Stock Firmware)

- **WiFi**: STA + AP mode, mDNS hostname (`PandaVent.local`)
- **MQTT**: `bambu_mqtt` module, local connection to Bambu printer
- **UDP Discovery**: `bambu_udp` — SSDP over UDP to discover Bambu printers
- **Web UI**: ESP HTTP Server (`httpd`) with WebSocket support
- **OTA**: HTTPS OTA via web upload
- **Hotspot**: Default SSID `Panda_Vent_XXXX`, password `987654321`, IP `192.168.254.1`

### MQTT Fields Parsed from Bambu Printers

| Field | Description |
|-------|-------------|
| `gcode_state` | Current print state |
| `stg_cur` | Current stage |
| `nozzle_temper` | Nozzle temperature |
| `bed_temper` | Bed temperature |
| `layer_num` | Current layer number |
| `print_error` | Error code |

Push command sent to Bambu printer:
```json
{"pushing": {"sequence_id": "0", "command": "pushall"}}
```

## NVS Storage Keys

Found in binary strings:

| Key | Purpose |
|-----|---------|
| `ssid` | WiFi SSID |
| `password` | WiFi password |
| `access_code` | Bambu printer access code |
| `ap.ssid` | AP mode SSID |
| `language` | UI language |
| `current_light_mode` | Active lighting mode |
| `rgb_light_mode` | RGB mode setting |
| `sys_rgb_mode` | System RGB mode |
| `warn` | Warning state |
| `warning_hot_mode` | Temperature warning mode |
| `warning_overide` | Warning override flag |
| `warning_sw` | Warning switch |
| `set_hostname` | mDNS hostname |

## Firmware Build Info

- NVS namespace: `app_nvs`
- Device name format: `ESP32_%02x%02X%02X` (MAC-based)
- AP SSID prefix: `Panda_Vent_`
- Built with ESP-IDF, cross-compiled on Windows (`HOST-x86_64-w64-mingw32`)
