// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_ble_gatt_client.h
 * @brief BLE GATT client helper API.
 */

#define POOM_BLE_GATT_CLIENT_REMOTE_SERVICE_UUID_DEFAULT      (0x00FFU)
#define POOM_BLE_GATT_CLIENT_REMOTE_NOTIFY_CHAR_UUID_DEFAULT  (0xFF01U)
#define POOM_BLE_GATT_CLIENT_SCAN_DURATION_SEC_DEFAULT        (60U)
#define POOM_BLE_GATT_CLIENT_REMOTE_NAME_MAX_LEN              (32U)

/**
 * @brief BLE scan and filter parameters used by the GATT client helper.
 */
typedef struct
{
    esp_bt_uuid_t remote_filter_service_uuid;
    esp_bt_uuid_t remote_filter_char_uuid;
    esp_bt_uuid_t notify_descr_uuid;
    esp_ble_scan_params_t ble_scan_params;
} poom_ble_gatt_client_scan_params_t;

/**
 * @brief Optional callbacks for forwarding GAP/GATTC events to user code.
 */
typedef struct
{
    void (*handler_client_cb)(esp_gattc_cb_event_t event_type,
                             esp_ble_gattc_cb_param_t *param);
    void (*handler_gap_cb)(esp_gap_ble_cb_event_t event_type,
                            esp_ble_gap_cb_param_t *param);
} poom_ble_gatt_client_event_cb_t;

/**
 * @brief Returns default remote service UUID filter.
 */
esp_bt_uuid_t poom_ble_gatt_client_default_service_uuid(void);

/**
 * @brief Returns default remote notify characteristic UUID filter.
 */
esp_bt_uuid_t poom_ble_gatt_client_default_char_uuid(void);

/**
 * @brief Returns default CCC descriptor UUID filter.
 */
esp_bt_uuid_t poom_ble_gatt_client_default_notify_descr_uuid(void);

/**
 * @brief Returns default BLE scan parameters.
 */
esp_ble_scan_params_t poom_ble_gatt_client_default_scan_params(void);

/**
 * @brief Sets target remote device name used during scan matching.
 *
 * @param device_name Complete advertising name to match.
 */
void poom_ble_gatt_client_set_remote_device_name(const char *device_name);

/**
 * @brief Sets scan and UUID filter parameters.
 *
 * @param scan_params Scan/filter parameters.
 */
void poom_ble_gatt_client_set_scan_params(const poom_ble_gatt_client_scan_params_t *scan_params);

/**
 * @brief Sets optional event forwarding callbacks.
 *
 * @param event_cb Event callback table.
 */
void poom_ble_gatt_client_set_callbacks(poom_ble_gatt_client_event_cb_t event_cb);

/**
 * @brief Sends data to remote characteristic handle.
 *
 * @param data Data buffer.
 * @param length Data length.
 */
void poom_ble_gatt_client_send_data(uint8_t *data, int length);

/**
 * @brief Starts BLE GATT client helper stack.
 */
void poom_ble_gatt_client_start(void);

/**
 * @brief Stops BLE GATT client helper stack and releases resources.
 */
void poom_ble_gatt_client_stop(void);

#ifdef __cplusplus
}
#endif
