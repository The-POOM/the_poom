// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_boot_policy.c
 * @brief Boot policy implementation for keeping the base app in `ota_0`
 *        and loading external games into `ota_1`.
 */

#include "poom_boot_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "button_driver.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_secrets_store.h"
#include "sd_card.h"

#define POOM_BOOT_POLICY_TAG "poom_boot_policy"

#define POOM_BOOT_POLICY_KEY_GAME    "game_pres"
#define POOM_BOOT_POLICY_APPS_DIR    "/sdcard/apps"
#define POOM_BOOT_POLICY_BUFFER_SIZE (4096U)

#define POOM_BOOT_POLICY_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_BOOT_POLICY_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_BOOT_POLICY_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_BOOT_POLICY_TAG, __func__, __LINE__, ##__VA_ARGS__)

/** @brief Indicates whether the module initialization path has already completed. */
static bool s_initialized = false;
/** @brief Cached flag telling whether a valid game image exists in `ota_1`. */
static bool s_game_present = false;
/** @brief Indicates whether `s_game_present` already reflects a validated state. */
static bool s_game_present_known = false;

static const esp_partition_t* poom_boot_policy_find_partition_(esp_partition_subtype_t subtype);
static esp_err_t poom_boot_policy_set_boot_partition_(esp_partition_subtype_t subtype);
static bool poom_boot_policy_partition_has_valid_app_(const esp_partition_t* partition);
static esp_err_t poom_boot_policy_apply_effective_boot_(void);
static esp_err_t poom_boot_policy_refresh_game_present_(void);
static bool poom_boot_policy_rescue_button_pressed_(void);
static bool poom_boot_policy_path_has_bin_ext_(const char* path);

/**
 * @brief Finds an application partition by OTA subtype.
 *
 * @param[in] subtype OTA partition subtype to find, for example `ESP_PARTITION_SUBTYPE_APP_OTA_0`.
 *
 * @return Pointer to the matching partition.
 * @return `NULL` if no partition exists for that subtype.
 */
static const esp_partition_t* poom_boot_policy_find_partition_(esp_partition_subtype_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
}

/**
 * @brief Selects an OTA partition as the next boot target.
 *
 * @param[in] subtype Subtype of the partition that should boot on the next restart.
 *
 * @return `ESP_OK` if the partition was configured successfully.
 * @return `ESP_ERR_NOT_FOUND` if the partition does not exist.
 * @return Another `esp_err_t` if `esp_ota_set_boot_partition()` fails.
 */
static esp_err_t poom_boot_policy_set_boot_partition_(esp_partition_subtype_t subtype)
{
    const esp_partition_t* part = poom_boot_policy_find_partition_(subtype);

    if(part == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    return esp_ota_set_boot_partition(part);
}

/**
 * @brief Checks whether a partition contains a valid app by reading its OTA description.
 *
 * @param[in] partition Partition to validate.
 *
 * @return `true` if the partition contains a valid app image.
 * @return `false` if the partition is null or does not contain a recognizable image.
 */
static bool poom_boot_policy_partition_has_valid_app_(const esp_partition_t* partition)
{
    esp_app_desc_t app_desc;

    if(partition == NULL)
    {
        return false;
    }

    return (esp_ota_get_partition_description(partition, &app_desc) == ESP_OK);
}

/**
 * @brief Applies the effective boot target used by the current policy.
 *
 * @return `ESP_OK` if the next boot was set to `ota_0`.
 * @return Another `esp_err_t` if the base partition could not be selected.
 */
static esp_err_t poom_boot_policy_apply_effective_boot_(void)
{
    return poom_boot_policy_set_boot_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_0);
}

/**
 * @brief Synchronizes the logical game-present flag with the real contents of `ota_1`.
 *
 * @return `ESP_OK` if the state was synchronized successfully.
 * @return Another `esp_err_t` if persistent state writing fails.
 */
static esp_err_t poom_boot_policy_refresh_game_present_(void)
{
    uint32_t stored_value = 0U;
    const esp_partition_t* ota_1 = poom_boot_policy_find_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    const bool actual = poom_boot_policy_partition_has_valid_app_(ota_1);
    const esp_err_t read_err = poom_secrets_get_u32(POOM_BOOT_POLICY_KEY_GAME, &stored_value);

    s_game_present = actual;
    s_game_present_known = true;

    if((read_err != ESP_OK) || ((stored_value != 0U) != actual))
    {
        return poom_boot_policy_set_game_present(actual);
    }

    return ESP_OK;
}

/**
 * @brief Detects whether the rescue button is pressed during startup.
 *
 * @return `true` if the button was pressed when sampled.
 * @return `false` if it was not pressed or if GPIO setup/read failed.
 */
static bool poom_boot_policy_rescue_button_pressed_(void)
{
    const gpio_num_t pin = (gpio_num_t)B_BUTTON_PIN;
    const uint32_t level_pressed = (uint32_t)BUTTON_ACTIVE_LEVEL;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if(gpio_config(&cfg) != ESP_OK)
    {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10U));
    return ((uint32_t)gpio_get_level(pin) == level_pressed);
}

/**
 * @brief Checks whether a path ends with the `.bin` extension.
 *
 * @param[in] path File path to validate.
 *
 * @return `true` if the path is valid and ends with `.bin`.
 * @return `false` if the path is null, empty, or has a different extension.
 */
static bool poom_boot_policy_path_has_bin_ext_(const char* path)
{
    const char* dot;

    if((path == NULL) || (path[0] == '\0'))
    {
        return false;
    }

    dot = strrchr(path, '.');
    if(dot == NULL)
    {
        return false;
    }

    return (strcasecmp(dot, ".bin") == 0);
}

/**
 * @copydoc poom_boot_policy_init
 */
esp_err_t poom_boot_policy_init(void)
{
    esp_err_t err;

    if(s_initialized)
    {
        return ESP_OK;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_boot_policy_refresh_game_present_();
    if(err != ESP_OK)
    {
        POOM_BOOT_POLICY_PRINTF_W("game presence sync failed: %s", esp_err_to_name(err));
    }

    s_initialized = true;
    return ESP_OK;
}

/**
 * @copydoc poom_boot_policy_apply_startup_policy
 */
esp_err_t poom_boot_policy_apply_startup_policy(void)
{
    esp_err_t err = poom_boot_policy_init();

    if(err != ESP_OK)
    {
        return err;
    }

    if(poom_boot_policy_rescue_button_pressed_())
    {
        POOM_BOOT_POLICY_PRINTF_I("Rescue button held at boot; forcing POOM");
        return poom_boot_policy_set_boot_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    }

    err = poom_boot_policy_refresh_game_present_();
    if(err != ESP_OK)
    {
        POOM_BOOT_POLICY_PRINTF_W("refresh game state failed: %s", esp_err_to_name(err));
    }

    return poom_boot_policy_apply_effective_boot_();
}

/**
 * @copydoc poom_boot_policy_get_preference
 */
poom_boot_target_t poom_boot_policy_get_preference(void)
{
    return POOM_BOOT_TARGET_POOM;
}

/**
 * @copydoc poom_boot_policy_set_preference
 */
esp_err_t poom_boot_policy_set_preference(poom_boot_target_t target)
{
    if((target != POOM_BOOT_TARGET_POOM) && (target != POOM_BOOT_TARGET_GAME))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(target == POOM_BOOT_TARGET_POOM)
    {
        return poom_boot_policy_boot_poom();
    }

    return poom_boot_policy_boot_game();
}

/**
 * @copydoc poom_boot_policy_game_present
 */
bool poom_boot_policy_game_present(void)
{
    if(!s_initialized)
    {
        (void)poom_boot_policy_init();
    }

    if(!s_game_present_known)
    {
        (void)poom_boot_policy_refresh_game_present_();
    }

    return s_game_present;
}

/**
 * @copydoc poom_boot_policy_set_game_present
 */
esp_err_t poom_boot_policy_set_game_present(bool present)
{
    esp_err_t err = poom_secrets_set_u32(POOM_BOOT_POLICY_KEY_GAME, present ? 1U : 0U);

    if(err == ESP_OK)
    {
        s_game_present = present;
        s_game_present_known = true;
    }

    return err;
}

/**
 * @copydoc poom_boot_policy_prepare_apps_dir
 */
esp_err_t poom_boot_policy_prepare_apps_dir(void)
{
    struct stat st;
    esp_err_t err = ESP_OK;

    if(sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    if(stat(POOM_BOOT_POLICY_APPS_DIR, &st) != 0)
    {
        if(mkdir(POOM_BOOT_POLICY_APPS_DIR, 0775) != 0)
        {
            return ESP_FAIL;
        }

        if(stat(POOM_BOOT_POLICY_APPS_DIR, &st) != 0)
        {
            return ESP_FAIL;
        }
    }

    if(!S_ISDIR(st.st_mode))
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/**
 * @copydoc poom_boot_policy_install
 */
esp_err_t poom_boot_policy_install(const char* path)
{
    FILE* file = NULL;
    struct stat st;
    const esp_partition_t* ota_1 = NULL;
    esp_ota_handle_t ota_handle = 0;
    uint8_t* buffer = NULL;
    size_t file_size = 0U;
    size_t total_written = 0U;
    bool ota_started = false;
    esp_err_t err = ESP_OK;

    if((path == NULL) || (path[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!poom_boot_policy_path_has_bin_ext_(path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_boot_policy_prepare_apps_dir();
    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_boot_policy_init();
    if(err != ESP_OK)
    {
        return err;
    }

    if(stat(path, &st) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if(!S_ISREG(st.st_mode))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(st.st_size <= 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    file_size = (size_t)st.st_size;

    ota_1 = poom_boot_policy_find_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    if(ota_1 == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if((ota_1->type != ESP_PARTITION_TYPE_APP) || (ota_1->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(file_size > ota_1->size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(path, "rb");
    if(file == NULL)
    {
        return ESP_FAIL;
    }

    buffer = (uint8_t*)malloc(POOM_BOOT_POLICY_BUFFER_SIZE);
    if(buffer == NULL)
    {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = esp_ota_begin(ota_1, file_size, &ota_handle);
    if(err != ESP_OK)
    {
        goto cleanup;
    }
    ota_started = true;

    while(total_written < file_size)
    {
        const size_t chunk_size = ((file_size - total_written) > POOM_BOOT_POLICY_BUFFER_SIZE) ?
            POOM_BOOT_POLICY_BUFFER_SIZE :
            (file_size - total_written);
        const size_t bytes_read = fread(buffer, 1U, chunk_size, file);

        if(bytes_read == 0U)
        {
            err = ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }

        err = esp_ota_write(ota_handle, buffer, bytes_read);
        if(err != ESP_OK)
        {
            goto cleanup;
        }

        total_written += bytes_read;
    }

    err = esp_ota_end(ota_handle);
    ota_started = false;
    if(err != ESP_OK)
    {
        goto cleanup;
    }

    err = poom_boot_policy_set_game_present(true);
    if(err != ESP_OK)
    {
        goto cleanup;
    }

    err = poom_boot_policy_refresh_game_present_();

cleanup:
    if(ota_started)
    {
        (void)esp_ota_abort(ota_handle);
    }

    if(buffer != NULL)
    {
        free(buffer);
    }

    if(file != NULL)
    {
        (void)fclose(file);
    }

    return err;
}

/**
 * @copydoc poom_boot_policy_install_and_boot
 */
esp_err_t poom_boot_policy_install_and_boot(const char* path)
{
    esp_err_t err = poom_boot_policy_install(path);

    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_boot_policy_boot_game();
    if(err != ESP_OK)
    {
        return err;
    }

    esp_restart();
    return ESP_OK;
}

/**
 * @copydoc poom_boot_policy_boot_poom
 */
esp_err_t poom_boot_policy_boot_poom(void)
{
    return poom_boot_policy_set_boot_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_0);
}

/**
 * @copydoc poom_boot_policy_boot_game
 */
esp_err_t poom_boot_policy_boot_game(void)
{
    esp_err_t err = poom_boot_policy_init();

    if(err != ESP_OK)
    {
        return err;
    }

    err = poom_boot_policy_refresh_game_present_();
    if(err != ESP_OK)
    {
        return err;
    }

    if(!s_game_present)
    {
        return ESP_ERR_NOT_FOUND;
    }

    return poom_boot_policy_set_boot_partition_(ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

/**
 * @copydoc poom_return_to_base
 */
esp_err_t poom_return_to_base(void)
{
    esp_err_t err = poom_boot_policy_boot_poom();

    if(err != ESP_OK)
    {
        return err;
    }

    esp_restart();
    return ESP_OK;
}
