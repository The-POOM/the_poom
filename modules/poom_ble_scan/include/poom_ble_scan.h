// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_SCAN_H
#define POOM_BLE_SCAN_H

#include <stdbool.h>

#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_ble_scan.h
 * @brief Generic BLE advertisement scanner helper.
 */

/**
 * @brief Callback invoked for each BLE advertising report while scanner is active.
 *
 * @note The callback receives the GAP event payload owned by the BLE stack.
 *       The pointer must not be retained after the callback returns.
 *
 * @param[in] scan_result BLE GAP scan result payload.
 */
typedef void (*poom_ble_scan_result_cb_t)(const esp_ble_gap_cb_param_t *scan_result);

/**
 * @brief Registers an optional callback to consume BLE advertising reports.
 *
 * @param[in] callback Callback invoked on each advertising report, or NULL to clear it.
 */
void poom_ble_scan_register_cb(poom_ble_scan_result_cb_t callback);

/**
 * @brief Sets BLE GAP scan filter policy before scanner start.
 *
 * @param[in] filter_type BLE GAP scan filter policy.
 */
void poom_ble_scan_set_filter_type(esp_ble_scan_filter_t filter_type);

/**
 * @brief Sets BLE GAP scan type before scanner start.
 *
 * @param[in] scan_type BLE GAP scan type.
 */
void poom_ble_scan_set_scan_type(esp_ble_scan_type_t scan_type);

/**
 * @brief Enables or disables UART forwarding of BLE advertising reports.
 *
 * @param[in] enabled true to forward each report via `poom_uart_sniffer`, false otherwise.
 */
void poom_ble_scan_set_uart_forward_enabled(bool enabled);

/**
 * @brief Starts generic BLE advertising scan flow.
 */
esp_err_t poom_ble_scan_start(void);

/**
 * @brief Stops generic BLE advertising scan flow.
 */
esp_err_t poom_ble_scan_stop(void);

/**
 * @brief Returns whether BLE scanner runtime is active.
 *
 * @return true Scanner active.
 * @return false Scanner inactive.
 */
bool poom_ble_scan_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_SCAN_H */
