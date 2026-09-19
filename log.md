## POOM 1.0.9

This release expands POOM's NFC capabilities with improved NTAG and MIFARE Classic support, adds a dedicated Amiibo workflow, and introduces the new GAME STORE for managing and installing games directly from POOM.

### NFC / NTAG

- Improved full reading support for **NTAG213, NTAG215 and NTAG216**, including tag detection, correct page count, version information and signature reading.

- Improved NTAG identification to correctly distinguish between supported tag variants.

### AMIIBO

- Added a dedicated **Amiibo** menu inside NFC with options to copy, save, select and use Amiibos.

- Amiibos can now be stored on the SD card as `.nfc` files under `/nfc/MyAmiibo`.

- Implemented and improved **Amiibo emulation**, including NTAG215 authentication compatibility.

- Improved handling of Amiibo data during loading and emulation.

### MIFARE CLASSIC

- Fixed **MIFARE Classic** reading, with important improvements for **4K cards**.

- Improved key searching and authentication handling.

- Improved cancellation during long MIFARE Classic operations to avoid unnecessary waits or blocked operations.

### GAME STORE

- Added **GAME STORE** to the `GAMER` menu.

- POOM can now browse the available game catalog directly from the device.

- Added support for downloading game covers and game files.

- Added an offline game library using previously downloaded catalog information and assets.

- Games can now be installed directly from the SD card.

### SETTINGS / SD

- Added **APP CLEAN** under `Settings > SD`.

- `APP CLEAN` removes games, covers and cached data stored under `/sdcard/apps`.

- After cleanup, POOM automatically recreates the empty `/sdcard/apps` directory.

### Community Contribution
Special thanks to THNRGLABS for reporting issues and helping improve POOM.



## POOM 1.0.8

Documented period: September 8–11, 2026.

Version: `1.0.8`.

### NFC / EMV

- Implemented full EMV reading: PPSE discovery, application/AID selection, PDOL, GPO, AFL, and `READ RECORD`, including cards that return data directly in the GPO response.
- Added separate Visa and Mastercard support, decoding the available EMV data without mixing scheme-specific tags. Cards that restrict access still produce a useful partial result.
- Fixed ISO-DEP communication by using the FWT from the ATS and handling WTX requests, preventing false timeouts during long exchanges.
- The display now supports selecting among multiple applications, scrolling through all retrieved data, and viewing a masked PAN.
- Version 2 `.nfc` files preserve NFC identification, the full PAN, decoded EMV fields, and the raw capture of every APDU command and response.
- Optimized RAM usage with dynamic allocation for EMV details and captures, releasing that memory when finished.
- Fixed Mastercard file saving on FATFS by using names that fit its limit: `EMV_<UID>_MC.nfc`, `EMV_<UID>_VI.nfc`, or `EMV_<UID>_UN.nfc`.
- Added the `nfc-emv-discover`, `nfc-emv-select <AID>`, and `nfc-emv-read` CLI commands for diagnostics.

### MIDI

- Removed redundant I2C locks from the MIDI and MIDI Harmony screens, leaving OLED updates under the display driver's control.

### Community Contribution
Special thanks to THNRGLABS for reporting issues and helping improve POOM.

## POOM 1.0.7

This release expands POOM's passive Wi-Fi and BLE detection tools and improves real-time device tracking and channel analysis.

### Added

### BLE DETECT

* **BLE DEVICES** — nearby BLE device inventory.
* **TRACKERS** — detects candidates compatible with AirTag, SmartTag, Tile and Find My devices.
* **WEARABLES** — detects candidates such as Ray-Ban Meta, Snap Spectacles and BLE-enabled body cameras.

### WIFI DETECT

* **WIFI DEVICES** — nearby APs with SSID, RSSI, channel, security and BSSID.
* **AP CLIENTS** — passively observes active clients communicating with a selected AP.
* **FLOCK / ALPR** — local detection using known Wi-Fi signatures, OUIs and Probe Request patterns.
* **IP CAMERAS** — identifies possible camera devices using OUI, SSID and passive Wi-Fi traffic.

### WIFI AIR

Available from `THE BEAST > SCAN CHANNELS > WIFI`. Shows real-time channel activity including **frames, retries, deauths**, and a **Frame Mix** view with **Data, Management, Control, RTS and CTS** traffic.

## Community Contribution

### PicoPass — Eric

* Added a **PicoPass dictionary attack**.
* PicoPass files can now be saved with a custom name using POOM's on-screen keyboard.
* Press `UP` from the Info screen to enter a name.
* Empty names fall back to the card CSN.
* `B` cancels and returns to the Info screen.
* File names are sanitized and limited to FATFS-safe lengths.

Hardware tested successfully.

Thanks Eric for the contribution!

Special thanks to **THNRGLABS** for the ideas and inspiration that helped shape some of these new features.

Thanks to everyone testing POOM, reporting issues, contributing code and sharing ideas.
