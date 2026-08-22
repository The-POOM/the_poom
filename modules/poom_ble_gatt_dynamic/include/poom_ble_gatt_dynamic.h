// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_BLE_GATT_DYNAMIC_SERVICE_UUID_DEFAULT         (0x00FFU)
#define POOM_BLE_GATT_DYNAMIC_CHAR_UUID_DEFAULT            (0xFF01U)
#define POOM_BLE_GATT_DYNAMIC_CHAR_VAL_MAX_LEN_DEFAULT     (0x80U)
#define POOM_BLE_GATT_DYNAMIC_DEVICE_NAME_DEFAULT          "POOM_GATT"

typedef struct
{
    const char *device_name;
} poom_ble_gatt_dynamic_props_t;

typedef struct
{
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t char_perm;
    esp_gatt_char_prop_t char_property;
    esp_attr_value_t char_val;
} poom_ble_gatt_dynamic_char_config_t;

typedef struct
{
    esp_bt_uuid_t service_uuid;
    size_t char_count;
    const poom_ble_gatt_dynamic_char_config_t *chars;
} poom_ble_gatt_dynamic_profile_params_t;

typedef struct
{
    esp_ble_adv_params_t adv_params;
    poom_ble_gatt_dynamic_props_t bt_props;
    poom_ble_gatt_dynamic_profile_params_t profile;
} poom_ble_gatt_dynamic_config_t;

esp_attr_value_t poom_ble_gatt_dynamic_default_char_val(void);
esp_ble_adv_params_t poom_ble_gatt_dynamic_default_adv_params(void);
void poom_ble_gatt_dynamic_set_config(const poom_ble_gatt_dynamic_config_t *config);
esp_err_t poom_ble_gatt_dynamic_start(void);
void poom_ble_gatt_dynamic_stop(void);
bool poom_ble_gatt_dynamic_is_started(void);

#ifdef __cplusplus
}
#endif
