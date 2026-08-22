# poom_drone_emul

`poom_drone_emul` is a RemoteID (OpenDroneID) emulator for local testing. It transmits **Wi‑Fi (Beacon + NAN)** frames carrying a Message Pack, and optionally **BLE** advertisements.

## Purpose

- **Wi‑Fi Beacon (RemoteID Message Pack)**: Vendor IE (ASTM) with `oui_type=0x0D`.
- **Wi‑Fi NAN**:
  - NAN Sync Beacon (discovery, ~1 Hz).
  - NAN Action Frame with a Message Pack (per drone, with `message_counter`).
- **BLE (optional)**: Service Data UUID `0xFFFA` + app code `0x0D` + counter + ODID message (one ODID message per ADV, rotates message type).

Note: AP mode is primarily used to keep the radio active and allow `esp_wifi_80211_tx()`; it is not meant to provide real connectivity.

## Public API

Header: `applications/poom_drone_emul/include/poom_drone_emul.h`

- `poom_drone_emul_config_default(poom_drone_emul_config_t *out_cfg)`
- `poom_drone_emul_start(const poom_drone_emul_config_t *cfg)`
- `poom_drone_emul_stop(void)`
- NVS persistence:
  - `poom_drone_emul_set_location(double lat, double lon)` (stores lat/lon)
  - `poom_drone_emul_set_count(uint8_t count)` (1..16)
  - `poom_drone_emul_set_ble_enabled(bool enabled)`

Important defaults:
- SSID: `"POOM DRONE"`
- Channel: `6`
- Location: **San Francisco, USA** (37.7749, -122.4194)
- Count: `1`
- BLE: `OFF`

## Usage

Menu: `THE MAKER -> DRONE EMUL`

Controls (emulator screen):
- `UP/DOWN`: select row
- `LEFT/RIGHT`: change value for selected row
  - `State`: START/STOP
  - `Qty`: 1..16
  - `BLE`: ON/OFF
  - `Loc`: read-only (adjust via API/CLI if available)
- `B`: exit

## Integration

- For quick validation, use two POOM devices:
  1) One running `DRONE EMUL` (TX).
  2) One running `DRONE SCAN` (Wi‑Fi, and optionally BLE via `LR:BLE`).
- If your app/receiver only detects NAN (and not beacon), this module already emits both.
- If you only see 1 drone even when `Qty>1`: some generic “Wi‑Fi scanners” will only show a single AP; the real goal is that a **RemoteID scanner** sees multiple emitters (per MAC/ID).
