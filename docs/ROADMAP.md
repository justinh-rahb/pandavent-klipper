# PandaVent-Klipper Roadmap

Custom firmware for the Bigtreetech Panda Vent hardware that replaces the
stock Bambu Lab MQTT integration with Moonraker/Klipper support.

## Goals

- Repurpose stock Panda Vent hardware for Klipper-based printers
- Automatically open/close the vent based on printer status (bed temperature)
- Provide a captive portal AP for WiFi setup (like stock firmware)
- Simple web UI for Moonraker connection settings
- OTA firmware updates via web UI

## Phase 1 — MVP

### Vent Control via Moonraker
- [x] Connect to Moonraker via WebSocket (`pv_moonraker`)
- [x] Subscribe to `print_stats` and `heater_bed` objects
- [x] Open vent when bed is heated / printing is active (`pv_policy`, AUTO mode)
- [x] Close vent when bed cools down / printer is idle (35 °C / 45 °C hysteresis)
- [x] Auto/manual mode toggle via physical button (GPIO 12) (`pv_button` + `app_main`)
- [x] Long-press button to switch between auto and manual mode

### WiFi & Captive Portal
- [x] AP mode on first boot (captive portal with DNS redirect) (`pv_wifi` + `pv_portal`)
- [x] Web page to enter SSID + password
- [x] Store WiFi credentials in NVS (`app_nvs` namespace, stock-compatible)
- [x] Auto-reconnect to saved WiFi on boot
- [ ] Scan visible networks and pick from a list (currently: type SSID by hand)

### Moonraker Configuration
- [x] Web page: enter Moonraker IP/hostname and port
- [x] Optional API key field
- [ ] mDNS discovery of `_moonraker._tcp` services
- [x] Store config in NVS
- [x] Connection status indicator on web UI (status header)

### Hardware Drivers
- [x] GPIO pin assignments confirmed via Ghidra disassembly ([HARDWARE_ANALYSIS](HARDWARE_ANALYSIS.md))
- [x] Motor driver: LEDC PWM forward/reverse with soft-start (`pv_motor`)
- [x] Hall sensor reading for vent position feedback (5-state classifier)
- [x] Button handler: debounce, single-click, long-press (`pv_button`)
- [x] Status LED on user button (`pv_status_led`; GPIO 27, solid = auto, blink = manual)

### Remaining before hardware bring-up
- [x] Read hardware-config ADC on GPIO 35 to pick 0/2/4 active motor groups (`pv_motor` samples once per second with 3-cycle debounce; matches stock's hot-plug behavior)
- [ ] mDNS hostname (`PandaVent.local`)
- [ ] On-device verification pass (LED indicates mode, motors respond, hall reads plausible, portal reachable)

## Phase 2 — RGB & Effects

- [ ] WS2812 LED strip driver via RMT
- [ ] Auto-detect strip count (1 or 2) via ADC on GPIO 35
- [ ] Map printer states to LED colors/effects
- [ ] Web UI for RGB configuration
- [ ] Temperature-based color gradients

## Phase 3 — Polish

- [ ] OTA firmware update via web UI
- [ ] Factory reset via BOOT button long-press
- [ ] mDNS hostname configuration
- [ ] Print progress on LEDs (percentage bar)
- [ ] Klipper macro integration (allow macros to control vent/LEDs)
- [ ] Home Assistant / MQTT bridge (optional)

## Architecture

Firmware is split into standalone ESP-IDF components under `firmware/components/`.
`app_main` is a thin orchestrator: boot each service in order, register the
button callback, mirror policy mode to the LED.

```
                    ┌─────────────────────────────┐
   printer ─── ws ──┤  pv_moonraker  (WS client)  │
                    │  print_stats, heater_bed    │
                    └──────────┬──────────────────┘
                               │ status
                               ▼
   button ──►  pv_button ──►  pv_policy ──►  pv_motor  ──► 4x DC motor
                    │        (auto/manual)      │           + hall ADC
                    │              │            │
                    ▼              ▼            │
              pv_status_led    pv_portal ◄──────┘  (web UI in both AP + STA)
              (GPIO 27)             │
                                    ▼
                                 pv_wifi (STA + AP fallback)

   pv_board = pin definitions shared by every component (single source of truth)
```

Every long-lived component owns its own FreeRTOS task and exposes a thread-safe
API. Shared state (WiFi/Moonraker/policy) lives in the `app_nvs` NVS namespace
so it survives reboots and is compatible with the stock firmware's layout.

External:

```
    Klipper host ── Moonraker ── ws ──► ESP32 ── PWM ──► vent motors
                                  │
                                  └── mDNS (planned) ── PandaVent.local
```

## State Machine

```
                    ┌─────────┐
          ┌────────►│  IDLE   │◄────────┐
          │         │vent shut│         │
          │         └────┬────┘         │
          │              │              │
     bed cools      bed heats      error/cancel
     below thr.     above thr.          │
          │              │              │
          │              ▼              │
          │         ┌─────────┐         │
          ├─────────│PRINTING │─────────┤
          │         │vent open│         │
          │         └────┬────┘         │
          │              │              │
          │           paused            │
          │              │              │
          │              ▼              │
          │         ┌─────────┐         │
          └─────────│ PAUSED  │─────────┘
                    │vent hold│
                    └─────────┘
```

## Moonraker Status Mapping

| Moonraker `print_stats.state` | Vent Action | LED (Phase 2) |
|-------------------------------|-------------|---------------|
| `"standby"` | Closed | White |
| `"printing"` | Open (auto) | Rainbow |
| `"paused"` | Hold position | White |
| `"complete"` | Open (cool-down timer) | Green |
| `"error"` / `"cancelled"` | Closed | Red |
| Klipper not ready | Closed | Breathing blue |

Bed temperature is the primary trigger: vent opens when `heater_bed.temperature`
exceeds a configurable threshold (default: 40°C), closes when it drops below.
