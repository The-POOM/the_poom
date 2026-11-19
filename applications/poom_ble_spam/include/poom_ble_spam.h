// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_SPAM_H
#define POOM_BLE_SPAM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "poom_ble_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    poom_ble_spam_mode_t mode;
    uint32_t duration_ms; /* Zero selects 30s; accepted range 1..60s. */
    uint32_t dwell_ms; /* Zero selects mode default; accepted range100..5000ms. */
} poom_ble_spam_config_t;
typedef struct {
    bool active;
    bool advertising;
    bool radio_released;
    poom_ble_spam_mode_t mode;
    uint32_t cycles; /* Successfully started advertising intervals, not packets heard. */
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    esp_err_t error;
    char label[24];
} poom_ble_spam_status_t;

esp_err_t poom_ble_spam_start_mode(const poom_ble_spam_config_t *config);
esp_err_t poom_ble_spam_stop(void);
bool poom_ble_spam_is_active(void);
void poom_ble_spam_get_status(poom_ble_spam_status_t *status);

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
esp_err_t poom_ble_spam_start(void);

/**
 * @brief Registers a callback with the currently rotated device label.
 *
 * @param callback Callback function. Pass NULL to clear.
 * A callback already copied by the worker can finish after clearing. Stop the
 * worker before releasing any storage used by that callback.
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
