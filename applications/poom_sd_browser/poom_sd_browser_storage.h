// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_SD_BROWSER_STORAGE_H
#define POOM_SD_BROWSER_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_SD_BROWSER_STORAGE_ROOT "/sdcard"
#define POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN (256U)
#define POOM_SD_BROWSER_STORAGE_MAX_NAME_LEN (64U)

typedef struct
{
    char name[POOM_SD_BROWSER_STORAGE_MAX_NAME_LEN + 1U];
    bool is_directory;
    size_t file_size_bytes;
} poom_sd_browser_storage_item_t;

typedef struct
{
    poom_sd_browser_storage_item_t* items;
    size_t items_count;
    size_t selected_index;
    char current_path[POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN];
    bool is_root;
} poom_sd_browser_storage_t;

/**
 * @brief Initializes SD browser storage context.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_init(poom_sd_browser_storage_t* storage);

/**
 * @brief Releases all allocated storage resources.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_deinit(poom_sd_browser_storage_t* storage);

/**
 * @brief Reloads directory items for current path.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_reload(poom_sd_browser_storage_t* storage);

/**
 * @brief Moves selection to next item.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_select_next(poom_sd_browser_storage_t* storage);

/**
 * @brief Moves selection to previous item.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_select_prev(poom_sd_browser_storage_t* storage);

/**
 * @brief Enters selected directory or reports selected file.
 *
 * @param[in,out] storage Storage context.
 * @param[out] out_is_file True when selected item is file.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_enter_selected(poom_sd_browser_storage_t* storage, bool* out_is_file);

/**
 * @brief Moves to parent directory when current path is not root.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_go_parent(poom_sd_browser_storage_t* storage);

/**
 * @brief Returns selected item pointer or NULL when list is empty.
 *
 * @param[in] storage Storage context.
 * @return const poom_sd_browser_storage_item_t*
 */
const poom_sd_browser_storage_item_t* poom_sd_browser_storage_get_selected(const poom_sd_browser_storage_t* storage);

#ifdef __cplusplus
}
#endif

#endif /* POOM_SD_BROWSER_STORAGE_H */
