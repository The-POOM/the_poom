/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 THE POOM
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "bootloader_hooks.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bsp_pong.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

#define POOM_BOOT_POLICY_RESCUE_HOLD_S (1U)
#define POOM_BOOT_POLICY_RESCUE_LEVEL  (0U)
#define POOM_BOOT_POLICY_BASE_SLOT     (0)
#define POOM_BOOT_POLICY_GAME_SLOT     (1)

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);
static bool poom_boot_policy_rescue_held_(void);
static uint32_t poom_boot_policy_compute_ota_seq_(uint32_t current_seq, uint32_t target_slot, uint8_t ota_app_count);
static esp_err_t poom_boot_policy_write_otadata_(const bootloader_state_t *bs, esp_ota_select_entry_t *otadata, uint32_t seq, uint8_t sec_id);
static esp_err_t poom_boot_policy_set_next_boot_slot_(const bootloader_state_t *bs, uint32_t target_slot);

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_before_init) {
        bootloader_before_init();
    }

    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    if (bootloader_after_init) {
        bootloader_after_init();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    bootloader_utility_load_boot_image(&bs, boot_index);
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    return selected_boot_partition(bs);
}

static int selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);

    if (boot_index == INVALID_INDEX) {
        return boot_index;
    }

    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP && poom_boot_policy_rescue_held_()) {
        ESP_LOGW(TAG, "Rescue button held, forcing ota_0");
        (void) poom_boot_policy_set_next_boot_slot_(bs, POOM_BOOT_POLICY_BASE_SLOT);
        return POOM_BOOT_POLICY_BASE_SLOT;
    }

    if (boot_index == POOM_BOOT_POLICY_GAME_SLOT) {
        const esp_err_t err = poom_boot_policy_set_next_boot_slot_(bs, POOM_BOOT_POLICY_BASE_SLOT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to arm return to ota_0, refusing ota_1 boot (%s)", esp_err_to_name(err));
            return POOM_BOOT_POLICY_BASE_SLOT;
        }
        ESP_LOGI(TAG, "ota_1 selected for one-shot boot, next boot forced to ota_0");
    }

    return boot_index;
}

static bool poom_boot_policy_rescue_held_(void)
{
    return (bootloader_common_check_long_hold_gpio_level(PIN_NUM_B,
                                                         POOM_BOOT_POLICY_RESCUE_HOLD_S,
                                                         POOM_BOOT_POLICY_RESCUE_LEVEL) == GPIO_LONG_HOLD);
}

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

static esp_err_t poom_boot_policy_write_otadata_(const bootloader_state_t *bs,
                                                 esp_ota_select_entry_t *otadata,
                                                 uint32_t seq,
                                                 uint8_t sec_id)
{
    esp_err_t err;
    const uint32_t offset = bs->ota_info.offset + (FLASH_SECTOR_SIZE * sec_id);
    const bool write_encrypted = esp_flash_encryption_enabled();

    if ((otadata == NULL) || (sec_id > 1U)) {
        return ESP_ERR_INVALID_ARG;
    }

    otadata[sec_id].ota_seq = seq;
    otadata[sec_id].ota_state = ESP_OTA_IMG_VALID;
    otadata[sec_id].crc = bootloader_common_ota_select_crc(&otadata[sec_id]);

    err = bootloader_flash_erase_sector(offset / FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        return err;
    }

    return bootloader_flash_write(offset, &otadata[sec_id], sizeof(esp_ota_select_entry_t), write_encrypted);
}

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

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
