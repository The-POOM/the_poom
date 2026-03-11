// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_SPAM_H
#define POOM_BLE_SPAM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_ble_spam.h
 * @brief Public API for the POOM BLE spam application.
 */

/* =========================
 * Logging control
 * ========================= */
#ifndef POOM_BLE_SPAM_LOG_ENABLED
#define POOM_BLE_SPAM_LOG_ENABLED            (1)
#endif

#ifndef POOM_BLE_SPAM_DEBUG_LOG_ENABLED
#define POOM_BLE_SPAM_DEBUG_LOG_ENABLED      (0)
#endif

/**
 * @brief Callback used to expose current advertised name.
 *
 * @param name Null-terminated device name.
 */
typedef void (*poom_ble_spam_cb_display)(const char *name);

/**
 * @brief Starts BLE payload rotation.
 */
void poom_ble_spam_start(void);

/**
 * @brief Registers a callback with the currently rotated device label.
 *
 * @param callback Callback function. Pass NULL to clear.
 */
void poom_ble_spam_register_cb(poom_ble_spam_cb_display callback);

/**
 * @brief Stops BLE advertising rotation.
 */
void poom_ble_spam_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_SPAM_H */
