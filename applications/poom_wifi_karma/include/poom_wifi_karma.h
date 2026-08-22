// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_KARMA_H
#define POOM_WIFI_KARMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum
{
    POOM_WIFI_KARMA_STATE_STOPPED = 0,
    POOM_WIFI_KARMA_STATE_SCANNING,
    POOM_WIFI_KARMA_STATE_CLONING,
    POOM_WIFI_KARMA_STATE_CAPTIVE
} poom_wifi_karma_state_t;

/**
 * @brief Starts POOM Wi-Fi Karma runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_karma_start(void);

/**
 * @brief Stops POOM Wi-Fi Karma runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_karma_stop(void);

/**
 * @brief Copies discovered SSIDs into caller-provided output array.
 * @param[out] destination_array Destination array of SSID strings.
 * @param[in] max_count Maximum number of entries to copy.
 * @return int
 */
int poom_wifi_karma_get_discovered_ssids(char destination_array[][33], int max_count);

/**
 * @brief Copies currently cloned/active SSID into caller buffer.
 * @param[out] out_ssid Destination buffer.
 * @param[in] out_len Destination buffer length.
 * @return bool
 */
bool poom_wifi_karma_get_active_ssid(char *out_ssid, size_t out_len);

/**
 * @brief Returns current Karma runtime state for UI/status use.
 * @return poom_wifi_karma_state_t
 */
poom_wifi_karma_state_t poom_wifi_karma_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_KARMA_H */
