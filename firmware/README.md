# PandaVent-Klipper Firmware

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

## Layout

- `main/app_main.c` — entry point (currently just a button-LED heartbeat)
- `main/hw_pins.h`  — GPIO map recovered from stock firmware; single source of truth
- `partitions.csv`  — OTA-capable 4MB layout (two app slots + NVS)
- `sdkconfig.defaults` — checked-in defaults; `sdkconfig` itself is generated & gitignored

## Flashing the first time

The stock firmware doesn't expose a UART bootloader path publicly, but the
ESP32 module inside the Panda Vent has the standard EN/BOOT pads. Refer to
BTT's docs for pad locations. Ground BOOT while pulsing EN to enter download
mode, then flash normally.
