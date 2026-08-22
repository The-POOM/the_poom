// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_SPAM_H
#define POOM_WIFI_SPAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define POOM_WIFI_SPAM_SSID_MAX_LEN (32U)
#define POOM_WIFI_SPAM_SSIDS_MAX (32U)

typedef struct
{
    uint8_t count;
    char ssids[POOM_WIFI_SPAM_SSIDS_MAX][POOM_WIFI_SPAM_SSID_MAX_LEN + 1U];
} poom_wifi_spam_ssid_list_t;

/**
 * @brief Starts the Wi-Fi spam module.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_start(void);

/**
 * @brief Stops the Wi-Fi spam module.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_stop(void);

/**
 * @brief Gets the running state of the Wi-Fi spam module.
 * @param[in/out] out_running Output running flag pointer.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_get_running(bool *out_running);

/**
 * @brief Get current SSID list (copy).
 *
 * If no custom list exists, returns the built-in defaults.
 *
 * @param[out] out_list SSID list snapshot.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_ssids_get(poom_wifi_spam_ssid_list_t* out_list);

/**
 * @brief Replace SSID list and persist it.
 *
 * @param[in] list New SSID list.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_ssids_set(const poom_wifi_spam_ssid_list_t* list);

/**
 * @brief Remove one SSID by index and persist.
 *
 * @param[in] index 0-based index.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_ssids_remove(uint8_t index);

/**
 * @brief Append one SSID and persist.
 *
 * @param[in] ssid SSID string (max 32 bytes).
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_ssids_add(const char* ssid);

/**
 * @brief Reset SSID list to built-in defaults and clear persisted config.
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_ssids_reset_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_SPAM_H */
