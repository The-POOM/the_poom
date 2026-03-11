// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_SPAM_H
#define POOM_WIFI_SPAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_SPAM_H */
