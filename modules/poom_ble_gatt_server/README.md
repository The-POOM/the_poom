# poom_ble_gatt_server

`poom_ble_gatt_server` is a BLE GATT server helper component for ESP-IDF.

It provides a compact server setup that:
- initializes BLE stack
- configures advertising and scan response payloads
- creates a service/characteristic/descriptor
- handles read/write/prepare-write events
- sends indications to a connected peer

## Structure

```text
components/poom_ble_gatt_server
├── CMakeLists.txt
├── component.mk
├── poom_ble_gatt_server.c
├── README.md
└── include/
    └── poom_ble_gatt_server.h
```

## Logging

`poom_ble_gatt_server.c` uses `printf` logs controlled by compile-time macros:

- `POOM_BLE_GATT_SERVER_LOG_ENABLED` (default `1`)
- `POOM_BLE_GATT_SERVER_DEBUG_LOG_ENABLED` (default `0`)

If debug is disabled, `POOM_BLE_GATT_SERVER_PRINTF_D(...)` is compiled out.

## Public API

Header: `components/poom_ble_gatt_server/include/poom_ble_gatt_server.h`

- `esp_attr_value_t poom_ble_gatt_server_default_char_val(void)`
- `esp_ble_adv_data_t poom_ble_gatt_server_default_adv_data(void)`
- `esp_ble_adv_data_t poom_ble_gatt_server_default_scan_rsp_data(void)`
- `esp_ble_adv_params_t poom_ble_gatt_server_default_adv_params(void)`
- `void poom_ble_gatt_server_set_adv_data_params(const poom_ble_gatt_server_adv_params_t *adv_params)`
- `void poom_ble_gatt_server_set_callbacks(poom_ble_gatt_server_event_cb_t event_cb)`
- `void poom_ble_gatt_server_send_data(uint8_t *data, int length)`
- `void poom_ble_gatt_server_start(void)`
- `void poom_ble_gatt_server_stop(void)`
