# poom_app_pack

`poom_app_pack` is the **application pack** behind the POOM on-device UI: a set of small OLED-first apps
that are launched from the top-level menu (`applications/poom_menu/`).

Each app:
- Renders to the OLED via `poom_arduboy_display` / `Arduboy2`
- Consumes button events via SBUS topic `input/button` (published by `button_driver`)
- Starts/stops the specific subsystem it needs (Wi‑Fi, BLE, NFC, IR, SD, etc.)
- Returns control to the launcher by publishing `poom/menu/resume`

## Purpose

- Provide a curated set of apps grouped by domain (THE BEAST / THE ZEN / THE GAMER / THE MAKER / SETTINGS)
- Offer a consistent, small-screen UI pattern (list selection, status screens, simple editors)
- Encapsulate subsystem lifecycles so apps can be started and stopped safely

## Structure

```text
applications/poom_app_pack/
├── CMakeLists.txt
├── component.mk
├── README.md
├── include/
│   ├── iconos.h
│   ├── menu_air_ble.h
│   ├── menu_ble_control.h
│   ├── menu_ble_scan.h
│   ├── menu_ble_spam.h
│   ├── menu_captive.h
│   ├── menu_cli_nfc.h
│   ├── menu_cli_web.h
│   ├── menu_control_music.h
│   ├── menu_deauth.h
│   ├── menu_deauth_detector.h
│   ├── menu_dfu.h
│   ├── menu_edge_impulse.h
│   ├── menu_fw_info.h
│   ├── menu_http_load_test.h
│   ├── menu_imu_monitor.h
│   ├── menu_ir_universal.h
│   ├── menu_karma.h
│   ├── menu_lua.h
│   ├── menu_midi.h
│   ├── menu_midi_harmony.h
│   ├── menu_nfc.h
│   ├── menu_nfc_tuning.h
│   ├── menu_plot.h
│   ├── menu_poom_pcap.h
│   ├── menu_poom_drone_emul.h
│   ├── menu_poom_drone_scan.h
│   ├── menu_poom_droneid.h
│   ├── menu_poom_wifi_arp.h
│   ├── menu_poom_wifi_scan.h
│   ├── menu_scanner_core.h
│   ├── menu_sd_browser.h
│   ├── menu_sniffer_device.h
│   ├── menu_ssid_spam.h
│   ├── menu_tone.h
│   ├── menu_tracker.h
│   └── menu_ws2812_color.h
└── src/
    ├── menu_air_ble.c
    ├── menu_ble_control.c
    ├── menu_ble_scan.c
    ├── menu_ble_spam.c
    ├── menu_captive.c
    ├── menu_cli_nfc.c
    ├── menu_cli_web.c
    ├── menu_control_music.c
    ├── menu_deauth.c
    ├── menu_deauth_detector.c
    ├── menu_dfu.c
    ├── menu_edge_impulse.c
    ├── menu_fw_info.c
    ├── menu_http_load_test.c
    ├── menu_imu_monitor.c
    ├── menu_ir_universal.c
    ├── menu_karma.c
    ├── menu_lua.c
    ├── menu_midi.c
    ├── menu_midi_harmony.c
    ├── menu_nfc.c
    ├── menu_nfc_tuning.c
    ├── menu_plot.c
    ├── menu_poom_pcap.c
    ├── menu_poom_drone_emul.c
    ├── menu_poom_drone_scan.c
    ├── menu_poom_droneid.c
    ├── menu_poom_wifi_arp.c
    ├── menu_poom_wifi_scan.c
    ├── menu_scanner_core.c
    ├── menu_sd_browser.c
    ├── menu_sniffer_device.c
    ├── menu_ssid_spam.c
    ├── menu_tone.c
    ├── menu_tracker.c
    └── menu_ws2812_color.c
```

## Menu Structure

The launcher (`applications/poom_menu/`) exposes these apps in 5 main categories:

```mermaid
graph TD
    A[POOM Main Menu] --> B[THE BEAST]
    A --> C[THE ZEN]
    A --> D[THE GAMER]
    A --> E[THE MAKER]
    A --> F[SETTINGS]

    B --> B1[DEAUTH]
    B --> B2[DEAUTH DET]
    B --> B3[KARMA]
    B --> B4[SPAM WIFI]
    B --> B5[SPAM BLE]
    B --> B6[CAPTIVE PORTAL]
    B --> B7[TRACKER]
    B --> B8[SNIFFER]
    B --> B9[SCAN CHANNELS]
    B --> B10[SCAN NET]
    B --> B11[CLI]
    B --> B12[HTTP LOAD]
    B --> B13[PROBE REQ]

    C --> C1[MIDI]
    C --> C2[HARMONY]
    C --> C3[TONE]
    C --> C4[CONTROL]
    C --> C5[NFC]
    C --> C6[IR UNIV]
    C --> C7[POOM WEB]

    D --> D1[TINY CONTROL]
    D --> D2[WII]

    E --> E1[PLOT]
    E --> E2[BLE SCAN]
    E --> E3[DRONE SCAN]
    E --> E4[DRONE EMUL]
    E --> E5[DRONE ID]
    E --> E6[I2C]
    E --> E7[LUA]
    E --> E8[EDGE AI]

    F --> F1[DFU]
    F --> F2[FW INFO]
    F --> F3[IMU]
    F --> F4[LED RGB]
    F --> F5[WI-FI]
    F --> F6[NFC TUNE]
    F --> F7[FILES]
```

## Applications Reference

This section expands each app with:
- **What it is** (user-visible goal)
- **Subsystems** it uses (Wi‑Fi/BLE/NFC/IR/SD/…)
- **I/O** (OLED status, SD files, UART logs)
- **Exit/return** behavior (how it hands control back to the launcher)

### THE BEAST — Wireless & RF tools (authorized environments only)

#### DEAUTH (`menu_deauth.c`, `app_deauth()`)
- What it is: Wi‑Fi scanner + active test modes for controlled environments.
- Subsystems: `poom_wifi_scanner`, `poom_wifi_attacks` (Wi‑Fi radio).
- UI: multi-screen flow (idle → scanning → AP list → mode selection → running).
- I/O: on-screen status; additional details via UART logs.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### DEAUTH DET (`menu_deauth_detector.c`, `app_deauth_detector()`)
- What it is: passive deauth-detector dashboard (alert level + correlation/reason views + basic settings).
- Subsystems: `poom_wifi_deauth_detector`.
- UI: multiple pages (overview/correlation/reason/settings).
- I/O: OLED-only summary; intended to be “always-on” during a session.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### KARMA (`menu_karma.c`, `menu_karma_init()`)
- What it is: probe-response/association behavior test tool for lab setups.
- Subsystems: `poom_wifi_karma` (Wi‑Fi radio).
- UI: status screen + current active SSID text.
- I/O: OLED status; UART logs for deeper debugging.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### SPAM WIFI (`menu_ssid_spam.c`, `menu_ssid_spam_init()`)
- What it is: Wi‑Fi beacon/SSID advertising stress-test tool.
- Subsystems: `poom_wifi_spam`.
- UI: simple start/stop + status.
- I/O: OLED status; RF output.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### SPAM BLE (`menu_ble_spam.c`, `menu_ble_spam_display()`)
- What it is: BLE advertising stress-test / compatibility test helper.
- Subsystems: `poom_ble_spam`.
- UI: status + mode cycling (implementation-defined).
- I/O: OLED status; BLE advertising traffic.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### CAPTIVE PORTAL (`menu_captive.c`, `menu_captive_display()`)
- What it is: captive-portal lab demo for web UI flows and device behavior testing.
- Subsystems: `poom_wifi_captive`, `poom_wifi_scanner`, `poom_ui_keyboard`, `sd_card`.
- UI: on-device setup + text entry (via on-screen keyboard) for test network parameters as needed by the demo.
- I/O: may read/write configuration/assets from SD depending on build; OLED status for run state.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### TRACKER (`menu_tracker.c`, `app_tracker_menu()`)
- What it is: BLE tracker front-end (configure targets/labels and show proximity-like info).
- Subsystems: `poom_ble_tracker`, `poom_ui_keyboard`, `poom_secrets_store` (persistence).
- UI: list-style UI + simple text input for identifiers.
- I/O: OLED status; stored settings in secrets store.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### SNIFFER (`menu_poom_pcap.c`, `menu_poom_pcap_show()`)
- What it is: capture packets into PCAP for later analysis.
- Subsystems: `poom_pcap_manager`, plus radios depending on selected capture mode (Zigbee / Wi‑Fi / BLE).
- UI: mode select + channel/config screens + running screen.
- I/O: output target shows as `SD` when a PCAP file path is available, otherwise `UART`.
- Exit/return: stops capture, returns to launcher, publishes `poom/menu/resume`.

#### SCAN CHANNELS (`menu_scanner_core.c`, `menu_scanner_core_show()`)
- What it is: real-time RF/channel activity view (Wi‑Fi vs IEEE 802.15.4).
- Subsystems: `poom_scanner_core`.
- UI: mode select + channel list view with activity bars.
- I/O: OLED-only.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### SCAN NET (`menu_poom_wifi_arp.c`, `menu_poom_wifi_arp_show()`)
- What it is: local network discovery/ARP tooling for diagnostics on authorized networks.
- Subsystems: `poom_wifi_ctrl` (connectivity), `poom_wifi_arp`, `poom_secrets_store` (remembered settings).
- UI: list/detail screens for network parameters and results.
- I/O: OLED status; may log to UART for debugging.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### CLI (`menu_cli_nfc.c`, `menu_cli_nfc()`)
- What it is: POOM console entry point currently used to expose the NFC-oriented on-device CLI.
- Subsystems: `poom_cli` (POOM console core), `cli_nfc`, `poom_nfc`.
- UI: shows minimal status/ownership; primary interaction is via the CLI interface.
- I/O: UART console; may use NFC peripherals.
- Exit/return: returns to launcher and publishes `poom/menu/resume`.

#### HTTP LOAD (`menu_http_load_test.c`, `menu_http_load_test_show()`)
- What it is: on-device HTTP load generator for testing servers you control.
- Subsystems: `poom_http_load_test`, `poom_wifi_ctrl`, `poom_secrets_store` (host/params persistence).
- UI: parameter editing + start/stop + status counters.
- I/O: OLED status; generates HTTP traffic; logs for debugging.
- Exit/return: stops run and publishes `poom/menu/resume`.

#### PROBE REQ (`menu_sniffer_device.c`, `menu_sniffer_device_show()`)
- What it is: Wi‑Fi probe-request monitor UI.
- Subsystems: `poom_sniffer_device`.
- UI: list UI for recent entries + detail screen.
- I/O: OLED status; optional UART logs.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

### THE ZEN - Creative Control Suite

#### MIDI (`menu_midi.c`, `menu_midi_init()`)
- What it is: BLE MIDI controller / performance UI.
- Subsystems: `ble_midi`, `poom_motion_midi`, `i2c` (hardware inputs), SBUS buttons.
- UI: on-device controller screen; behavior depends on current MIDI mapping/profile.
- I/O: BLE MIDI output; OLED status.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### HARMONY (`menu_midi_harmony.c`, `menu_midi_harmony_init()`)
- What it is: MIDI “player” UI for structured sequences/chords.
- Subsystems: `ble_midi`, `poom_midi_player`, `sd_card`, `i2c`.
- Storage: reads `.json` harmony definitions from `POOM_MIDI_HARMONY_DIR` (`/sdcard/harmonies`).
- I/O: BLE MIDI output; OLED status.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### TONE (`menu_tone.c`, `app_buzzer_menu()`)
- What it is: buzzer/tone browser + player.
- Subsystems: `poom_buz_theme`, `sd_card`, `cJSON`.
- Storage: scans/creates `/sdcard/tones` and loads `.tone` files (JSON event lists).
- I/O: buzzer audio output; OLED file list/status.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### CONTROL (`menu_control_music.c`, `menu_control_init()`)
- What it is: “media keys” style BLE HID control (play/pause, next/prev, etc.).
- Subsystems: `poom_ble_keyboard` (HID), SBUS buttons.
- I/O: BLE HID events; OLED status.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### NFC (`menu_nfc.c`, `menu_nfc_show()`)
- What it is: NFC reader/emulator tooling UI.
- Subsystems: `poom_nfc_controller`, `poom_nfc_dump`, `poom_nfc_emulator`, `poom_nfc_store`, `sd_card`.
- Storage: supports SD card `.nfc` files and on-device store, depending on selected mode.
- I/O: OLED status; NFC I/O; optional UART logs.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### IR UNIV (`menu_ir_universal.c`, `menu_ir_universal_show()`)
- What it is: universal IR remote (learn/store/transmit) UI.
- Subsystems: `ir`, `ir_tx`, `ir_dec`, `poom_led_rainbow`, `poom_sd_browser`, `poom_secrets_store`.
- Storage: browses `.ir` files from SD (start directory defaults to `/sdcard`); remembers selections via secrets store.
- I/O: IR RX/TX; OLED status; optional UART logs for debugging.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### POOM WEB (`menu_cli_web.c`, `menu_cli_web_show()`)
- What it is: local web UI + device-side CLI bridging.
- Subsystems: `poom_web`, `esp_console`, VFS; can integrate NFC CLI (`cli_nfc`) depending on build.
- I/O: HTTP server + OLED status; may expose limited CLI endpoints for local network use.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

### THE GAMER - Gaming Applications

#### TINY CONTROL (`menu_ble_control.c`, `menu_control_display()`)
- What it is: minimal BLE HID “keyboard-like” controller UI intended for games.
- Subsystems: `poom_ble_keyboard`.
- UI: pairing/connected status; exits via chord (LEFT+RIGHT) so `B` can be used as a HID key.
- I/O: BLE HID events; OLED status.
- Exit/return: stops BLE keyboard module and publishes `poom/menu/resume`.

#### WII (`menu_air_ble.c`, `menu_air_ble_display()`)
- What it is: “POOM WII” controller mapping (sends keypresses over the `poom_wii` backend).
- Subsystems: `poom_wii`.
- UI: init/running state screen; sends key presses while buttons are held.
- I/O: host-side key events over the configured transport; OLED status.
- Exit/return: stops the backend, releases keys, publishes `poom/menu/resume`.

### THE MAKER - Development Tools

#### PLOT (`menu_plot.c`, `menu_plot_init()`)
- What it is: quick entry point to live plots (IMU stream/plot and BLE plotting tools).
- Subsystems: `poom_imu_stream`, `poom_imu_plot`, `poom_ble_plot`, `poom_imu_stream`, `poom_led_rainbow`.
- I/O: OLED plot UI; optional BLE streaming depending on mode.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### BLE SCAN (`menu_ble_scan.c`, `app_ble_scan()`)
- What it is: BLE scanner UI for discovering nearby devices/advertisements.
- Subsystems: `poom_ble_scan` + ESP-IDF GAP BLE.
- I/O: OLED list; BLE scan traffic; optional UART logs.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### DRONE SCAN (`menu_poom_drone_scan.c`, `menu_poom_drone_scan_show()`)
- What it is: RemoteID scanner UI (lab tool; integrates with drone modules).
- Subsystems: `poom_drone` (scanner), timer/queue based UI refresh.
- Storage: optional “PCAP to SD” capture toggles exist in config (defaults off).
- I/O: OLED list + details; optional SD logging when enabled.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### DRONE EMUL (`menu_poom_drone_emul.c`, `menu_poom_drone_emul_show()`)
- What it is: RemoteID emulator UI for controlled testing of the scanner.
- Subsystems: `poom_drone_emul` (emulation), `poom_drone` (shared types).
- I/O: OLED status; emits test beacons/frames per configured emulation mode.
- Exit/return: stops emulation and publishes `poom/menu/resume`.

#### DRONE ID (`menu_poom_droneid.c`, `menu_poom_droneid_show()`)
- What it is: RemoteID detail/debug UI + optional capture toggles.
- Subsystems: `poom_drone`, `sd_card`.
- Storage: can enable/disable “PCAP to SD” capture in UI (default OFF).
- I/O: OLED status/detail screens; optional SD logging when enabled.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### I2C
- What it is: on-device I2C scan is exposed as the external app `menu_i2c_scan_show()`.
- Note: the launcher only delegates to this app from Maker; the scan UI no longer lives in `applications/poom_menu/src/poom_menu.c`.

#### LUA (`menu_lua.c`, `menu_lua_show()`)
- What it is: Lua runner UI.
- Subsystems: `poom_lua`, `poom_sd_browser`.
- Storage: default entry path is `/sdcard/main.lua`; file browser filters `.lua`.
- I/O: OLED status; script output/logs typically go to UART depending on script.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

#### EDGE AI (`menu_edge_impulse.c`, `menu_edge_impulse_show()`)
- What it is: Edge Impulse integration UI (run model, show status/results).
- Subsystems: `poom_edge_impulse`.
- I/O: OLED status; model-specific logs may go to UART.
- Exit/return: exits to launcher and publishes `poom/menu/resume`.

### SETTINGS - System Configuration

#### DFU (`menu_dfu.c`, `dfu_start_task()`)
- What it is: device firmware update (DFU) modal UI.
- Subsystems: `poom_fw_update` + `dfu` + `poom_dfu_log` (uses a local update flow, typically via `poom.local`).
- UI: shows the AP SSID/password to connect to, then progress/status screens.
- I/O: OLED progress/status; firmware update traffic depending on configured transport.
- Exit/return: does not publish `poom/menu/resume`; successful updates reboot, errors remain on the DFU screen.

#### FW INFO (`menu_fw_info.c`, `menu_fw_info_show()`)
- What it is: firmware metadata screen (build version + OTA slot/partition info).
- Subsystems: `esp_app_desc`, `esp_ota_ops`.
- I/O: OLED-only.
- Exit/return: publishes `poom/menu/resume` when exiting.

#### IMU (`menu_imu_monitor.c`, `menu_imu_monitor_show()`)
- What it is: IMU stream/monitor UI.
- Subsystems: `poom_imu_stream`.
- I/O: OLED readings/status; optional UART logs.
- Exit/return: publishes `poom/menu/resume` on exit.

#### LED RGB (`menu_ws2812_color.c`, `menu_ws2812_color_show()`)
- What it is: WS2812/RGB control UI.
- Subsystems: `ws2812`, `poom_led_rainbow`, board pin map (`bsp_pong.h`).
- I/O: LED output + OLED status.
- Exit/return: publishes `poom/menu/resume` on exit.

#### WI‑FI (`menu_poom_wifi_scan.c`, `menu_poom_wifi_scan_show()`)
- What it is: Wi‑Fi scan + connect/config UI for legitimate network management.
- Subsystems: `poom_wifi_ctrl`, `poom_wifi_scanner`, `poom_ui_keyboard`, `poom_secrets_store`.
- Storage: stores network credentials/settings via secrets store.
- I/O: OLED status; Wi‑Fi connectivity.
- Exit/return: publishes `poom/menu/resume` on exit.

#### NFC TUNE (`menu_nfc_tuning.c`, `menu_nfc_tuning_show()`)
- What it is: NFC controller diagnostics/tuning UI.
- Subsystems: `poom_nfc_controller`.
- I/O: OLED status; interacts with NFC front-end hardware.
- Exit/return: publishes `poom/menu/resume` on exit.

#### FILES (`menu_sd_browser.c`, `app_sd_browser_menu()`)
- What it is: SD file browser UI.
- Subsystems: `poom_sd_browser`, `sd_card`.
- I/O: OLED list UI; file reads/writes depend on invoked actions.
- Exit/return: publishes `poom/menu/resume` on exit.

## Dependencies

Defined in `applications/poom_app_pack/CMakeLists.txt`:

- UI + control plane: `poom_sbus`, `poom_arduboy_display`, `button_driver`
- Persistence: `poom_secrets_store`
- Storage: `sd_card`, `poom_sd_browser`
- Wireless: `poom_wifi_ctrl`, `poom_wifi_scanner`, `poom_ble_scan`, `poom_ble_keyboard`, `poom_ble_spam`, `poom_ble_tracker`
- NFC/IR: `poom_nfc`, `ir` (IR TX/RX are both used by the IR app through this component)
- Tooling apps: `poom_pcap`, `poom_scanner_core`, `poom_sniffer_device`, `poom_edge_impulse`, `poom_http_load_test`, `poom_web`, `poom_lua`, `poom_midi`

## Entry points (public headers)

Each app has a small “launch” function declared in the matching header under `applications/poom_app_pack/include/`.
The **Applications Reference** above already lists the canonical entry point next to each `.c` file.

Notes:
- Naming isn’t fully uniform historically: some entries are `menu_*_show()`, others are `app_*()` or `*_init()`.
- DFU is started via `dfu_start_task()` from `applications/poom_app_pack/include/menu_dfu.h` (it is modal and does not return to the launcher).

## Runtime Behavior

- Most apps follow the same lifecycle:
  - Subscribe to `input/button` and render an initial frame
  - Run until an exit condition occurs (often `B`, sometimes a chord)
  - Stop/cleanup their subsystem(s)
  - Publish a token to `poom/menu/resume` so the launcher re-attaches its input handlers

## Firmware Version

Firmware version is provided by ESP-IDF `esp_app_desc_t.version` (build-time `PROJECT_VER`).

- **FW INFO** screen shows version/build metadata and OTA slots.
- DFU mode shows the current version via the OTA status JSON (`current_fw_version`).
- The runtime `get_version` tool returns `POOM <PROJECT_VER>`.

- **Navigation Controls**:
  - LEFT/RIGHT: Switch between menu categories
  - UP/DOWN: Navigate within category applications
  - A: Select and launch application
  - B: Exit menu (when applicable)

- **Application Launching**:
  - Detaches menu task to free resources
  - Publishes launch message to SBUS
  - Calls application-specific initialization
  - Applications run independently until completion

- **Menu Categories**:
  - **THE BEAST**: Wireless security testing tools
  - **THE ZEN**: Creative and control applications
  - **THE GAMER**: Gaming and controller applications
  - **THE MAKER**: Development and prototyping tools
  - **SETTINGS**: System configuration and utilities

## Integration

The menu system integrates with:

- **SBUS**: For inter-module communication and event handling
- **OLED Display**: For visual interface rendering
- **Button Driver**: For user input processing
- Prefer clean stop on exit: unsubscribe SBUS handlers, stop radio modules, and release IR or BLE resources.
- Keep OLED text short because most UIs assume about 18 to 22 characters per line depending on font.
- When adding SD-based content, document the directory, expected file extension, and whether files are created automatically or must be provisioned first.
- **Application Modules**: Individual feature implementations
- **System Services**: BLE, WiFi, NFC, and hardware peripherals
