// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_sd_browser_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * @brief Validates storage context pointer.
 *
 * @param[in] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_validate_(const poom_sd_browser_storage_t* storage)
{
    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief Clears loaded item list from storage context.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_clear_items_(poom_sd_browser_storage_t* storage)
{
    esp_err_t err = poom_sd_browser_storage_validate_(storage);

    if(err != ESP_OK)
    {
        return err;
    }

    free(storage->items);
    storage->items = NULL;
    storage->items_count = 0U;
    storage->selected_index = 0U;

    return ESP_OK;
}

/**
 * @brief Checks if a directory entry name must be skipped.
 *
 * @param[in] name Entry name.
 * @return bool
 */
static bool poom_sd_browser_storage_skip_entry_(const char* name)
{
    if(name == NULL)
    {
        return true;
    }

    if((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0))
    {
        return true;
    }

    return false;
}

/**
 * @brief Builds full child path from parent path and entry name.
 *
 * @param[in] parent_path Parent absolute path.
 * @param[in] child_name Child entry name.
 * @param[out] out_path Output path buffer.
 * @param[in] out_path_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_build_child_path_(
    const char* parent_path,
    const char* child_name,
    char* out_path,
    size_t out_path_len)
{
    int written;

    if((parent_path == NULL) || (child_name == NULL) || (out_path == NULL) || (out_path_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_path, out_path_len, "%s/%s", parent_path, child_name);
    if((written < 0) || ((size_t)written >= out_path_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/**
 * @brief Resolves directory flag and file size for an entry path.
 *
 * @param[in] path Entry absolute path.
 * @param[in] d_type dirent type field.
 * @param[out] out_is_dir True when entry is directory.
 * @param[out] out_size File size for regular files.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_resolve_entry_info_(
    const char* path,
    unsigned char d_type,
    bool* out_is_dir,
    size_t* out_size)
{
    struct stat st;
    int stat_ret;

    if((path == NULL) || (out_is_dir == NULL) || (out_size == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = 0U;

    if(d_type == DT_DIR)
    {
        *out_is_dir = true;
        return ESP_OK;
    }

    if(d_type == DT_REG)
    {
        *out_is_dir = false;
    }
    else
    {
        stat_ret = stat(path, &st);
        if(stat_ret != 0)
        {
            return ESP_FAIL;
        }

        *out_is_dir = S_ISDIR(st.st_mode);
        if(S_ISREG(st.st_mode))
        {
            *out_size = (size_t)st.st_size;
        }

        return ESP_OK;
    }

    stat_ret = stat(path, &st);
    if(stat_ret == 0)
    {
        *out_size = (size_t)st.st_size;
    }

    return ESP_OK;
}

/**
 * @brief Appends one item into storage list.
 *
 * @param[in,out] storage Storage context.
 * @param[in] name Entry name.
 * @param[in] is_directory True when entry is directory.
 * @param[in] file_size_bytes Entry file size.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_append_item_(
    poom_sd_browser_storage_t* storage,
    const char* name,
    bool is_directory,
    size_t file_size_bytes)
{
    poom_sd_browser_storage_item_t* new_items;
    poom_sd_browser_storage_item_t* item;

    if((storage == NULL) || (name == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    new_items = (poom_sd_browser_storage_item_t*)realloc(
        storage->items,
        (storage->items_count + 1U) * sizeof(poom_sd_browser_storage_item_t));
    if(new_items == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    storage->items = new_items;
    item = &storage->items[storage->items_count];

    snprintf(item->name, sizeof(item->name), "%s", name);
    item->is_directory = is_directory;
    item->file_size_bytes = file_size_bytes;

    storage->items_count++;
    return ESP_OK;
}

/**
 * @brief Sort comparator for directory items.
 *
 * @param[in] a First list item.
 * @param[in] b Second list item.
 * @return int
 */
static int poom_sd_browser_storage_item_compare_(const void* a, const void* b)
{
    const poom_sd_browser_storage_item_t* item_a = (const poom_sd_browser_storage_item_t*)a;
    const poom_sd_browser_storage_item_t* item_b = (const poom_sd_browser_storage_item_t*)b;

    if((item_a->is_directory != false) && (item_b->is_directory == false))
    {
        return -1;
    }

    if((item_a->is_directory == false) && (item_b->is_directory != false))
    {
        return 1;
    }

    return strcmp(item_a->name, item_b->name);
}

/**
 * @brief Updates root state based on current path.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_update_root_state_(poom_sd_browser_storage_t* storage)
{
    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    storage->is_root = (strcmp(storage->current_path, POOM_SD_BROWSER_STORAGE_ROOT) == 0);
    return ESP_OK;
}

/**
 * @brief Trims current path to parent directory.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_storage_trim_parent_path_(poom_sd_browser_storage_t* storage)
{
    char* last_sep;

    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(strcmp(storage->current_path, POOM_SD_BROWSER_STORAGE_ROOT) == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    last_sep = strrchr(storage->current_path, '/');
    if(last_sep == NULL)
    {
        snprintf(storage->current_path, sizeof(storage->current_path), "%s", POOM_SD_BROWSER_STORAGE_ROOT);
        return ESP_OK;
    }

    if(last_sep == storage->current_path)
    {
        snprintf(storage->current_path, sizeof(storage->current_path), "%s", POOM_SD_BROWSER_STORAGE_ROOT);
        return ESP_OK;
    }

    *last_sep = '\0';

    if(strlen(storage->current_path) < strlen(POOM_SD_BROWSER_STORAGE_ROOT))
    {
        snprintf(storage->current_path, sizeof(storage->current_path), "%s", POOM_SD_BROWSER_STORAGE_ROOT);
    }

    return ESP_OK;
}

/**
 * @brief Initializes SD browser storage context.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_init(poom_sd_browser_storage_t* storage)
{
    esp_err_t err = poom_sd_browser_storage_validate_(storage);

    if(err != ESP_OK)
    {
        return err;
    }

    memset(storage, 0, sizeof(*storage));
    snprintf(storage->current_path, sizeof(storage->current_path), "%s", POOM_SD_BROWSER_STORAGE_ROOT);

    return poom_sd_browser_storage_update_root_state_(storage);
}

/**
 * @brief Releases all allocated storage resources.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_deinit(poom_sd_browser_storage_t* storage)
{
    return poom_sd_browser_storage_clear_items_(storage);
}

/**
 * @brief Reloads directory items for current path.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_reload(poom_sd_browser_storage_t* storage)
{
    DIR* dir;
    struct dirent* entry;
    esp_err_t err;

    err = poom_sd_browser_storage_validate_(storage);
    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_sd_browser_storage_clear_items_(storage);
    if(err != ESP_OK)
    {
        return err;
    }

    dir = opendir(storage->current_path);
    if(dir == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    while((entry = readdir(dir)) != NULL)
    {
        char full_path[POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN];
        bool is_dir = false;
        size_t file_size = 0U;

        if(poom_sd_browser_storage_skip_entry_(entry->d_name))
        {
            continue;
        }

        err = poom_sd_browser_storage_build_child_path_(
            storage->current_path,
            entry->d_name,
            full_path,
            sizeof(full_path));
        if(err != ESP_OK)
        {
            continue;
        }

        err = poom_sd_browser_storage_resolve_entry_info_(
            full_path,
            entry->d_type,
            &is_dir,
            &file_size);
        if(err != ESP_OK)
        {
            continue;
        }

        err = poom_sd_browser_storage_append_item_(storage, entry->d_name, is_dir, file_size);
        if(err != ESP_OK)
        {
            closedir(dir);
            return err;
        }
    }

    closedir(dir);

    if(storage->items_count > 1U)
    {
        qsort(
            storage->items,
            storage->items_count,
            sizeof(poom_sd_browser_storage_item_t),
            poom_sd_browser_storage_item_compare_);
    }

    if(storage->selected_index >= storage->items_count)
    {
        storage->selected_index = (storage->items_count == 0U) ? 0U : (storage->items_count - 1U);
    }

    return poom_sd_browser_storage_update_root_state_(storage);
}

/**
 * @brief Moves selection to next item.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_select_next(poom_sd_browser_storage_t* storage)
{
    esp_err_t err = poom_sd_browser_storage_validate_(storage);

    if(err != ESP_OK)
    {
        return err;
    }

    if(storage->items_count == 0U)
    {
        return ESP_OK;
    }

    if(storage->selected_index + 1U >= storage->items_count)
    {
        storage->selected_index = 0U;
    }
    else
    {
        storage->selected_index++;
    }

    return ESP_OK;
}

/**
 * @brief Moves selection to previous item.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_select_prev(poom_sd_browser_storage_t* storage)
{
    esp_err_t err = poom_sd_browser_storage_validate_(storage);

    if(err != ESP_OK)
    {
        return err;
    }

    if(storage->items_count == 0U)
    {
        return ESP_OK;
    }

    if(storage->selected_index == 0U)
    {
        storage->selected_index = storage->items_count - 1U;
    }
    else
    {
        storage->selected_index--;
    }

    return ESP_OK;
}

/**
 * @brief Enters selected directory or reports selected file.
 *
 * @param[in,out] storage Storage context.
 * @param[out] out_is_file True when selected item is file.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_enter_selected(poom_sd_browser_storage_t* storage, bool* out_is_file)
{
    char child_path[POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN];
    const poom_sd_browser_storage_item_t* item;
    esp_err_t err;

    if((storage == NULL) || (out_is_file == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(storage->items_count == 0U)
    {
        return ESP_ERR_NOT_FOUND;
    }

    item = &storage->items[storage->selected_index];
    if(item->is_directory == false)
    {
        *out_is_file = true;
        return ESP_OK;
    }

    err = poom_sd_browser_storage_build_child_path_(
        storage->current_path,
        item->name,
        child_path,
        sizeof(child_path));
    if(err != ESP_OK)
    {
        return err;
    }

    snprintf(storage->current_path, sizeof(storage->current_path), "%s", child_path);
    storage->selected_index = 0U;
    *out_is_file = false;

    return poom_sd_browser_storage_reload(storage);
}

/**
 * @brief Moves to parent directory when current path is not root.
 *
 * @param[in,out] storage Storage context.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_storage_go_parent(poom_sd_browser_storage_t* storage)
{
    esp_err_t err;

    err = poom_sd_browser_storage_validate_(storage);
    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_sd_browser_storage_trim_parent_path_(storage);
    if(err != ESP_OK)
    {
        return err;
    }

    storage->selected_index = 0U;
    return poom_sd_browser_storage_reload(storage);
}

/**
 * @brief Returns selected item pointer or NULL when list is empty.
 *
 * @param[in] storage Storage context.
 * @return const poom_sd_browser_storage_item_t*
 */
const poom_sd_browser_storage_item_t* poom_sd_browser_storage_get_selected(const poom_sd_browser_storage_t* storage)
{
    if((storage == NULL) || (storage->items_count == 0U) || (storage->items == NULL))
    {
        return NULL;
    }

    if(storage->selected_index >= storage->items_count)
    {
        return NULL;
    }

    return &storage->items[storage->selected_index];
}
