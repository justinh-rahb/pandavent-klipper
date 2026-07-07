# OpenVent Firmware

ESP-IDF v5.3+ project targeting the classic ESP32 in the Bigtreetech Panda Vent.

## First-time setup (macOS)

```sh
brew install cmake ninja dfu-util
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
# Add to your shell rc:
alias get_idf='. ~/esp/esp-idf/export.sh'
```

Then in each new shell:

```sh
get_idf
```

## Build / flash / monitor

```sh
cd firmware
idf.py set-target esp32   # first time only
idf.py build
idf.py -p /dev/tty.usbserial-* flash monitor
```

`Ctrl-]` exits the monitor.

## CI / releases

GitHub Actions builds the firmware on pushes and pull requests that touch
`firmware/` or workflow files. Release artifacts are produced when a `v*` tag
is pushed, or by running the "Firmware Release" workflow manually with a tag.

### Flashing release artifacts

Each release ships two binaries:

- **`openvent-full.bin`** — bootloader + partition table + OTA data +
  app, one file. For first-time flashing over USB.

  ```sh
  python -m esptool --chip esp32 -p /dev/tty.usbserial-* -b 460800 \
    write_flash 0x0 openvent-full.bin
  ```

- **`openvent-ota.bin`** — app only. Upload via the portal's
  **OTA firmware update** form on a device that's already running
  OpenVent. No USB cable needed.

`SHA256SUMS` next to them if you want to verify.

## Layout

```
firmware/
├── CMakeLists.txt
├── partitions.csv           # OTA-capable 4MB layout (two app slots + two NVS)
├── sdkconfig.defaults       # checked in; sdkconfig itself is generated + gitignored
├── main/
│   └── app_main.c           # thin orchestrator: init components, route buttons
└── components/
    ├── pv_board/            # GPIO pin map — single source of truth
    ├── pv_motor/            # 30 kHz LEDC PWM + hall ADC state machine (4 groups)
    ├── pv_button/           # USER + BOOT debouncing, short/long press dispatch
    ├── pv_status_led/       # user-button LED: off = auto, blink = manual
    ├── pv_wifi/             # STA + AP fallback, NVS-backed credentials
    ├── pv_moonraker/        # WebSocket client, subscribes to print_stats + heater_bed
    ├── pv_policy/           # auto/manual mode, hysteresis-based open/close decision
    └── pv_portal/           # unified web UI (AP + STA), captive DNS in AP mode
```

Each component owns its own task, exposes a thread-safe API, and persists any
state under the `app_nvs` namespace (stock-firmware-compatible).

## Flashing the first time

The stock firmware doesn't expose a UART bootloader path publicly, but the
ESP32 module inside the Panda Vent has the standard EN/BOOT pads. Refer to
BTT's docs for pad locations. Ground BOOT while pulsing EN to enter download
mode, then flash normally.
