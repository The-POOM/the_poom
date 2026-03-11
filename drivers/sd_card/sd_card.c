// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file sd_card.c
 * @brief SD card mount and file utility helpers (SPI mode, FATFS).
 */

#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_pong.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#ifndef PIN_NUM_MISO
#define PIN_NUM_MISO 20
#endif

#ifndef PIN_NUM_MOSI
#define PIN_NUM_MOSI 19
#endif

#ifndef PIN_NUM_CLK
#define PIN_NUM_CLK 21
#endif

#ifndef PIN_NUM_CS
#define PIN_NUM_CS 18
#endif

#define SD_CARD_MOUNT_POINT             SD_CARD_PATH
#define SD_CARD_SPI_MAX_TRANSFER_SIZE   (4000U)
#define SD_CARD_MAX_OPEN_FILES          (4U)
#define SD_CARD_ALLOC_UNIT_SIZE         (16U * 1024U)
#define SD_CARD_PATH_SEPARATOR          "/"
#define SD_CARD_APPEND_MODE             "a"
#define SD_CARD_READ_MODE               "r"
#define SD_CARD_READ_BINARY_MODE        "rb"
#define SD_CARD_WRITE_MODE              "w"
#define SD_CARD_ROOT_DIR                SD_CARD_MOUNT_POINT
#define SD_CARD_OCR_SDHC_CAP_MASK       (1U << 30)

static const char *SD_CARD_TAG = "sd_card";

#if defined(CONFIG_SD_CARD_ENABLE_LOG) && CONFIG_SD_CARD_ENABLE_LOG

#define SD_CARD_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", SD_CARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define SD_CARD_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", SD_CARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define SD_CARD_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", SD_CARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define SD_CARD_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", SD_CARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define SD_CARD_PRINTF_E(...)
#define SD_CARD_PRINTF_W(...)
#define SD_CARD_PRINTF_I(...)
#define SD_CARD_PRINTF_D(...)

#endif

static const char *const s_fresult_names[] = {
    "FR_OK",
    "FR_DISK_ERR",
    "FR_INT_ERR",
    "FR_NOT_READY",
    "FR_NO_FILE",
    "FR_NO_PATH",
    "FR_INVALID_NAME",
    "FR_DENIED",
    "FR_EXIST",
    "FR_INVALID_OBJECT",
    "FR_WRITE_PROTECTED",
    "FR_INVALID_DRIVE",
    "FR_NOT_ENABLED",
    "FR_NO_FILESYSTEM",
    "FR_MKFS_ABORTED",
    "FR_TIMEOUT",
    "FR_LOCKED",
    "FR_NOT_ENOUGH_CORE",
    "FR_TOO_MANY_OPEN_FILES",
    "FR_INVALID_PARAMETER",
};

static sdmmc_card_t *s_card = NULL;
static bool s_format_if_mount_failed = false;
static sd_card_info_t s_sd_card_info = {0};

/**
 * @brief Convert FatFs error code to printable string.
 */
static const char *sd_card_fresult_to_name_(FRESULT result)
{
    if ((size_t)result >= (sizeof(s_fresult_names) / sizeof(s_fresult_names[0])))
    {
        return "FR_UNKNOWN";
    }
    return s_fresult_names[result];
}

/**
 * @brief Build full path under SD mount root. Output buffer must be freed.
 */
static esp_err_t sd_card_build_full_path_(const char *path, char **out_full_path)
{
    size_t full_len;
    char *full_path;
    int written;

    if ((path == NULL) || (out_full_path == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    full_len = strlen(SD_CARD_MOUNT_POINT) + strlen(SD_CARD_PATH_SEPARATOR) + strlen(path) + 1U;
    full_path = (char *)malloc(full_len);
    if (full_path == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    written = snprintf(full_path, full_len, "%s/%s", SD_CARD_MOUNT_POINT, path);
    if ((written < 0) || ((size_t)written >= full_len))
    {
        free(full_path);
        return ESP_FAIL;
    }

    *out_full_path = full_path;
    return ESP_OK;
}

/**
 * @brief Fill cache with parsed card metadata.
 */
static esp_err_t sd_card_fill_info_(const sdmmc_card_t *card)
{
    if (card == NULL)
    {
        SD_CARD_PRINTF_E("card pointer is null");
        return ESP_ERR_INVALID_ARG;
    }

    s_sd_card_info.name = card->cid.name;
    s_sd_card_info.total_space =
        ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024U * 1024U);
    s_sd_card_info.speed = (card->real_freq_khz < 1000U)
                               ? (float)card->real_freq_khz
                               : ((float)card->real_freq_khz / 1000.0F);

    if (card->is_sdio)
    {
        s_sd_card_info.type = "SDIO";
    }
    else if (card->is_mmc)
    {
        s_sd_card_info.type = "MMC";
    }
    else
    {
        s_sd_card_info.type = ((card->ocr & SD_CARD_OCR_SDHC_CAP_MASK) != 0U) ? "SDHC/SDXC" : "SDSC";
    }

    return ESP_OK;
}

/**
 * @brief Mount SD card filesystem through SDSPI.
 */
static esp_err_t sd_card_mount_internal_(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = s_format_if_mount_failed,
        .max_files = SD_CARD_MAX_OPEN_FILES,
        .allocation_unit_size = SD_CARD_ALLOC_UNIT_SIZE,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_CARD_SPI_MAX_TRANSFER_SIZE,
    };
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_err_t ret;

    SD_CARD_PRINTF_I("mounting SD (format_if_mount_failed=%s)",
                     s_format_if_mount_failed ? "true" : "false");

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        SD_CARD_PRINTF_E("spi_bus_initialize failed (%s)", esp_err_to_name(ret));
        return ESP_ERR_NO_MEM;
    }

    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            SD_CARD_PRINTF_E("mount failed: FAT format required");
            (void)spi_bus_free(host.slot);
            return ESP_ERR_NOT_SUPPORTED;
        }

        SD_CARD_PRINTF_E("card init failed (%s)", esp_err_to_name(ret));
        (void)spi_bus_free(host.slot);
        return ESP_ERR_NOT_FOUND;
    }

    (void)sd_card_fill_info_(s_card);
    return ESP_OK;
}

/**
 * @brief Unmount SD card filesystem and release SPI bus.
 */
static esp_err_t sd_card_unmount_internal_(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    if (s_card == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card) != ESP_OK)
    {
        SD_CARD_PRINTF_E("sdcard unmount failed");
        return ESP_FAIL;
    }

    if (spi_bus_free(host.slot) != ESP_OK)
    {
        SD_CARD_PRINTF_E("spi_bus_free failed");
        return ESP_FAIL;
    }

    s_card = NULL;
    SD_CARD_PRINTF_I("SD card unmounted");
    return ESP_OK;
}

void sd_card_printf_files(void)
{
    DIR *dir;
    struct dirent *ent;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_W("SD card not mounted");
        return;
    }

    dir = opendir(SD_CARD_ROOT_DIR);
    if (dir == NULL)
    {
        SD_CARD_PRINTF_E("failed to open directory: %s", SD_CARD_ROOT_DIR);
        return;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        SD_CARD_PRINTF_I("entry: %s", ent->d_name);
    }
    (void)closedir(dir);
}

void sd_card_begin(void)
{
    SD_CARD_PRINTF_D("module init");
}

esp_err_t sd_card_mount(void)
{
    esp_err_t err;

    if (sd_card_is_mounted())
    {
        SD_CARD_PRINTF_W("SD card already mounted");
        return ESP_OK;
    }

    err = sd_card_mount_internal_();
    if (err != ESP_OK)
    {
        SD_CARD_PRINTF_E("mount failed (%s)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sd_card_unmount(void)
{
    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_W("SD card already unmounted");
        return ESP_OK;
    }
    return sd_card_unmount_internal_();
}

esp_err_t sd_card_check_format(void)
{
    esp_err_t err;

    s_format_if_mount_failed = true;
    err = sd_card_mount();
    s_format_if_mount_failed = false;
    return err;
}

esp_err_t sd_card_format(void)
{
    esp_err_t err = ESP_OK;

    if (sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if (err != ESP_OK)
        {
            SD_CARD_PRINTF_E("mount before format failed");
            return ESP_FAIL;
        }
    }

    err = esp_vfs_fat_sdcard_format(SD_CARD_MOUNT_POINT, s_card);
    if (err != ESP_OK)
    {
        SD_CARD_PRINTF_E("format failed (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return (s_card != NULL);
}

bool sd_card_is_not_mounted(void)
{
    return !sd_card_is_mounted();
}

esp_err_t sd_card_create_dir(const char *dir_name)
{
    FRESULT res;

    if (dir_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    res = f_mkdir(dir_name);
    if (res == FR_OK)
    {
        return ESP_OK;
    }
    if (res == FR_EXIST)
    {
        return ESP_OK;
    }

    SD_CARD_PRINTF_E("create dir failed: %s", sd_card_fresult_to_name_(res));
    return ESP_FAIL;
}

esp_err_t sd_card_list_files(const char *path)
{
    FF_DIR dp;
    FRESULT res;

    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    res = f_opendir(&dp, path);
    if (res != FR_OK)
    {
        SD_CARD_PRINTF_E("open dir failed: %s", sd_card_fresult_to_name_(res));
        return ESP_FAIL;
    }
    (void)f_closedir(&dp);
    return ESP_OK;
}

esp_err_t sd_card_create_file(const char *path)
{
    FILE *file = NULL;
    char *full_path = NULL;
    esp_err_t err;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_E("SD card not mounted");
        return ESP_FAIL;
    }

    err = sd_card_build_full_path_(path, &full_path);
    if (err != ESP_OK)
    {
        return err;
    }

    file = fopen(full_path, SD_CARD_READ_MODE);
    if (file != NULL)
    {
        (void)fclose(file);
        free(full_path);
        return ESP_ERR_FILE_EXISTS;
    }

    file = fopen(full_path, SD_CARD_WRITE_MODE);
    if (file == NULL)
    {
        SD_CARD_PRINTF_E("failed to create file: %s", full_path);
        free(full_path);
        return ESP_FAIL;
    }

    (void)fclose(file);
    free(full_path);
    return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path)
{
    char *full_path = NULL;
    FILE *file = NULL;
    char line[1024];
    esp_err_t err;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_E("SD card not mounted");
        return ESP_FAIL;
    }
    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = sd_card_build_full_path_(path, &full_path);
    if (err != ESP_OK)
    {
        return err;
    }

    file = fopen(full_path, SD_CARD_READ_MODE);
    if (file == NULL)
    {
        SD_CARD_PRINTF_E("failed to open file: %s", full_path);
        free(full_path);
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), file) != NULL)
    {
        char *pos = strchr(line, '\n');
        if (pos != NULL)
        {
            *pos = '\0';
        }
        SD_CARD_PRINTF_I("%s", line);
    }

    (void)fclose(file);
    free(full_path);
    return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const char *data)
{
    FILE *file = NULL;
    char *full_path = NULL;
    esp_err_t err;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_E("SD card not mounted");
        return ESP_FAIL;
    }
    if ((path == NULL) || (data == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = sd_card_build_full_path_(path, &full_path);
    if (err != ESP_OK)
    {
        return err;
    }

    file = fopen(full_path, SD_CARD_WRITE_MODE);
    if (file == NULL)
    {
        SD_CARD_PRINTF_E("open for write failed: %s", full_path);
        free(full_path);
        return ESP_FAIL;
    }

    if (fputs(data, file) < 0)
    {
        (void)fclose(file);
        free(full_path);
        SD_CARD_PRINTF_E("write failed: %s", full_path);
        return ESP_FAIL;
    }

    (void)fclose(file);
    free(full_path);
    return ESP_OK;
}

esp_err_t sd_card_append_to_file(const char *path, const char *data)
{
    FILE *file = NULL;
    char *full_path = NULL;
    esp_err_t err;
    size_t data_len;
    size_t written;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_E("SD card not mounted");
        return ESP_FAIL;
    }
    if ((path == NULL) || (data == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = sd_card_build_full_path_(path, &full_path);
    if (err != ESP_OK)
    {
        return err;
    }

    file = fopen(full_path, SD_CARD_APPEND_MODE);
    if (file == NULL)
    {
        SD_CARD_PRINTF_E("open for append failed: %s", full_path);
        free(full_path);
        return ESP_FAIL;
    }

    data_len = strlen(data);
    written = fwrite(data, 1U, data_len, file);
    if (written != data_len)
    {
        SD_CARD_PRINTF_E("short append write: wrote %u of %u bytes",
                         (unsigned)written,
                         (unsigned)data_len);
        (void)fclose(file);
        free(full_path);
        return ESP_FAIL;
    }

    if (fflush(file) != 0)
    {
        SD_CARD_PRINTF_E("fflush failed");
        (void)fclose(file);
        free(full_path);
        return ESP_FAIL;
    }

    (void)fclose(file);
    free(full_path);
    return ESP_OK;
}

sd_card_info_t sd_card_get_info(void)
{
    return s_sd_card_info;
}

esp_err_t sd_card_read_file_to_buffer(const char *path, uint8_t **out_data, size_t *out_size)
{
    char *full_path = NULL;
    FILE *file = NULL;
    long file_size_long;
    uint8_t *buf = NULL;
    size_t file_size;
    size_t read_size;
    esp_err_t err;

    if (sd_card_is_not_mounted())
    {
        SD_CARD_PRINTF_E("SD card not mounted");
        return ESP_FAIL;
    }
    if ((path == NULL) || (out_data == NULL) || (out_size == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = sd_card_build_full_path_(path, &full_path);
    if (err != ESP_OK)
    {
        return err;
    }

    file = fopen(full_path, SD_CARD_READ_BINARY_MODE);
    if (file == NULL)
    {
        free(full_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        (void)fclose(file);
        free(full_path);
        return ESP_FAIL;
    }

    file_size_long = ftell(file);
    if (file_size_long < 0)
    {
        (void)fclose(file);
        free(full_path);
        return ESP_FAIL;
    }

    file_size = (size_t)file_size_long;
    (void)rewind(file);

    buf = (uint8_t *)malloc(file_size + 1U);
    if (buf == NULL)
    {
        (void)fclose(file);
        free(full_path);
        return ESP_ERR_NO_MEM;
    }

    read_size = fread(buf, 1U, file_size, file);
    (void)fclose(file);

    if (read_size != file_size)
    {
        free(buf);
        free(full_path);
        return ESP_FAIL;
    }

    buf[file_size] = '\0';
    *out_data = buf;
    *out_size = file_size;

    SD_CARD_PRINTF_I("read %u bytes from %s", (unsigned)*out_size, full_path);
    free(full_path);
    return ESP_OK;
}
