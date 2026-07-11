# OpenVent

Custom firmware for the [Bigtreetech Panda Vent](https://github.com/bigtreetech/Panda-Vent) that replaces the stock Bambu Lab integration with [Moonraker](https://github.com/Arksine/moonraker)/[Klipper](https://www.klipper3d.org/) support.

## What is this?

The Panda Vent is a smart vent riser for enclosed 3D printers. The stock firmware only works with Bambu Lab printers via their proprietary MQTT protocol. OpenVent replaces that firmware so the hardware works with any Klipper-based printer via Moonraker's API.

## Features

Working today (v0.3.3):

- **Automatic vent control** — six-state printer model (idle / preparing / printing / paused / complete / error), material-aware policy (PLA opens for cooling, ABS/ASA seals for heat retention), bed-temp hysteresis for residual heat
- **Stock-parity hall sensing** — per-boot ADC line-fitting calibration with calibrated-millivolt thresholds, matching stock's reproduction contract
- **Captive portal WiFi setup** — same UX as the stock firmware, show-password toggles, dark mode
- **Moonraker integration** — WebSocket ingest with `webhooks` / `print_stats` / `virtual_sdcard` / `heater_bed` / `extruder` / optional chamber + `save_variables` (for material), re-subscribes on Klippy restart
- **Tasmota-compatible power endpoint** — `POWER_ON vent` / `POWER_OFF vent` from any Klipper macro, Mainsail/Fluidd Power-panel toggle for free
- **Configurable thresholds** — bed OPEN/CLOSE °C editable in the portal, persisted to NVS
- **Physical button control** — auto/manual mode toggle, manual vent override, manual target persists across reboots
- **Web configuration UI** — Home / WiFi / Printer / Log / System tabs with live event log
- **OTA firmware updates** — flash new firmware from the web UI

Deferred:

- **RGB LED status** — WS2812 driver + effects engine, planned for the 0.4.x series

## Documentation

- [Hardware Analysis](docs/HARDWARE_ANALYSIS.md) — reverse-engineered GPIO pinout and hardware details
- [Roadmap](docs/ROADMAP.md) — development phases and architecture

## Hardware

- **Kit contents**: 1 mainboard + several motorized vent modules + LED boards. Each vent module has one motor, one hall sensor, and identical 3-pin JST connectors on both ends — so multiple modules chain together
- **Board**: Bigtreetech Panda Vent (ESP32 Xtensa dual-core LX6)
- **Motors**: up to 4 independent DC motors across two mainboard chains, each driven forward/reverse via LEDC PWM at 30 kHz with hall-sensor position feedback
- **LEDs**: WS2812 addressable strips via RMT — GPIO 14 and GPIO 4, one per chain
- **User button**: switch on GPIO 12, illumination LED on GPIO 27 (off = auto, blink = manual)
- **BOOT button**: GPIO 0 (long-press = factory reset)
- **Hardware auto-detect**: single ADC on GPIO 35 picks between "all chains populated" (4 motors), "one chain" (2 motors), and "nothing" — hot-plug supported

Full pin map + provenance: [docs/HARDWARE_ANALYSIS.md](docs/HARDWARE_ANALYSIS.md).

## ⚠ Back up your stock firmware first

Before flashing OpenVent, **dump the whole flash** so you have a way back —
BTT doesn't publish source or release binaries for the Panda Vent, so if you
lose the stock image there's no official way to recover it. The included
[`scripts/openvent`](scripts/openvent) helper wraps this up:

```
scripts/openvent backup                 # → stock-panda-vent-backup.bin
scripts/openvent install v0.2.0         # download + flash a release
scripts/openvent restore stock-panda-vent-backup.bin   # roll back
```

(All three take an optional `-p /dev/tty.usbserial-XXXX` if the default
autodetect doesn't find the right port.)

## Status

**v0.3.3 restores stock ADC calibration parity.** Reverse-engineered
against the Ghidra decompile of stock v1.0.0 —
see [`docs/adc-calibration-spec.md`](docs/adc-calibration-spec.md) for
the reproduction contract. This unblocks per-board hall-sensor accuracy
without requiring per-board threshold tuning.

Prior milestone: **v0.2.6 was the first stable proof-of-concept release.**
2026-07-10 field test on tester OldGuyMeltsPlastic's retail 2-vent kit:
10× consecutive open/close cycles, no ESP crash, motor stops cleanly on
each arrival.

- ✅ Motors drive both directions and reliably stop at endpoints, using
  stock-parity mV thresholds and per-boot ADC line-fitting calibration
- ✅ Six-state printer model + material-aware auto policy, re-subscribes
  on Klippy restart
- ✅ Firmware flashes on real Panda Vent hardware; `openvent` script for
  backup / restore / install works end-to-end
- ✅ WiFi station + AP fallback, mDNS `OpenVent.local`, captive portal
- ✅ Portal: tabbed UI (Home / WiFi / Printer / Log / System), live event
  log, WiFi setup, Moonraker config, OTA upload, factory reset, dark mode
- ✅ Tasmota-compatible power endpoint for gcode-macro vent control
- ⬜ Deferred to 0.4.x: WS2812 RGB status lighting

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/wildtang3nt)

## License

[MIT](LICENSE) © Justin Hayes
