# OpenVent

Custom firmware for the [Bigtreetech Panda Vent](https://github.com/bigtreetech/Panda-Vent) that replaces the stock Bambu Lab integration with [Moonraker](https://github.com/Arksine/moonraker)/[Klipper](https://www.klipper3d.org/) support.

## What is this?

The Panda Vent is a smart vent riser for enclosed 3D printers. The stock firmware only works with Bambu Lab printers via their proprietary MQTT protocol. OpenVent replaces that firmware so the hardware works with any Klipper-based printer via Moonraker's API.

## Features (Planned)

- **Automatic vent control** — opens/closes based on bed temperature and print status
- **Captive portal WiFi setup** — same UX as the stock firmware
- **Moonraker integration** — connects via WebSocket for real-time printer status
- **Physical button control** — auto/manual mode toggle, manual vent override
- **Web configuration UI** — Moonraker connection, vent settings
- **OTA firmware updates** — flash new firmware from the web UI
- **RGB LED status** (Phase 2) — printer state visualization via WS2812 strips

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

**First real-hardware field test happened on 2026-07-07**
([notes](docs/testing/2026-07-07-field-test-notes.md)). First-time flash,
UI, captive portal, and stock-firmware restore all worked. Motors drove
in the correct direction but the "arrived at endpoint" check was fooled
by inverted hall thresholds — fixed in v0.2.1 pending re-verification.

- ✅ Firmware flashes on real Panda Vent hardware; `openvent` script for
  backup / restore / install works end-to-end
- ✅ WiFi station + AP fallback, mDNS `OpenVent.local`, captive portal
- ✅ Portal: tabbed UI, live status, WiFi setup, Moonraker config, OTA upload, factory reset, dark mode
- ✅ Moonraker WebSocket client with `print_stats` + `heater_bed` subscriptions
- ✅ Physical button and status LED wired to the mode state machine
- ✅ Motors drive in the correct direction on real hardware; vents physically open on command
- ⏳ Hall-endpoint detection labels corrected 2026-07-07 (v0.2.1) — re-verification pending
- ⬜ Not yet verified on hardware: WS2812 outputs, exact ADC bands for the config-detect line

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/wildtang3nt)

## License

[MIT](LICENSE) © Justin Hayes
