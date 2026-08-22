# poom_ble_gatt_dynamic

`poom_ble_gatt_dynamic` is a configurable BLE GATT server module that builds services and characteristics from runtime-provided descriptors.

It is useful when a POOM feature needs to expose custom BLE attributes without hardcoding one fixed profile into a dedicated server component.

## Structure

```text
modules/poom_ble_gatt_dynamic
├── CMakeLists.txt
├── README.md
├── poom_ble_gatt_dynamic.c
└── include/
    └── poom_ble_gatt_dynamic.h
```

## Dependencies

Declared in `modules/poom_ble_gatt_dynamic/CMakeLists.txt`:

* `bt`
* `nvs_flash`

## Public API

Header:
`modules/poom_ble_gatt_dynamic/include/poom_ble_gatt_dynamic.h`

```c
esp_attr_value_t poom_ble_gatt_dynamic_default_char_val(void);
esp_ble_adv_params_t poom_ble_gatt_dynamic_default_adv_params(void);
void poom_ble_gatt_dynamic_set_config(const poom_ble_gatt_dynamic_config_t *config);
esp_err_t poom_ble_gatt_dynamic_start(void);
void poom_ble_gatt_dynamic_stop(void);
bool poom_ble_gatt_dynamic_is_started(void);
```

## Runtime Behavior

When started, the module:

1. initializes BLE stack state,
2. applies the current runtime config,
3. creates the configured GATT service,
4. adds each configured characteristic,
5. starts advertising with the generated payloads.

The profile definition is supplied through `poom_ble_gatt_dynamic_config_t`, including:

* device name,
* advertising parameters,
* service UUID,
* list of characteristics and values.

## Integration

* Use this module when a fixed profile like `poom_ble_gatt_server` is too rigid.
* The implementation has optional local log macros, but they are compiled out by default.
