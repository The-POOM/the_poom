/*
 * SPDX-FileCopyrightText: 2026 THE POOM
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file hooks.c
 * @brief Bootloader hook that allows a one-shot boot into `ota_1`
 *        and automatically schedules the return to `ota_0`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "bootloader_common.h"
#include "bootloader_utility.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

#define POOM_BOOT_POLICY_BASE_SLOT     (0)
#define POOM_BOOT_POLICY_GAME_SLOT     (1)
#define POOM_BOOT_POLICY_FLASH_SECTOR_SIZE (0x1000U)

/**
 * @brief Anchor symbol used to force this module to be linked into the bootloader.
 */
void bootloader_hooks_include(void)
{
}

int __real_bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs);
esp_err_t bootloader_flash_erase_sector(size_t sector);
esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size, bool write_encrypted);

static uint32_t poom_boot_policy_compute_ota_seq_(uint32_t current_seq, uint32_t target_slot, uint8_t ota_app_count);
static esp_err_t poom_boot_policy_write_otadata_(const bootloader_state_t *bs, esp_ota_select_entry_t *otadata, uint32_t seq, uint8_t sec_id);
static esp_err_t poom_boot_policy_set_next_boot_slot_(const bootloader_state_t *bs, uint32_t target_slot);

/**
 * @brief Intercepts the boot selection computed by the bootloader.
 *
 * @param[in] bs Bootloader state containing the OTA information detected during startup.
 *
 * @return Partition index that should be booted in the current power cycle.
 *         If the selected target is `ota_1`, this function also prepares `ota_0`
 *         for the next reboot.
 */
int __wrap_bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = __real_bootloader_utility_get_selected_boot_partition(bs);

    if (boot_index == INVALID_INDEX) {
        return boot_index;
    }

    if (boot_index == POOM_BOOT_POLICY_GAME_SLOT) {
        const esp_err_t err = poom_boot_policy_set_next_boot_slot_(bs, POOM_BOOT_POLICY_BASE_SLOT);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to arm return to ota_0");
            return POOM_BOOT_POLICY_BASE_SLOT;
        }

        ESP_LOGI(TAG, "ota_1 selected for one-shot boot, next boot forced to ota_0");
    }

    return boot_index;
}

/**
 * @brief Computes a new OTA sequence value that points to the requested slot.
 *
 * @param[in] current_seq Currently active OTA sequence.
 * @param[in] target_slot OTA slot that should be scheduled for the next boot.
 * @param[in] ota_app_count Total number of OTA application slots.
 *
 * @return Valid OTA sequence value for the requested slot.
 */
static uint32_t poom_boot_policy_compute_ota_seq_(uint32_t current_seq, uint32_t target_slot, uint8_t ota_app_count)
{
    uint32_t seq = target_slot + 1U;

    if (ota_app_count == 0U) {
        return 0U;
    }

    while (seq <= current_seq) {
        seq += ota_app_count;
    }

    return seq;
}

/**
 * @brief Writes one `otadata` entry to flash.
 *
 * @param[in] bs Bootloader state containing the base `otadata` offset.
 * @param[in,out] otadata Array holding the two OTA entries already loaded into RAM.
 * @param[in] seq New OTA sequence value to store.
 * @param[in] sec_id Index of the entry to overwrite, typically `0` or `1`.
 *
 * @return `ESP_OK` if the entry was written successfully.
 * @return `ESP_ERR_INVALID_ARG` if `otadata` is null or `sec_id` is out of range.
 * @return Another `esp_err_t` if flash erase or write fails.
 */
static esp_err_t poom_boot_policy_write_otadata_(const bootloader_state_t *bs,
                                                 esp_ota_select_entry_t *otadata,
                                                 uint32_t seq,
                                                 uint8_t sec_id)
{
    const uint32_t offset = bs->ota_info.offset + (POOM_BOOT_POLICY_FLASH_SECTOR_SIZE * sec_id);
    const bool write_encrypted = esp_flash_encryption_enabled();

    if ((otadata == NULL) || (sec_id > 1U)) {
        return ESP_ERR_INVALID_ARG;
    }

    otadata[sec_id].ota_seq = seq;
    otadata[sec_id].ota_state = ESP_OTA_IMG_VALID;
    otadata[sec_id].crc = bootloader_common_ota_select_crc(&otadata[sec_id]);

    esp_err_t err = bootloader_flash_erase_sector(offset / POOM_BOOT_POLICY_FLASH_SECTOR_SIZE);

    if (err != ESP_OK) {
        return err;
    }

    return bootloader_flash_write(offset, &otadata[sec_id], sizeof(esp_ota_select_entry_t), write_encrypted);
}

/**
 * @brief Schedules the next boot for a specific OTA slot.
 *
 * @param[in] bs Bootloader state with already resolved OTA metadata.
 * @param[in] target_slot OTA slot that should be armed for the next boot.
 *
 * @return `ESP_OK` if `otadata` was updated successfully.
 * @return `ESP_ERR_INVALID_STATE` if the OTA state read by the bootloader is invalid.
 * @return `ESP_ERR_INVALID_ARG` if `target_slot` does not match an existing slot.
 * @return Another `esp_err_t` if reading or writing `otadata` fails.
 */
static esp_err_t poom_boot_policy_set_next_boot_slot_(const bootloader_state_t *bs, uint32_t target_slot)
{
    esp_ota_select_entry_t otadata[2];
    int active_otadata;
    int next_otadata;
    uint32_t new_seq;
    esp_err_t err;

    if ((bs == NULL) || (bs->ota_info.offset == 0U) || (bs->app_count == 0U)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (target_slot >= bs->app_count) {
        return ESP_ERR_INVALID_ARG;
    }

    err = bootloader_common_read_otadata(&bs->ota_info, otadata);
    if (err != ESP_OK) {
        return err;
    }

    active_otadata = bootloader_common_get_active_otadata(otadata);
    if (active_otadata >= 0) {
        const uint32_t current_slot = (otadata[active_otadata].ota_seq - 1U) % bs->app_count;

        if (current_slot == target_slot) {
            next_otadata = active_otadata;
            new_seq = otadata[active_otadata].ota_seq;
        } else {
            next_otadata = (~active_otadata) & 1;
            new_seq = poom_boot_policy_compute_ota_seq_(otadata[active_otadata].ota_seq,
                                                        target_slot,
                                                        bs->app_count);
        }
    } else {
        next_otadata = 0;
        new_seq = target_slot + 1U;
    }

    return poom_boot_policy_write_otadata_(bs, otadata, new_seq, (uint8_t) next_otadata);
}
