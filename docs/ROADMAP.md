# PandaVent-Klipper Roadmap

Custom firmware for the Bigtreetech Panda Vent hardware that replaces the
stock Bambu Lab MQTT integration with Moonraker/Klipper support.

## Goals

- Repurpose stock Panda Vent hardware for Klipper-based printers
- Automatically open/close the vent based on printer status (bed temperature)
- Provide a captive portal AP for WiFi setup (like stock firmware)
- Simple web UI for Moonraker connection settings
- OTA firmware updates via web UI
- Complete feature parity with original Bigtreetech firmware

## Phase 1 — MVP (Hardware Bring-up & Core Logic)

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
- [x] Advertise `PandaVent.local` over mDNS

### Moonraker Configuration
- [x] Web page: enter Moonraker IP/hostname and port
- [x] Optional API key field
- [x] Store config in NVS
- [x] Connection status indicator on web UI (status header)

### Hardware Drivers
- [x] GPIO pin assignments confirmed via Ghidra disassembly ([HARDWARE_ANALYSIS](HARDWARE_ANALYSIS.md))
- [x] Motor driver: LEDC PWM forward/reverse with soft-start (`pv_motor`)
- [x] Hall sensor reading for vent position feedback (5-state classifier)
- [x] Button handler: debounce, single-click, long-press (`pv_button`)
- [x] Status LED on user button (`pv_status_led`; GPIO 27, off = auto, blink = manual)
- [x] Read hardware-config ADC on GPIO 35 to pick 0/2/4 active motor groups

### Remaining Core Verification
- [ ] On-device verification pass:
  - LED indicates mode correctly
  - Portal reachable from browser (both AP and STA modes)
  - Motors respond and hall reads are plausible
  - Confirm which mainboard chain maps to motor groups 0/1 vs 2/3
  - Confirm the 3-way config-detect ADC bands hit the expected raw values

## Phase 2 — Firmware Parity (Web UI & Settings)

To match the stock BTT firmware capabilities, the following features must be implemented in the Web UI and backend:

### Wi-Fi Page Parity
- [x] Network Scanner: Scan for visible Wi-Fi networks and display as a selectable list (`pv_wifi_scan_start` + portal `<select>` dropdown)
- [ ] Connection Status: Display IP address and clear connection status/troubleshooting prompts

### AP Page Parity
- [x] Configurable AP Hotspot: Allow user to change AP SSID, Password, and IP subnet (`pv_wifi_ap_config_t` + portal `/ap_config`)
- [ ] AP Toggle: Allow disabling the AP hotspot entirely to save resources
- [x] Apply & Reboot: Require and handle device reboot to apply AP changes (`pv_wifi_set_ap_config_and_reboot`)

### Printer (Moonraker) Page Parity
- [ ] Printer Discovery: Implement mDNS discovery for `_moonraker._tcp` to populate a selectable list of printers on the LAN, eliminating manual IP entry

### Settings Page Parity
- [ ] OTA Updates: Implement `.bin` upload via Web UI and OTA flashing process
- [ ] Factory Reset: Add Web UI button for clearing NVS and rebooting (physical BOOT-button reset already implemented in `pv_button`)
- [ ] Firmware Version: Display current version
- [ ] Language Toggle: Support for English and Simplified Chinese (optional, but needed for 1:1 parity)

### Vent Button Parity (Minor Adjustments)
- [x] Single-click in AUTO immediately switches to MANUAL and reverses vent state (`app_main:on_button`)
- [x] Long-press mode switch (3 s per stock v1.0.0 binary `DAT_400d0948 = 2999 × 10 ms` and shipped user manual; the [BTT wiki](https://neo.bttwiki.com/en/docs/panda-series/module/panda-vent/panda-vent-firmware) says 6 s but v1.0.0 is the only released firmware — the wiki appears aspirational)
- [x] Status LED matches stock: OFF during AUTO, blinking during MANUAL (`app_main:reflect_mode_on_led`)

## Phase 3 — Firmware Parity (RGB Lighting & Themes)

The stock firmware relies heavily on RGB status lighting via WS2812 strips. This requires implementing the `pv_rgb` RMT driver and a complex effects engine.

### RGB Driver & Core
- [ ] WS2812 LED strip driver via RMT
- [ ] Auto-detect strip count (1 strip = 16 LEDs, 2 strips = 27 LEDs) via ADC on GPIO 35

### Theme Page (Control UI)
- [ ] Main Light Switch: Global toggle for all RGB effects
- [ ] Warning Override Switch: Flash red globally on printer error (overrides all effects)
- [ ] Behavior Toggles: "Follow Printer Mode" (colors match state) vs "Follow Exhaust Vent" (colors match vent open/close)
- [ ] Reverse Direction: Toggle to reverse the flow of LED effects

### Light Modes Engine
- [ ] **Simple Mode**: Fixed effect applied to all LEDs. 
  - Support 7 effects: Solid, Breathing, Flash, Flow, Marquee, Rainbow Cycle, Multicolor.
  - Support adjustable Color, Brightness (0-100%), Speed (0-100%).
- [ ] **Advance Mode (H2D)**: State-machine-driven effects based on Moonraker status.
  - Configure distinct effect, color, brightness, and speed for each state: Idle, Preparing, Printing, Paused, Complete, Error.
- [ ] **Warning Hot Mode**: Temperature-driven effects based on Moonraker `heater_bed` or `extruder`.
  - Safe State (Green) vs Danger State (Red).
  - Support static or flashing effects with adjustable brightness and speed.

## Phase 4 — Extras & Polish

- [ ] Print progress on LEDs (percentage bar using Moonraker `display_status` / `virtual_sdcard`)
- [ ] Klipper macro integration (allow macros to explicitly control vent/LEDs via custom endpoints)
- [ ] Home Assistant / MQTT bridge (optional fallback for non-Moonraker setups)

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
