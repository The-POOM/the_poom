// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_SD_BROWSER_H
#define POOM_SD_BROWSER_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Enable or disable poom_sd_browser logs. */
#ifndef POOM_SD_BROWSER_ENABLE_LOG
#define POOM_SD_BROWSER_ENABLE_LOG (1)
#endif

/** @brief Enable or disable poom_sd_browser debug logs. */
#ifndef POOM_SD_BROWSER_DEBUG_LOG_ENABLED
#define POOM_SD_BROWSER_DEBUG_LOG_ENABLED (0)
#endif

/**
 * @brief Exit callback type triggered when user exits at SD root.
 */
typedef void (*poom_sd_browser_exit_cb_t)(void* user_ctx);

/**
 * @brief Starts SD browser and subscribes button handling.
 *
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_start(void);

/**
 * @brief Stops SD browser and unsubscribes button handling.
 *
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_stop(void);

/**
 * @brief Checks whether SD browser is running.
 *
 * @return esp_err_t
 */
bool poom_sd_browser_is_running(void);

/**
 * @brief Registers exit callback executed when user exits at SD root.
 *
 * @param[in] callback Exit callback. NULL disables callback.
 * @param[in] user_ctx User context pointer passed to callback.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_set_exit_callback(poom_sd_browser_exit_cb_t callback, void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* POOM_SD_BROWSER_H */
