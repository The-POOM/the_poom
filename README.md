# POOM

POOM is an open-source multitool platform built to **Pentest. Play. Create.**

It is designed for makers, security learners, gamers, and engineers who want a compact device capable of real embedded workflows across wireless security, data capture, automation, and interaction.

## Product Summary

POOM is delivered as an ESP-IDF firmware project with a modular architecture and multiple operating domains:

- Maker Mode
- ZEN Mode
- Sniffer and Analyzer
- Wireless Toolkit (Wi-Fi, Bluetooth, Aerial)

## Core Capabilities

### Maker Mode

- I2C scanner for rapid Qwiic-style peripheral discovery
- Sensor streaming to automation platforms (n8n, Node-RED, Home Assistant)
- Data capture and visualization pipelines for Edge Impulse and external plotting tools
- Embedded APIs for dataset collection and feature testing

### ZEN Mode

- NFC read and experimentation workflows
- BLE MIDI motion control
- Controller mode for apps, media, and presentations
- Compact gaming integrations (for example Snake and portable-console flows)

### Beast Mode

- Wi-Fi scan, deauth testing (authorized), karma, captive/evil twin, SSID spam, ARP spoofing.
- BLE spam and BLE proximity/tag tracking.
- Wi-Fi, BLE, Zigbee, and 802.15.4/Thread capture paths.
- ART and host export pipelines for offline analysis.
- Drone ID and drone research features.

### Gamer Mode

- BLE gamepad and IMU motion control.
- Snake and portable-console flows.
- Compact gaming integrations built for the device in your hand
- Arduboy Library Support
  

## Platform and Targets

- Framework: ESP-IDF `v6.1` — see note

  > **Note:** upstream esp-idf is tagged `v6.1`. There is **no `v6.1.0` tag**, so
  > `git checkout v6.1.0` fails.
  >
  > ⚠ **Needs confirmation:** a clean `esp32c5` build currently *fails* on v6.1 —
  > `espressif/button` 3.x cannot compile because `driver/gpio.h` was removed when
  > the driver component was split into `esp_driver_*` in IDF 6.x, and button 4.x
  > requires porting `drivers/button_driver/`. The supported IDF version should be
  > verified and stated here.

- **Shipping hardware target: `esp32c5`** — ESP32-C5, dual-band 2.4/5 GHz Wi-Fi 6,
  Bluetooth 5 (LE), and IEEE 802.15.4 (Zigbee/Thread)
- Additionally builds for: `esp32c6`

> **Build the `esp32c5` target for production devices.** The C5 is what ships.
> C6 is retained for development/alternate boards; a C6 image will not run on a
> C5 device.

## Repository Structure

```text
.
├── applications/          # Product applications and end-user features
├── modules/               # Reusable POOM modules
├── drivers/               # Hardware-facing drivers
├── third-party/           # Integrated external components
├── kernel/                # Internal runtime and system utilities
├── board/                 # Board support / BSP definitions
├── bootloader_components/ # Custom bootloader components
├── main/                  # Firmware entry point
├── sdkconfig.defaults     # Base configuration
├── sdkconfig.zigbee       # Zigbee overlay
├── sdkconfig.openthread   # OpenThread overlay
├── partitions.csv         # Custom partition table
└── CMakeLists.txt         # Root build orchestration
```

## Build and Flash

```bash
# Clone the matching framework version (the tag is v6.1, not v6.1.0)
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf"
"$HOME/esp/esp-idf/install.sh" esp32c5
. "$HOME/esp/esp-idf/export.sh"

# Build for ESP32-C5 (shipping hardware)
idf.py set-target esp32c5
idf.py build

# Flash and monitor
idf.py flash monitor
```

### Optional: build for ESP32-C6 (development boards)

```bash
idf.py set-target esp32c6
idf.py build
```

### Optional: Zigbee / OpenThread overlays

Zigbee and OpenThread are mutually exclusive in RAM budget, so they are supplied
as separate overlays on top of `sdkconfig.defaults`:

```bash
# Zigbee
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.zigbee" reconfigure
idf.py build

# OpenThread
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.openthread" reconfigure
idf.py build
```

## Security and Legal

POOM includes offensive-security capabilities intended strictly for:

- Authorized penetration testing
- Controlled laboratory environments
- Educational and defensive research

Use only with explicit permission and in compliance with local laws and regulations.

## Contributing

Contributions are welcome. For consistency:

- Follow `poom_*` naming conventions
- Keep code and documentation in English
- Update component/application READMEs when behavior changes
- Prefer production-grade C style, clear APIs, and maintainable interfaces
