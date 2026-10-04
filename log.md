## POOM 1.0.11

This release expands **THE BEAST** and **NFC** tools with a redesigned Captive Portal workflow, TV OFF IR control and deauthentication tools, expanded PCAP capture modes and deeper EMV card analysis.

### CAPTIVE PORTAL

- Redesigned the **Captive Portal** workflow under **THE BEAST → CAPTIVE PORTAL**.

- Added separate **Start**, **Settings** and **Scan SSID** options instead of immediately starting the portal from the initial selection.

- Added configurable **AP Name** under **Settings**, allowing the portal access point name to be entered manually or copied from a WiFi scan.

- Added selectable portal HTML files under **Settings → Portal**.

- Added SD card browsing filtered to `.html` files.

- Portal files are loaded from `/sdcard/portals`, which is automatically created when missing.

- Added **Scan SSID** to scan nearby WiFi networks and copy the selected SSID as the Captive Portal AP name.

- Selecting an SSID no longer automatically starts the portal.

### CAPTIVE PORTAL CLIENTS

- Added a real-time **Clients** view while the Captive Portal is running.

- Added online status indication for connected clients.

- Added client information including detected name/type, IP address, MAC address and RSSI.

- Added per-client detail view for inspecting connected devices.

- Improved IP-to-MAC correlation for connected clients.

### TV OFF

- Added the new **TV OFF** application under **THE BEAST → TV OFF**.

- Added transmission of common infrared **Power / Off** commands for nearby televisions.

- Added support for Samsung, NEC, NEC Extended, RC5, RC6 and SIRC infrared protocols.

- Added a visual progress bar showing transmitted commands and the current protocol or brand.

- The **B button** stops the transmission sequence.

- The **A button** repeats the TV OFF sequence after completion.

### DEAUTH DETECTION

- Improved **THE BEAST → DEAUTH DET** with visual and audible alerts.

- Added LED notification when deauthentication activity is detected.

- Added buzzer notification when deauthentication activity is detected.

- Added settings to independently enable or disable detection alerts.

### PCAP SNIFFER

- Expanded **THE BEAST → SNIFFER** with additional packet capture modes.

### WIFI PCAP

- Added additional WiFi packet capture filters and modes.

- Added dedicated **HANDSHAKE** capture mode.

- Handshake mode captures Beacon / Probe Response, EAPOL traffic, WiFi handshakes and PMKID information.

- Captured handshake data is stored under `/sdcard/pcaps/handshakes`.

- Added automatic export to **Hashcat `.22000` format**.

- The `.22000` export supports both **PMKID (`WPA*01`)** and **EAPOL (`WPA*02`)** entries.

- Multiple detected PMKIDs and handshake sessions can be stored in the same `.22000` export.

### NFC / EMV

- Expanded EMV analysis under **THE ZEN → NFC**.

- Added additional EMV information including card scheme, expected kernel, AIP, CVM, AFL records and recognized EMV tags.

- Added parsing and display of **PDOL**, **CDOL** and **DDOL** information.

- Added additional optional EMV application data when available.

### EMV CARD SCHEMES

- Added support for identifying additional EMV card families and AIDs.

- Added **American Express**, **JCB**, **Discover / Diners Club**, **UnionPay**, **Interac**, **Cartes Bancaires**, **eftpos** and **RuPay**.

### EMV CVM DETAILS

- Added detailed **Cardholder Verification Method** information.

- Added detection of offline PIN, online PIN, signature and No CVM rules.

- Added CVM condition information including transaction amount and cashback-related conditions when available.

### NFC EXPORT

- Improved NFC scan and save output.

- Saved NFC files now include a more complete EMV summary.

- Added parsed TLV tree information.

- Added CVM rules and recognized tag names.

- Added certificate and optional EMV data when available.

### Community Contribution

Special thanks to **THNRGLABS** for suggesting the **audible alert for Deauth Detection**, helping improve real-time deauthentication notifications.

Special thanks to **GohanAMP** for suggesting the **Captive Portal client monitor**, including the ability to view connected clients and inspect their details directly from the portal interface.


## POOM 1.0.10

This release introduces the new **Motion MIDI** application, allowing POOM to use IMU motion as a BLE MIDI controller for drums and melodic gestures.

### MOTION MIDI

- Added the new **Motion MIDI** application using the onboard IMU and BLE MIDI.

- Added **DRUM mode** with motion-triggered percussion using MIDI channel 10.

- Added support for Kick, Snare, Closed Hi-Hat, Open Hi-Hat, Tom and Crash sounds.

- Added downward strike detection with hit-strength-based MIDI velocity.

- Improved gesture filtering to reduce false triggers and prevent the upward return motion from generating duplicate hits.

### MELODY MODE

- Added **MELODY mode** using device tilt to select musical notes over MIDI channel 1.

- For the best experience in **GarageBand**, use a **Piano** instrument when playing in MELODY mode.

- Added **FIXED mode**, which keeps the selected note while the A button is held.

- Added **MOVING mode**, allowing the selected note to follow device tilt while playing.

- Added Major Pentatonic, Minor Pentatonic, Major and Minor scales.

- Added configurable tonic, octave and velocity intensity.

- Added tilt smoothing and hysteresis to prevent unwanted note changes near note boundaries.

### CALIBRATION / IMU

- Added motion calibration for gyro bias, gravity direction and player reference posture.

- Added startup calibration and manual recalibration from the Motion MIDI menu.

- Improved IMU processing for short drum strikes and orientation tracking.

- Added stale sensor detection with automatic recovery when valid IMU data returns.

- Temporary sensor read interruptions no longer reset the calibrated playing position.

### BLE MIDI

- Improved BLE MIDI note handling for pause, disconnect and application exit.

- Active notes are now automatically released when leaving Motion MIDI or when the BLE connection is lost.

- BLE reconnection does not automatically resume previously held notes.

- Improved MIDI worker handling so UI commands, calibration and note generation are processed safely without concurrent state changes.

### DISPLAY / CONTROLS

- Added Motion MIDI controls using the existing POOM menu layout.

- Added selectable parameters using UP / DOWN and LEFT / RIGHT.

- The A button controls actions such as arm, play and calibration.

- The B button always exits Motion MIDI and safely releases active MIDI notes.

- Added status information for BLE connection, armed state, pause state, current note and strike velocity.


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
