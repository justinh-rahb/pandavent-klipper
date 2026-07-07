# OpenVent Roadmap

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
- [x] Advertise `OpenVent.local` over mDNS

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

Verified on a bare ESP32 devkit:
- [x] Boots cleanly, no panics
- [x] LED (GPIO 27) driven correctly when policy mode changes
- [x] Portal reachable from a phone in both AP and STA modes
- [x] Captive portal detection triggers on iOS / Android
- [x] WiFi scan populates the SSID dropdown
- [x] mDNS `OpenVent.local` resolves on the LAN
- [x] Config-detect ADC reports 0 groups on a bare board (unpopulated pin)
- [x] OTA upload from the portal writes to the inactive slot and reboots into it

Verified on real Panda Vent hardware (2026-07-07 field test — [notes](testing/2026-07-07-field-test-notes.md)):
- [x] `openvent install` flashes cleanly over USB
- [x] `openvent backup` + `openvent restore` round-trips the stock image
- [x] Captive portal + WiFi provisioning workflow
- [x] Motor drive direction correct — vents physically open on command
- [x] Hall sensors report values (endpoint bands were mislabeled — fixed in v0.2.1)
- [x] Stock firmware restore is fully non-destructive

Still to verify on hardware:
- [ ] "Arrived at endpoint" detection works with the corrected hall labels (motor stops instead of chugging into the retry loop)
- [ ] Vents close on command (was blocked by same label bug)
- [ ] Confirm which mainboard chain maps to motor groups 0/1 vs 2/3
- [ ] Confirm the 3-way config-detect ADC bands hit the expected raw values on the retail 2-vent kit
- [ ] WS2812 outputs light up on the LED boards
- [ ] Root-cause the intermittent AP dropouts observed during the field session

## Phase 2 — Firmware Parity (Web UI & Settings)

To match the stock BTT firmware capabilities, the following features must be implemented in the Web UI and backend:

### Portal (Web UI)
- [x] Tabbed layout — Home / WiFi / Printer / System (CSS-only, no JS)
- [x] Persistent status card — firmware version, WiFi state + IP + RSSI, Moonraker state, printer state, bed temp, vent target, mode
- [x] Quick-action Open / Close vent buttons on Home (same effect as short-press on the physical button)
- [x] Dark mode via `prefers-color-scheme` (no toggle — follows OS setting)

### Wi-Fi Page Parity
- [x] Network scanner + selectable SSID dropdown (`pv_wifi_scan_start`)
- [x] Manual SSID entry for hidden networks
- [x] Connection status: IP + RSSI in the status card

### AP Page Parity
- [x] Configurable AP hotspot — SSID / password / IP (`pv_wifi_ap_config_t`)
- [x] AP toggle — checkbox to disable AP fallback entirely (with lockout warning)
- [x] Apply & reboot on save (`pv_wifi_set_ap_config_and_reboot`)

### Printer (Moonraker) Page Parity
- [ ] Printer discovery: tried mDNS `_moonraker._tcp` (most Klipper installs don't advertise it) and then a subnet TCP-connect sweep (unreliable on real LANs). Both removed. Users configure host/port manually. Worth revisiting later with better probing.

### System Page Parity
- [x] OTA updates: `.bin` upload via web UI, streams directly into the inactive OTA partition
- [x] Factory reset: web UI button (in Danger Zone) — clears WiFi / Moonraker / policy NVS and reboots
- [x] Firmware version: shown in status card (from `esp_app_get_description`)
- [ ] Language toggle: English / Simplified Chinese (optional; not needed for functional parity)

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
