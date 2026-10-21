# POOM Community Pack v1

Community additions for POOM firmware 1.0.13 (ESP32-C5).

## Features

### Fox Hunt
- Hunt BLE advertisers, Wi-Fi access points, or Wi-Fi clients using received signal strength (RSSI).
- Signal-strength heat levels, trends, LED colour and audible feedback.
- Adjustable LED brightness and audio volume, with preferences saved locally.
- Target selection by discovered device; navigation into hunts from the Wi-Fi and BLE detector screens.
- Wi-Fi client hunting includes optional manual bursts and a continuous deauthentication option intended **only for networks and devices you own or are explicitly authorised to test**.

### Wi-Fi detection and client inspection
- Access-point client discovery and selection, with device detail and client-hunt hand-off.
- Device name/SSID versus MAC/BSSID display modes; manufacturer lookup via the bundled OUI database.
- Device aliases and client actions from the detector interface.
- Targeted and multi-client deauthentication controls for authorised testing. These may interrupt network connectivity.

### BLE detection
- Name versus MAC address views, aliases, and an entry point to BLE Fox Hunt.

### Wi-Fi profiles
- Save, select, connect to and delete multiple locally stored Wi-Fi profiles, using the firmware's existing secrets-store facilities.
- Wi-Fi credentials are not embedded in the contributed source files.

### Interface and input
- Menu navigation, keyboard and buzzer improvements.
- Contextual controls and more consistent detector-to-hunt navigation.

## Build and testing

- Based on upstream POOM 1.0.13 (`2b12efd`).
- ESP-IDF build completed successfully for ESP32-C5; `build/poom.bin` generated.
- App image used `0x371a70` bytes out of a `0x3a0000`-byte app partition, leaving `0x2e590` bytes (~5%) spare.
- Working source underwent targeted checks for known personal identifiers; the compiled application image was scanned for the same known identifiers.
- Full hardware regression and compatibility testing have **not** been claimed.

## Notes for maintainers

- The manufacturer/OUI lookup table is large (~57,540 source lines), contributing significantly to code size and diff size. It may merit a separate data-generation or compression approach in future.
- Deauthentication is disruptive; restrict use to authorised environments and applicable laws.
- No personal Sony TV remote application, Net Guardian experiment, Wardrive experiment, or local development notes are included.
- This is a combined community contribution; maintainers may prefer to split it into smaller PRs for review.
