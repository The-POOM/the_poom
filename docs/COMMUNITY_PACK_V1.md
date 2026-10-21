# POOM Community Pack v1

Community additions for POOM firmware 1.0.13 (ESP32-C5).

## Features

### Fox Hunt
- Hunt BLE advertisers, Wi-Fi access points, or Wi-Fi clients using received signal strength (RSSI).
- Signal-strength heat levels, trends, LED colour and audible feedback.
- Adjustable LED brightness and audio volume, with preferences saved locally.
- Target selection by discovered device; navigation into hunts from the Wi-Fi and BLE detector screens.
- **Fox Hunt controls:** UP/DOWN navigate lists and A selects; B returns/back (or cancels an active Wi-Fi scan). On target lists LEFT toggles between device name/OUI identification and the raw MAC address. During an active hunt UP/DOWN adjust audio volume and LEFT/RIGHT adjust LED brightness in 20% increments. Audio/LED levels are also configurable from Fox Hunt > OPTIONS and saved locally.
- **Wi-Fi client hunt controls (authorised testing only):** Hold A and press UP to toggle continuous deauthentication; hold A and press DOWN for a one-off burst. These controls do not apply to BLE hunting. In a BLE hunt, A is a modifier but its deauthentication combinations have no action.
- Wi-Fi client hunting includes optional manual bursts and a continuous deauthentication option intended **only for networks and devices you own or are explicitly authorised to test**.

### Wi-Fi detection and client inspection
- Access-point client discovery and selection, with device detail and client-hunt hand-off.
- **OUI identification:** manufacturer/vendor lookup from the bundled OUI database for supported, globally assigned MAC addresses. Randomised/private MAC addresses cannot reliably identify a manufacturer.
- **MAC/OUI display toggle:** LEFT switches detector list entries between a friendly name/vendor (OUI) view and the raw MAC/BSSID view, with on-screen hints such as `L:MAC` and `L:OUI`.
- Device aliases and client actions from the detector interface.
- **Select exactly which clients are included:** in **WiFi Detect → AP Clients**, use **RIGHT** to tag or untag individual clients, building a selected set instead of affecting every connected device. **LEFT** independently toggles the MAC/OUI display mode.
- **Deliberate start control:** with clients tagged, **long-press A** on the client list to launch the selected-client deauthentication action; a normal A tap opens the selected client's details instead. Targeted and multi-client deauthentication can interrupt network connectivity and must only be used on authorised networks.

### BLE detection
- Name/OUI versus raw MAC address list views (**LEFT** to toggle), aliases, and an entry point to BLE Fox Hunt.
- **BLE detector controls:** UP/DOWN select a device, A opens its details, RIGHT on the detail screen starts a hunt for that BLE target, and B goes back. In the BLE detail view, LEFT toggles the displayed identification mode and A opens alias editing.

### Wi-Fi profiles
- Save, select, connect to and delete multiple locally stored Wi-Fi profiles, using the firmware's existing secrets-store facilities.
- Wi-Fi credentials are not embedded in the contributed source files.

### Interface and input
- **Rearrange menu entries** separately within the POOM categories (Beast, Zen, Gamer, Maker and Settings). The custom order is saved in NVS and persists across restarts. Use RIGHT to enter move mode in a submenu, UP/DOWN to reposition an item, A to save or B to cancel. Settings also provides `MOVE MENU` to select a category and `RESET ORDER` to restore defaults (with confirmation).
- **Menu navigation:** per-category menu rearranging is available directly from a submenu with RIGHT to enter move mode; the Settings menu also exposes a category picker and a confirmed reset to default order.
- **On-screen keyboard:** LEFT/RIGHT navigation wraps at the end of a row; UP/DOWN moves to the nearest key in the adjacent row. Press UP from the top keyboard row to focus the text field, then LEFT/RIGHT to position the insertion cursor and DOWN to return to the keys. Insert or backspace characters within existing text instead of only at the end. Long entries scroll horizontally to keep the cursor visible; the numeric/symbol layout also includes `:` for addresses and similar input.
- **Buzzer/audio:** added a `buzzer_set_volume(percent)` PWM-duty control (0–100%, with 0 silent) so Fox Hunt can offer adjustable audible feedback. This is a driver-level capability used by Fox Hunt, **not** a new system-wide volume settings menu.
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
- This is a combined community contribution; maintainers may prefer to split it into smaller PRs for review.
