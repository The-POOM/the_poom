// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file sd_card.h
 * @brief SD card mount and file utility helpers (SPI mode, FATFS).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Returned when trying to create a file that already exists. */
#define ESP_ERR_FILE_EXISTS       ESP_ERR_NOT_ALLOWED
/** @brief Returned when file open operation fails. */
#define ESP_ERR_FILE_OPEN_FAILED  ESP_FAIL
/** @brief Returned when file write operation fails. */
#define ESP_ERR_FILE_WRITE_FAILED ESP_FAIL

/** @brief Default mount root for SD card VFS. */
#define SD_CARD_PATH "/sdcard"

/**
 * @brief SD card metadata snapshot.
 */
typedef struct
{
    const char *name;
    uint64_t total_space;
    float speed;
    const char *type;
} sd_card_info_t;

/**
 * @brief Initialize the SD card module.
 */
void sd_card_begin(void);

/**
 * @brief Mount the SD card filesystem.
 *
 * @return ESP_OK on success.
 * @return ESP_ERR_NO_MEM on SPI bus init failure.
 * @return ESP_ERR_NOT_SUPPORTED if format is required and disabled.
 * @return ESP_ERR_NOT_FOUND when card init/probe fails.
 * @return ESP_FAIL on generic error.
 */
esp_err_t sd_card_mount(void);

/**
 * @brief Unmount SD card and release SPI bus.
 *
 * @return ESP_OK on success.
 * @return ESP_FAIL on failure.
 */
esp_err_t sd_card_unmount(void);

/**
 * @brief Attempt a mount allowing automatic FAT format when needed.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t sd_card_check_format(void);

/**
 * @brief Format mounted SD card using FATFS formatter.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t sd_card_format(void);

/**
 * @brief Check if SD card is currently mounted and writable.
 *
 * @return true when mounted.
 * @return false otherwise.
 */
bool sd_card_is_mounted(void);

/**
 * @brief Inverse of sd_card_is_mounted().
 */
bool sd_card_is_not_mounted(void);

/**
 * @brief Create a directory.
 *
 * @param dir_name Directory path.
 * @return ESP_OK on success or if it already exists.
 * @return ESP_FAIL on error.
 */
esp_err_t sd_card_create_dir(const char *dir_name);

/**
 * @brief Create an empty file.
 *
 * @param path Relative file path from SD_CARD_PATH.
 * @return ESP_OK on success.
 * @return ESP_ERR_FILE_EXISTS if file is already present.
 * @return ESP_FAIL on error.
 */
esp_err_t sd_card_create_file(const char *path);

/**
 * @brief Print file content line by line to log output.
 *
 * @param path Relative file path from SD_CARD_PATH.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sd_card_read_file(const char *path);

/**
 * @brief Write string content to file (truncate/create).
 *
 * @param path Relative file path from SD_CARD_PATH.
 * @param data Null-terminated text content.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sd_card_write_file(const char *path, const char *data);

/**
 * @brief Append string content to file.
 *
 * @param path Relative file path from SD_CARD_PATH.
 * @param data Null-terminated text content.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sd_card_append_to_file(const char *path, const char *data);

/**
 * @brief Read basic card metadata.
 *
 * @return sd_card_info_t Current metadata snapshot.
 */
sd_card_info_t sd_card_get_info(void);

/**
 * @brief Open and close a directory to verify it can be listed.
 *
 * @param path Directory path.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sd_card_list_files(const char *path);

/**
 * @brief Print file names from SD root directory.
 */
void sd_card_printf_files(void);

/**
 * @brief Read file into malloc-allocated buffer (NUL-terminated).
 *
 * Caller must free(*out_data).
 *
 * @param path Relative file path from SD_CARD_PATH.
 * @param out_data Output pointer to allocated buffer.
 * @param out_size Output number of bytes read (without NUL terminator).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t sd_card_read_file_to_buffer(const char *path, uint8_t **out_data, size_t *out_size);
