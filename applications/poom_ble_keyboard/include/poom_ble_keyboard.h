// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_KEYBOARD_H
#define POOM_BLE_KEYBOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type invoked on BLE HID connection state changes.
 *
 * @param[in] connected true when BLE HID is connected, false otherwise.
 */
typedef void (*poom_ble_keyboard_connection_cb_t)(bool connected);

/**
 * @brief Starts the BLE keyboard module.
 *
 * @return void
 */
void poom_ble_keyboard_start(void);

/**
 * @brief Stops the BLE keyboard module.
 *
 * @return void
 */
void poom_ble_keyboard_stop(void);

/**
 * @brief Sets keyboard mode.
 *
 * @param[in] enabled true to send keyboard keys, false to send media keys.
 * @return void
 */
void poom_ble_keyboard_set_keyboard_mode(bool enabled);

/**
 * @brief Registers a BLE connection callback.
 *
 * @param[in] cb Callback function. Must not be NULL.
 * @return void
 */
void poom_ble_keyboard_set_connection_callback(poom_ble_keyboard_connection_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_KEYBOARD_H */
