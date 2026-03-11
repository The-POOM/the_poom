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

- Wi-Fi scan, deauth testing (authorized), karma, captive/evil twin, SSID spam, DIAL, ARP spoofing.
-  BLE spam and BLE proximity/tag tracking.
-  Wi-Fi, BLE, Zigbee, and 802.15.4/Thread capture paths. U
-  ART and host export pipelines for offline analysis.
-  Drone ID and drone research features.

### Gamer Mode

- BLE gamepad and IMU motion control.
- Snake and portable-console flows.
- Compact gaming integrations built for the device in your hand
- Arduboy Library Support
  

## Platform and Targets

- Framework: ESP-IDF `v6.1.0`
- Primary targets: `esp32c5`, `esp32c6`
- Current default target in this workspace: `esp32c6`

## Repository Structure

```text
.
├── applications/   # Product applications and end-user features
├── modules/        # Reusable POOM modules
├── drivers/        # Hardware-facing drivers
├── third-party/    # Integrated external components
├── kernel/         # Internal runtime and system utilities
├── main/           # Firmware entry point
└── CMakeLists.txt  # Root build orchestration
```

## Build and Flash

```bash
. "$HOME/esp/esp-idf/export.sh"

# Build for ESP32-C6
idf.py set-target esp32c6
idf.py build

# Build for ESP32-C5
idf.py set-target esp32c5
idf.py build

# Flash and monitor
idf.py flash monitor
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
