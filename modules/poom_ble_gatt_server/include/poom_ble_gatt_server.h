// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdint.h>

#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_ble_gatt_server.h
 * @brief BLE GATT server helper API.
 */

#define POOM_BLE_GATT_SERVER_SERVICE_UUID_DEFAULT            (0x00FFU)
#define POOM_BLE_GATT_SERVER_CHAR_UUID_DEFAULT               (0xFF01U)
#define POOM_BLE_GATT_SERVER_DESCR_UUID_DEFAULT              (0x3333U)
#define POOM_BLE_GATT_SERVER_CHAR_VAL_MAX_LEN_DEFAULT        (0x80U)
#define POOM_BLE_GATT_SERVER_PREPARE_BUF_MAX_SIZE_DEFAULT    (1024U)
#define POOM_BLE_GATT_SERVER_MANUFACTURER_DATA_LEN_DEFAULT   (17U)

/**
 * @brief Basic server identity properties.
 */
typedef struct
{
    const char *device_name;
    const uint8_t *manufacturer_data;
} poom_ble_gatt_server_props_t;

/**
 * @brief Complete server advertising and characteristic configuration.
 */
typedef struct
{
    esp_ble_adv_data_t adv_data;
    esp_ble_adv_data_t scan_rsp_data;
    esp_ble_adv_params_t adv_params;
    esp_attr_value_t char_val;
    poom_ble_gatt_server_props_t bt_props;
} poom_ble_gatt_server_adv_params_t;

/**
 * @brief Optional callbacks for forwarding GATTS/GAP events to user code.
 */
typedef struct
{
    void (*handler_server_cb)(esp_gatts_cb_event_t event_type,
                             esp_ble_gatts_cb_param_t *param);
    void (*handler_gap_cb)(esp_gap_ble_cb_event_t event_type,
                           esp_ble_gap_cb_param_t *param);
} poom_ble_gatt_server_event_cb_t;

/**
 * @brief Returns default characteristic value descriptor.
 */
esp_attr_value_t poom_ble_gatt_server_default_char_val(void);

/**
 * @brief Returns default advertising payload configuration.
 */
esp_ble_adv_data_t poom_ble_gatt_server_default_adv_data(void);

/**
 * @brief Returns default scan response payload configuration.
 */
esp_ble_adv_data_t poom_ble_gatt_server_default_scan_rsp_data(void);

/**
 * @brief Returns default advertising parameters.
 */
esp_ble_adv_params_t poom_ble_gatt_server_default_adv_params(void);

/**
 * @brief Sets advertising and characteristic parameters.
 *
 * @param adv_params Parameter bundle. If NULL, defaults are restored.
 */
void poom_ble_gatt_server_set_adv_data_params(const poom_ble_gatt_server_adv_params_t *adv_params);

/**
 * @brief Sets optional event forwarding callbacks.
 *
 * @param event_cb Event callback table.
 */
void poom_ble_gatt_server_set_callbacks(poom_ble_gatt_server_event_cb_t event_cb);

/**
 * @brief Sends data to connected peer via indication.
 *
 * @param data Data buffer.
 * @param length Data length.
 */
void poom_ble_gatt_server_send_data(uint8_t *data, int length);

/**
 * @brief Starts BLE GATT server helper stack.
 */
void poom_ble_gatt_server_start(void);

/**
 * @brief Stops BLE GATT server helper stack and releases resources.
 */
void poom_ble_gatt_server_stop(void);

#ifdef __cplusplus
}
#endif
