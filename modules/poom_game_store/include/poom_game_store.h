// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_GAME_STORE_H
#define POOM_GAME_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define POOM_GAME_STORE_ID_MAX               (96U)
#define POOM_GAME_STORE_NAME_MAX             (96U)
#define POOM_GAME_STORE_DESCRIPTION_MAX      (160U)
#define POOM_GAME_STORE_AUTHOR_MAX           (64U)
#define POOM_GAME_STORE_VERSION_MAX          (24U)
#define POOM_GAME_STORE_CATEGORY_MAX         (32U)
#define POOM_GAME_STORE_TYPE_MAX             (16U)
#define POOM_GAME_STORE_STORAGE_FILENAME_MAX (128U)
#define POOM_GAME_STORE_URL_MAX              (384U)
#define POOM_GAME_STORE_SHA256_HEX_LEN       (64U)
#define POOM_GAME_STORE_LOCAL_PATH_MAX       (256U)
#define POOM_GAME_STORE_THUMBNAIL_WIDTH       (128U)
#define POOM_GAME_STORE_THUMBNAIL_HEIGHT      (64U)
#define POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE \
    ((POOM_GAME_STORE_THUMBNAIL_WIDTH * POOM_GAME_STORE_THUMBNAIL_HEIGHT) / 8U)

#define POOM_GAME_STORE_ERR_BASE       (0x20000)
#define POOM_GAME_STORE_ERR_SD_PREPARE (POOM_GAME_STORE_ERR_BASE + 0x01)
#define POOM_GAME_STORE_ERR_SD_OPEN    (POOM_GAME_STORE_ERR_BASE + 0x02)
#define POOM_GAME_STORE_ERR_SD_WRITE   (POOM_GAME_STORE_ERR_BASE + 0x03)
#define POOM_GAME_STORE_ERR_SD_COMMIT  (POOM_GAME_STORE_ERR_BASE + 0x04)
#define POOM_GAME_STORE_ERR_HASH       (POOM_GAME_STORE_ERR_BASE + 0x05)
#define POOM_GAME_STORE_ERR_HTTP_INIT  (POOM_GAME_STORE_ERR_BASE + 0x06)
#define POOM_GAME_STORE_ERR_HTTP       (POOM_GAME_STORE_ERR_BASE + 0x07)
#define POOM_GAME_STORE_ERR_SD_READ    (POOM_GAME_STORE_ERR_BASE + 0x08)

typedef struct
{
    char id[POOM_GAME_STORE_ID_MAX];
    char name[POOM_GAME_STORE_NAME_MAX];
    char description[POOM_GAME_STORE_DESCRIPTION_MAX];
    char author[POOM_GAME_STORE_AUTHOR_MAX];
    char version[POOM_GAME_STORE_VERSION_MAX];
    char category[POOM_GAME_STORE_CATEGORY_MAX];
    char type[POOM_GAME_STORE_TYPE_MAX];
    char storage_filename[POOM_GAME_STORE_STORAGE_FILENAME_MAX];
    char bin_url[POOM_GAME_STORE_URL_MAX];
    char sha256[POOM_GAME_STORE_SHA256_HEX_LEN + 1U];
    size_t size;
} poom_game_store_game_t;

typedef struct
{
    poom_game_store_game_t* games;
    size_t count;
    char version[POOM_GAME_STORE_VERSION_MAX];
} poom_game_store_catalog_t;

/**
 * Called while a game is downloaded. Return false to cancel the operation.
 */
typedef bool (*poom_game_store_progress_cb_t)(size_t received, size_t total, void* user_ctx);

/** Returns the catalog URL compiled into this firmware. */
const char* poom_game_store_catalog_url(void);

/** Synchronizes the system clock before certificate validation. */
esp_err_t poom_game_store_sync_time(void);

/** Downloads and parses the remote catalog. */
esp_err_t poom_game_store_fetch_catalog(poom_game_store_catalog_t* out_catalog);

/** Loads the last validated catalog saved in the SD cache. */
esp_err_t poom_game_store_load_cached_catalog(poom_game_store_catalog_t* out_catalog);

/** Releases all heap memory owned by a catalog. */
void poom_game_store_free_catalog(poom_game_store_catalog_t* catalog);

/** Ensures that the SD card is mounted and /sdcard/apps exists. */
esp_err_t poom_game_store_prepare_storage(void);

/** Creates `/sdcard/apps/<category>` and migrates a legacy flat file when present. */
esp_err_t poom_game_store_prepare_game_storage(const poom_game_store_game_t* game);

/** Returns currently available bytes on the mounted SD volume. */
esp_err_t poom_game_store_get_free_bytes(uint64_t* out_free_bytes);

/** Builds the final absolute SD path for one game. */
esp_err_t poom_game_store_get_local_path(const poom_game_store_game_t* game,
                                         char* out_path,
                                         size_t out_path_len);

/** Verifies size and SHA-256 of an already downloaded game on the SD card. */
esp_err_t poom_game_store_verify_local(const poom_game_store_game_t* game,
                                       char* out_path,
                                       size_t out_path_len);

/** Downloads, hashes, and atomically publishes one game on the SD card. */
esp_err_t poom_game_store_download(const poom_game_store_game_t* game,
                                   poom_game_store_progress_cb_t progress_cb,
                                   void* user_ctx,
                                   char* out_path,
                                   size_t out_path_len);

/** Downloads a missing/corrupt thumbnail into the SD catalog cache. */
esp_err_t poom_game_store_sync_thumbnail(const poom_game_store_game_t* game,
                                         bool* out_downloaded);

/** Decodes a cached 128x64 PNG into a row-major, MSB-first 1bpp bitmap. */
esp_err_t poom_game_store_load_thumbnail(const poom_game_store_game_t* game,
                                         uint8_t* out_bitmap,
                                         size_t out_bitmap_len);

/** True when thumbnail synchronization was processed for this catalog revision. */
bool poom_game_store_thumbnails_are_synced(const char* catalog_version,
                                           size_t game_count);

/** Atomically records a completed thumbnail synchronization pass. */
esp_err_t poom_game_store_mark_thumbnails_synced(const char* catalog_version,
                                                 size_t game_count);

/** Invalidates the thumbnail synchronization marker when the catalog changes. */
void poom_game_store_invalidate_thumbnail_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_GAME_STORE_H */
