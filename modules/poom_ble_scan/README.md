# poom_ble_scan

## Purpose

`poom_ble_scan` provides a generic BLE advertisement scanner built on top of `poom_ble_gatt_client`.

## Responsibilities

- Configure BLE scan type and filter policy.
- Receive advertising reports from the BLE GAP callback path.
- Forward results to user callbacks.
- Optionally serialize BLE advertising packets through `poom_uart_sniffer`.

## Features

- Passive or active BLE scan modes.
- Callback registration for application/UI integration.
- Optional UART forwarding compatible with `poom_uart_sniffer`.
- Clean start/stop wrapper around the shared BLE client helper.

## Public API Overview

Header: `modules/poom_ble_scan/include/poom_ble_scan.h`

- `void poom_ble_scan_register_cb(poom_ble_scan_result_cb_t callback)`
- `void poom_ble_scan_set_filter_type(esp_ble_scan_filter_t filter_type)`
- `void poom_ble_scan_set_scan_type(esp_ble_scan_type_t scan_type)`
- `void poom_ble_scan_set_uart_forward_enabled(bool enabled)`
- `void poom_ble_scan_start(void)`
- `void poom_ble_scan_stop(void)`
- `bool poom_ble_scan_is_active(void)`

## File Structure

```text
modules/poom_ble_scan
├── CMakeLists.txt
├── component.mk
├── README.md
├── include/
│   └── poom_ble_scan.h
└── poom_ble_scan.c
```

## Integration Notes

- Add `poom_ble_scan` in `REQUIRES` where this API is used.
- Depends on `poom_ble_gatt_client`, `poom_uart_sniffer`, and `bt`.
- The callback receives a stack-owned `esp_ble_gap_cb_param_t`; consume it immediately and do not retain the pointer.

## Configuration Options

- `CONFIG_POOM_BLE_SCAN_ENABLE_LOG`
  Enables local POOM log macros when defined in project configuration.

## Logging Behavior

- Uses POOM log format with tag `poom_ble_scan`.
- Logs scanner start/stop and basic scan lifecycle events when enabled.
