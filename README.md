# PandaVent-Klipper

Custom firmware for the [Bigtreetech Panda Vent](https://github.com/bigtreetech/Panda-Vent) that replaces the stock Bambu Lab integration with [Moonraker](https://github.com/Arksine/moonraker)/[Klipper](https://www.klipper3d.org/) support.

## What is this?

The Panda Vent is a smart vent riser for enclosed 3D printers. The stock firmware only works with Bambu Lab printers via their proprietary MQTT protocol. This project replaces that firmware so the hardware works with any Klipper-based printer via Moonraker's API.

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
- **User button**: switch on GPIO 12, illumination LED on GPIO 27 (steady = auto, blink = manual)
- **BOOT button**: GPIO 0 (long-press = factory reset)
- **Hardware auto-detect**: single ADC on GPIO 35 picks between "all chains populated" (4 motors), "one chain" (2 motors), and "nothing" — hot-plug supported

Full pin map + provenance: [docs/HARDWARE_ANALYSIS.md](docs/HARDWARE_ANALYSIS.md).

## Status

🚧 **Phase 1 firmware complete, awaiting hardware bring-up.** Every component compiles against ESP-IDF v5.3+; on-device verification is the next milestone.

## License

[MIT](LICENSE) © Justin Hayes
