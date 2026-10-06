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
 * @brief Advertising payload platform family.
 */
typedef enum
{
    POOM_BLE_SPAM_PLATFORM_APPLE = 0,   /**< Apple proximity-pairing popups.      */
    POOM_BLE_SPAM_PLATFORM_GOOGLE,      /**< Google Fast Pair (Android).          */
    POOM_BLE_SPAM_PLATFORM_MICROSOFT,   /**< Microsoft Swift Pair (Windows).      */
    POOM_BLE_SPAM_PLATFORM_ALL,         /**< Rotate across every platform.        */
    POOM_BLE_SPAM_PLATFORM_COUNT
} poom_ble_spam_platform_t;

/**
 * @brief Callback used to expose current advertised name.
 *
 * @param name Null-terminated device name.
 */
typedef void (*poom_ble_spam_cb_display)(const char *name);

/**
 * @brief Selects which platform's payloads are advertised.
 *
 * @param platform Platform selection. Values outside the enum are ignored.
 */
void poom_ble_spam_set_platform(poom_ble_spam_platform_t platform);

/**
 * @brief Returns the currently selected platform.
 *
 * @return poom_ble_spam_platform_t Current platform selection.
 */
poom_ble_spam_platform_t poom_ble_spam_get_platform(void);

/**
 * @brief Returns a short display label for a platform.
 *
 * @param platform Platform selection.
 * @return const char * Upper-case label, or "?" for an unknown value.
 */
const char *poom_ble_spam_platform_name(poom_ble_spam_platform_t platform);

/**
 * @brief Steps the platform selection forwards or backwards.
 *
 * @param direction +1 for the next platform, -1 for the previous one.
 * @return poom_ble_spam_platform_t The newly selected platform.
 */
poom_ble_spam_platform_t poom_ble_spam_cycle_platform(int direction);

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
