// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_DEAUTH_H
#define POOM_WIFI_DEAUTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Scans nearby APs and prints them to console.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_scan_and_list(void);

/**
 * @brief Sends one deauth broadcast attack against a selected AP index.
 * @param[in] index AP index from scanner cache.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_attack(int index);

/**
 * @brief Stops the running deauth loop and attack module.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_stop(void);

/**
 * @brief Starts the deauth scan/attack loop task.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_start(void);

/**
 * @brief Reports whether the deauth loop is currently running.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_deauth_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_DEAUTH_H */
