// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_boot_policy.h
 * @brief API for installing external games into `ota_1` and controlling the next boot target.
 */

#ifndef POOM_BOOT_POLICY_H
#define POOM_BOOT_POLICY_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    /**< Selects the base project image in `ota_0`. */
    POOM_BOOT_TARGET_POOM = 0,
    /**< Selects the game installed in `ota_1`. */
    POOM_BOOT_TARGET_GAME = 1,
} poom_boot_target_t;

/**
 * @brief Initializes the module state and synchronizes whether a game exists in `ota_1`.
 *
 * @return `ESP_OK` if initialization completed successfully.
 * @return Another `esp_err_t` if the persistent state storage could not be prepared.
 */
esp_err_t poom_boot_policy_init(void);

/**
 * @brief Applies the boot policy when the base app starts.
 *
 * @details This currently forces the next normal boot to point to `ota_0`.
 *
 * @return `ESP_OK` if the policy was applied successfully.
 * @return Another `esp_err_t` if the boot target could not be queried or updated.
 */
esp_err_t poom_boot_policy_apply_startup_policy(void);

/**
 * @brief Returns the boot target for compatibility.
 *
 * @details This function no longer represents a global persistent preference; it only preserves
 *          the previous API shape and reflects that the base project is the default target.
 *
 * @return `POOM_BOOT_TARGET_POOM` as the base target.
 */
poom_boot_target_t poom_boot_policy_get_preference(void);

/**
 * @brief Sets the next boot target for compatibility.
 *
 * @param[in] target Desired target for the next boot.
 *               Use `POOM_BOOT_TARGET_POOM` for the base app or `POOM_BOOT_TARGET_GAME` for the game.
 *
 * @return `ESP_OK` if the target was updated successfully.
 * @return `ESP_ERR_INVALID_ARG` if `target` is invalid.
 * @return Another `esp_err_t` if the selected boot target could not be prepared.
 */
esp_err_t poom_boot_policy_set_preference(poom_boot_target_t target);

/**
 * @brief Reports whether a valid game app is currently installed in `ota_1`.
 *
 * @return `true` if a valid game image was detected.
 * @return `false` if no game is installed or the state could not be confirmed.
 */
bool poom_boot_policy_game_present(void);

/**
 * @brief Stores the logical game-presence state.
 *
 * @param[in] present `true` to mark that a game is installed; `false` to mark that no game is installed.
 *
 * @return `ESP_OK` if the state was stored successfully.
 * @return Another `esp_err_t` if persistent storage failed.
 */
esp_err_t poom_boot_policy_set_game_present(bool present);

/**
 * @brief Ensures that `/sdcard/apps` exists for browsing external bins.
 *
 * @return `ESP_OK` if the SD card is mounted and the directory exists.
 * @return Another `esp_err_t` if the SD card could not be mounted or the directory could not be created/verified.
 */
esp_err_t poom_boot_policy_prepare_apps_dir(void);

/**
 * @brief Installs an external binary into the `ota_1` partition.
 *
 * @param[in] path Absolute or relative path to the `.bin` file to install.
 *             It must point to a regular file accessible from the SD card.
 *
 * @return `ESP_OK` if the binary was written successfully into `ota_1`.
 * @return `ESP_ERR_INVALID_ARG` if `path` is null, empty, or does not end with `.bin`.
 * @return `ESP_ERR_NOT_FOUND` if the file or the `ota_1` partition does not exist.
 * @return `ESP_ERR_INVALID_SIZE` if the file is empty or does not fit in `ota_1`.
 * @return `ESP_ERR_NO_MEM` if the temporary copy buffer could not be allocated.
 * @return Another `esp_err_t` if the OTA process fails.
 */
esp_err_t poom_boot_policy_install(const char* path);

/**
 * @brief Installs an external binary into `ota_1` and reboots into the game.
 *
 * @param[in] path Path to the `.bin` file to install and run.
 *
 * @return `ESP_OK` if the system reaches the requested reboot.
 * @return Another `esp_err_t` if installation or game boot preparation fails.
 */
esp_err_t poom_boot_policy_install_and_boot(const char* path);

/**
 * @brief Configures the next boot to return to the base app in `ota_0`.
 *
 * @return `ESP_OK` if `ota_0` was selected for the next boot.
 * @return Another `esp_err_t` if the partition was not found or the OTA metadata update failed.
 */
esp_err_t poom_boot_policy_boot_poom(void);

/**
 * @brief Configures the next boot to run the game installed in `ota_1`.
 *
 * @return `ESP_OK` if `ota_1` was selected for the next boot.
 * @return `ESP_ERR_NOT_FOUND` if no valid game exists in `ota_1`.
 * @return Another `esp_err_t` if the game partition could not be prepared.
 */
esp_err_t poom_boot_policy_boot_game(void);

/**
 * @brief Forces an immediate return to the base app and reboots the device.
 *
 * @return `ESP_OK` if the system reaches the requested reboot.
 * @return Another `esp_err_t` if `ota_0` could not be selected before rebooting.
 */
esp_err_t poom_return_to_base(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BOOT_POLICY_H */
