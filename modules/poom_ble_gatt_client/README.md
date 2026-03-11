# poom_ble_gatt_client

`poom_ble_gatt_client` is a BLE GATT client helper component for ESP-IDF.

It wraps the most common flow:
- initialize BLE stack
- start scanning
- filter by service/characteristic UUID
- optionally match by remote device name
- connect and enable notifications
- send writes to the remote characteristic

## Structure

```text
components/poom_ble_gatt_client
├── CMakeLists.txt
├── component.mk
├── poom_ble_gatt_client.c
├── README.md
└── include/
    └── poom_ble_gatt_client.h
```

## Logging

`poom_ble_gatt_client.c` uses `printf` logs controlled by compile-time macros:

- `POOM_BLE_GATT_CLIENT_LOG_ENABLED` (default `1`)
- `POOM_BLE_GATT_CLIENT_DEBUG_LOG_ENABLED` (default `0`)

If debug is disabled, `POOM_BLE_GATT_CLIENT_PRINTF_D(...)` is compiled out.

## Public API

Header: `components/poom_ble_gatt_client/include/poom_ble_gatt_client.h`

- `esp_bt_uuid_t poom_ble_gatt_client_default_service_uuid(void)`
- `esp_bt_uuid_t poom_ble_gatt_client_default_char_uuid(void)`
- `esp_bt_uuid_t poom_ble_gatt_client_default_notify_descr_uuid(void)`
- `esp_ble_scan_params_t poom_ble_gatt_client_default_scan_params(void)`
- `void poom_ble_gatt_client_set_remote_device_name(const char *device_name)`
- `void poom_ble_gatt_client_set_scan_params(const poom_ble_gatt_client_scan_params_t *scan_params)`
- `void poom_ble_gatt_client_set_callbacks(poom_ble_gatt_client_event_cb_t event_cb)`
- `void poom_ble_gatt_client_send_data(uint8_t *data, int length)`
- `void poom_ble_gatt_client_start(void)`
- `void poom_ble_gatt_client_stop(void)`
