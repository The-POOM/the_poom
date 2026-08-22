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
 * @brief File-selected callback invoked when user selects a file.
 *
 * @param[in] abs_path Absolute file path (under `/sdcard`).
 * @param[in] user_ctx User context pointer passed during registration.
 */
typedef void (*poom_sd_browser_file_selected_cb_t)(const char* abs_path, void* user_ctx);

/**
 * @brief Optional predicate used to filter directory entries.
 *
 * Return true to show the entry.
 */
typedef bool (*poom_sd_browser_filter_cb_t)(const char* name, bool is_directory, void* user_ctx);

typedef struct
{
    /** Optional start directory (absolute under `/sdcard`). Defaults to `/sdcard`. */
    const char* start_dir;
    /** Optional header title. Defaults to "SD BROWSER". */
    const char* header;
    /** Optional filter predicate. */
    poom_sd_browser_filter_cb_t filter;
    void* filter_ctx;
    /** Optional file-selected callback. When set, file details shows `A:Select`. */
    poom_sd_browser_file_selected_cb_t on_file_selected;
    void* on_file_selected_ctx;
} poom_sd_browser_config_t;

/**
 * @brief Starts SD browser and subscribes button handling.
 *
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_start(void);

/**
 * @brief Starts SD browser with custom configuration (start dir, filter, callbacks).
 *
 * Passing NULL uses defaults (equivalent to `poom_sd_browser_start()`).
 */
esp_err_t poom_sd_browser_start_ex(const poom_sd_browser_config_t* config);

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
