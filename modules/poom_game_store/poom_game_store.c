// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_game_store.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/sha256.h"
#include "miniz.h"
#include "sd_card.h"

#ifndef POOM_GAME_STORE_CATALOG_URL
#define POOM_GAME_STORE_CATALOG_URL \
    "https://raw.githubusercontent.com/The-POOM/the_poom/refs/heads/main/test.json"
#endif

#define POOM_GAME_STORE_APPS_DIR             SD_CARD_PATH "/apps"
#define POOM_GAME_STORE_CATALOG_DIR          POOM_GAME_STORE_APPS_DIR "/.catalog"
#define POOM_GAME_STORE_THUMBNAILS_DIR       POOM_GAME_STORE_CATALOG_DIR "/thumbnails"
#define POOM_GAME_STORE_CATALOG_PATH         POOM_GAME_STORE_CATALOG_DIR "/catalog.json"
#define POOM_GAME_STORE_THUMBNAIL_SYNC_PATH  POOM_GAME_STORE_CATALOG_DIR "/thumbnails.sync"
#define POOM_GAME_STORE_HTTP_TIMEOUT_MS       (30000)
#define POOM_GAME_STORE_BINARY_CONNECT_TIMEOUT_MS (5000)
#define POOM_GAME_STORE_BINARY_READ_TIMEOUT_MS    (500)
#define POOM_GAME_STORE_HTTP_BUFFER_SIZE      (4096)
#define POOM_GAME_STORE_CATALOG_MAX_BYTES     (256U * 1024U)
#define POOM_GAME_STORE_CATALOG_INITIAL_BYTES (4096U)
#define POOM_GAME_STORE_MAX_GAMES             (256U)
#define POOM_GAME_STORE_MAX_BINARY_BYTES      (32U * 1024U * 1024U)
#define POOM_GAME_STORE_MAX_THUMBNAIL_BYTES   (64U * 1024U)
#define POOM_GAME_STORE_NTP_TIMEOUT_MS         (12000U)
#define POOM_GAME_STORE_VALID_TIME            ((time_t)1704067200)
#define POOM_GAME_STORE_NTP_SERVER            "pool.ntp.org"
#define POOM_GAME_STORE_BINARY_URL_PREFIX      "https://poom.stellar-iot.com/esp32-games/"

typedef struct
{
    char* data;
    size_t length;
    size_t capacity;
    esp_err_t error;
} poom_game_store_json_download_t;

typedef struct
{
    FILE* file;
    mbedtls_sha256_context sha;
    size_t received;
    size_t expected;
    poom_game_store_progress_cb_t progress_cb;
    void* progress_ctx;
    esp_err_t error;
} poom_game_store_bin_download_t;

typedef struct
{
    FILE* file;
    size_t received;
    esp_err_t error;
} poom_game_store_thumbnail_download_t;

static uint32_t poom_game_store_read_be32_(const unsigned char* value)
{
    return ((uint32_t)value[0] << 24U) |
           ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) |
           (uint32_t)value[3];
}

static bool poom_game_store_is_safe_id_(const char* id)
{
    size_t len;

    if(id == NULL)
    {
        return false;
    }
    len = strlen(id);
    if((len == 0U) || (len >= POOM_GAME_STORE_ID_MAX) ||
       ((len + strlen(".png.part")) > CONFIG_FATFS_MAX_LFN))
    {
        return false;
    }
    for(size_t i = 0U; i < len; i++)
    {
        const unsigned char ch = (unsigned char)id[i];
        if(!(isalnum(ch) || (ch == '-') || (ch == '_')))
        {
            return false;
        }
    }
    return true;
}

static bool poom_game_store_is_safe_catalog_version_(const char* version)
{
    size_t len;

    if(version == NULL)
    {
        return false;
    }
    len = strlen(version);
    if((len == 0U) || (len >= POOM_GAME_STORE_VERSION_MAX))
    {
        return false;
    }
    for(size_t i = 0U; i < len; i++)
    {
        const unsigned char ch = (unsigned char)version[i];
        if(!(isalnum(ch) || (ch == '.') || (ch == '-') || (ch == '_')))
        {
            return false;
        }
    }
    return true;
}

static bool poom_game_store_is_safe_filename_(const char* filename)
{
    size_t len;

    if(filename == NULL)
    {
        return false;
    }

    len = strlen(filename);
    if((len < 5U) || (len >= POOM_GAME_STORE_STORAGE_FILENAME_MAX) ||
       ((len + strlen(".part")) > CONFIG_FATFS_MAX_LFN) ||
       (strcasecmp(filename + len - 4U, ".bin") != 0))
    {
        return false;
    }

    for(size_t i = 0U; i < len; i++)
    {
        const unsigned char ch = (unsigned char)filename[i];
        if(!(isalnum(ch) || (ch == '-') || (ch == '_') || (ch == '.')))
        {
            return false;
        }
    }

    return (strcmp(filename, ".bin") != 0) && (strstr(filename, "..") == NULL);
}

static bool poom_game_store_is_safe_category_(const char* category)
{
    size_t len;

    if(category == NULL)
    {
        return false;
    }
    len = strlen(category);
    if((len == 0U) || (len >= POOM_GAME_STORE_CATEGORY_MAX))
    {
        return false;
    }
    for(size_t i = 0U; i < len; i++)
    {
        const unsigned char ch = (unsigned char)category[i];
        if(!(isalnum(ch) || (ch == '-') || (ch == '_')))
        {
            return false;
        }
    }
    return true;
}

static bool poom_game_store_is_sha256_(const char* value)
{
    if((value == NULL) || (strlen(value) != POOM_GAME_STORE_SHA256_HEX_LEN))
    {
        return false;
    }

    for(size_t i = 0U; i < POOM_GAME_STORE_SHA256_HEX_LEN; i++)
    {
        if(!isxdigit((unsigned char)value[i]))
        {
            return false;
        }
    }
    return true;
}

static esp_err_t poom_game_store_copy_json_string_(const cJSON* object,
                                                    const char* key,
                                                    char* out,
                                                    size_t out_len)
{
    const cJSON* item;
    size_t len;

    if((object == NULL) || (key == NULL) || (out == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!cJSON_IsString(item) || (item->valuestring == NULL))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    len = strlen(item->valuestring);
    if((len == 0U) || (len >= out_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)memcpy(out, item->valuestring, len + 1U);
    return ESP_OK;
}

static esp_err_t poom_game_store_parse_game_(const cJSON* item,
                                             poom_game_store_game_t* game)
{
    const cJSON* size_item;
    const cJSON* enabled_item;
    char target[16];
    char format[24];
    esp_err_t err;

    if((item == NULL) || (game == NULL) || !cJSON_IsObject(item))
    {
        return ESP_ERR_INVALID_ARG;
    }

    enabled_item = cJSON_GetObjectItemCaseSensitive(item, "enabled");
    if(!cJSON_IsBool(enabled_item) || !cJSON_IsTrue(enabled_item))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    (void)memset(game, 0, sizeof(*game));

#define COPY_GAME_FIELD(json_key, member)                                                   \
    do                                                                                      \
    {                                                                                       \
        err = poom_game_store_copy_json_string_(item, json_key, game->member, sizeof(game->member)); \
        if(err != ESP_OK)                                                                    \
        {                                                                                    \
            return err;                                                                      \
        }                                                                                    \
    } while(0)

    COPY_GAME_FIELD("id", id);
    COPY_GAME_FIELD("name", name);
    COPY_GAME_FIELD("description", description);
    COPY_GAME_FIELD("author", author);
    COPY_GAME_FIELD("version", version);
    COPY_GAME_FIELD("category", category);
    COPY_GAME_FIELD("type", type);
    COPY_GAME_FIELD("storage_filename", storage_filename);
    COPY_GAME_FIELD("bin_url", bin_url);
    COPY_GAME_FIELD("sha256", sha256);

#undef COPY_GAME_FIELD

    err = poom_game_store_copy_json_string_(item, "target", target, sizeof(target));
    if((err != ESP_OK) || (strcmp(target, "esp32c5") != 0))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = poom_game_store_copy_json_string_(item, "format", format, sizeof(format));
    if((err != ESP_OK) || (strcmp(format, "application-bin") != 0))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_item = cJSON_GetObjectItemCaseSensitive(item, "size");
    if(!cJSON_IsNumber(size_item) || (size_item->valuedouble <= 0.0) ||
       (size_item->valuedouble > (double)POOM_GAME_STORE_MAX_BINARY_BYTES))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    game->size = (size_t)size_item->valuedouble;
    if((double)game->size != size_item->valuedouble)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if(!poom_game_store_is_safe_id_(game->id) ||
       !poom_game_store_is_safe_category_(game->category) ||
       !poom_game_store_is_safe_filename_(game->storage_filename) ||
       !poom_game_store_is_sha256_(game->sha256) ||
       (strncmp(game->bin_url,
                POOM_GAME_STORE_BINARY_URL_PREFIX,
                strlen(POOM_GAME_STORE_BINARY_URL_PREFIX)) != 0))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t poom_game_store_json_event_(esp_http_client_event_t* event)
{
    poom_game_store_json_download_t* download;
    size_t required;
    size_t new_capacity;
    char* resized;

    if(event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    download = (poom_game_store_json_download_t*)event->user_data;
    if((download == NULL) || (event->event_id != HTTP_EVENT_ON_DATA) || (event->data_len <= 0))
    {
        return ESP_OK;
    }

    required = download->length + (size_t)event->data_len + 1U;
    if((required < download->length) || (required > POOM_GAME_STORE_CATALOG_MAX_BYTES))
    {
        download->error = ESP_ERR_INVALID_SIZE;
        return download->error;
    }

    if(required > download->capacity)
    {
        new_capacity = download->capacity;
        while(new_capacity < required)
        {
            new_capacity *= 2U;
        }
        if(new_capacity > POOM_GAME_STORE_CATALOG_MAX_BYTES)
        {
            new_capacity = POOM_GAME_STORE_CATALOG_MAX_BYTES;
        }

        resized = (char*)realloc(download->data, new_capacity);
        if(resized == NULL)
        {
            download->error = ESP_ERR_NO_MEM;
            return download->error;
        }
        download->data = resized;
        download->capacity = new_capacity;
    }

    (void)memcpy(download->data + download->length, event->data, (size_t)event->data_len);
    download->length += (size_t)event->data_len;
    download->data[download->length] = '\0';
    return ESP_OK;
}

static esp_err_t poom_game_store_thumbnail_event_(esp_http_client_event_t* event)
{
    poom_game_store_thumbnail_download_t* download;
    size_t data_len;

    if(event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    download = (poom_game_store_thumbnail_download_t*)event->user_data;
    if((download == NULL) || (event->event_id != HTTP_EVENT_ON_DATA) || (event->data_len <= 0))
    {
        return ESP_OK;
    }

    data_len = (size_t)event->data_len;
    if((download->received > POOM_GAME_STORE_MAX_THUMBNAIL_BYTES) ||
       (data_len > (POOM_GAME_STORE_MAX_THUMBNAIL_BYTES - download->received)))
    {
        download->error = ESP_ERR_INVALID_SIZE;
        return download->error;
    }
    if(fwrite(event->data, 1U, data_len, download->file) != data_len)
    {
        download->error = POOM_GAME_STORE_ERR_SD_WRITE;
        return download->error;
    }
    download->received += data_len;
    return ESP_OK;
}

static esp_http_client_handle_t poom_game_store_http_init_(const char* url,
                                                           http_event_handle_cb event_cb,
                                                           void* user_data)
{
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = event_cb,
        .user_data = user_data,
        .timeout_ms = POOM_GAME_STORE_HTTP_TIMEOUT_MS,
        .buffer_size = POOM_GAME_STORE_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    return esp_http_client_init(&config);
}

static esp_err_t poom_game_store_commit_file_(const char* part_path, const char* final_path)
{
    char backup_path[POOM_GAME_STORE_LOCAL_PATH_MAX + 5U];
    struct stat st;
    bool had_existing = false;
    int written;

    written = snprintf(backup_path, sizeof(backup_path), "%s.bak", final_path);
    if((written < 0) || ((size_t)written >= sizeof(backup_path)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)unlink(backup_path);
    if(stat(final_path, &st) == 0)
    {
        if(!S_ISREG(st.st_mode) || (rename(final_path, backup_path) != 0))
        {
            printf("[E] [poom_game_store] backup rename failed: errno=%d\n", errno);
            return POOM_GAME_STORE_ERR_SD_COMMIT;
        }
        had_existing = true;
    }

    if(rename(part_path, final_path) != 0)
    {
        if(had_existing)
        {
            (void)rename(backup_path, final_path);
        }
        printf("[E] [poom_game_store] final rename failed: errno=%d\n", errno);
        return POOM_GAME_STORE_ERR_SD_COMMIT;
    }

    if(had_existing)
    {
        (void)unlink(backup_path);
    }
    return ESP_OK;
}

static esp_err_t poom_game_store_prepare_catalog_storage_(void)
{
    struct stat st;
    esp_err_t err = poom_game_store_prepare_storage();

    if(err != ESP_OK)
    {
        return err;
    }
    err = sd_card_create_dir("/apps/.catalog");
    if((err != ESP_OK) || (stat(POOM_GAME_STORE_CATALOG_DIR, &st) != 0) ||
       !S_ISDIR(st.st_mode))
    {
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }
    return ESP_OK;
}

static esp_err_t poom_game_store_prepare_thumbnail_storage_(void)
{
    struct stat st;
    esp_err_t err = poom_game_store_prepare_catalog_storage_();

    if(err != ESP_OK)
    {
        return err;
    }
    err = sd_card_create_dir("/apps/.catalog/thumbnails");
    if((err != ESP_OK) || (stat(POOM_GAME_STORE_THUMBNAILS_DIR, &st) != 0) ||
       !S_ISDIR(st.st_mode))
    {
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }
    return ESP_OK;
}

static esp_err_t poom_game_store_get_thumbnail_path_(const poom_game_store_game_t* game,
                                                     char* out_path,
                                                     size_t out_path_len)
{
    int written;

    if((game == NULL) || (out_path == NULL) || (out_path_len == 0U) ||
       !poom_game_store_is_safe_id_(game->id))
    {
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(out_path, out_path_len, "%s/%s.png",
                       POOM_GAME_STORE_THUMBNAILS_DIR, game->id);
    return ((written < 0) || ((size_t)written >= out_path_len)) ?
        ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t poom_game_store_get_thumbnail_url_(const poom_game_store_game_t* game,
                                                    char* out_url,
                                                    size_t out_url_len)
{
    const char* slash;
    size_t directory_len;
    int written;

    if((game == NULL) || (out_url == NULL) || (out_url_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    slash = strrchr(game->bin_url, '/');
    if(slash == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    directory_len = (size_t)(slash - game->bin_url);
    written = snprintf(out_url, out_url_len, "%.*s/thumbnail.png",
                       (int)directory_len, game->bin_url);
    return ((written < 0) || ((size_t)written >= out_url_len)) ?
        ESP_ERR_INVALID_SIZE : ESP_OK;
}

static unsigned char poom_game_store_png_paeth_(unsigned char left,
                                                unsigned char above,
                                                unsigned char upper_left)
{
    const int prediction = (int)left + (int)above - (int)upper_left;
    const int left_distance = abs(prediction - (int)left);
    const int above_distance = abs(prediction - (int)above);
    const int upper_left_distance = abs(prediction - (int)upper_left);

    if((left_distance <= above_distance) && (left_distance <= upper_left_distance))
    {
        return left;
    }
    return (above_distance <= upper_left_distance) ? above : upper_left;
}

static unsigned char poom_game_store_png_packed_sample_(const unsigned char* row,
                                                        size_t pixel,
                                                        unsigned bit_depth)
{
    const size_t bit_offset = pixel * bit_depth;
    const unsigned shift = 8U - bit_depth - (unsigned)(bit_offset & 7U);
    const unsigned mask = (1U << bit_depth) - 1U;
    return (unsigned char)((row[bit_offset >> 3U] >> shift) & mask);
}

static esp_err_t poom_game_store_load_thumbnail_path_(const char* path,
                                                      uint8_t* out_bitmap,
                                                      size_t out_bitmap_len)
{
    static const unsigned char png_signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    };
    FILE* file = NULL;
    unsigned char* png = NULL;
    unsigned char* compressed = NULL;
    unsigned char* scanlines = NULL;
    tinfl_decompressor* inflater = NULL;
    const unsigned char* palette = NULL;
    const unsigned char* transparency = NULL;
    size_t palette_len = 0U;
    size_t transparency_len = 0U;
    size_t compressed_len = 0U;
    size_t offset = sizeof(png_signature);
    size_t row_bytes = 0U;
    size_t scanline_bytes = 0U;
    size_t filter_bytes_per_pixel = 0U;
    struct stat st;
    uint32_t width = 0U;
    uint32_t height = 0U;
    unsigned bit_depth = 0U;
    unsigned color_type = 0U;
    unsigned channels = 0U;
    bool have_header = false;
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;

    if((path == NULL) || (out_bitmap == NULL) ||
       (out_bitmap_len < POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if((stat(path, &st) != 0) || !S_ISREG(st.st_mode) || (st.st_size <= 0) ||
       ((uint64_t)st.st_size > POOM_GAME_STORE_MAX_THUMBNAIL_BYTES))
    {
        return ESP_ERR_NOT_FOUND;
    }

    png = (unsigned char*)malloc((size_t)st.st_size);
    if(png == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    file = fopen(path, "rb");
    if((file == NULL) || (fread(png, 1U, (size_t)st.st_size, file) != (size_t)st.st_size))
    {
        err = POOM_GAME_STORE_ERR_SD_READ;
        goto cleanup;
    }
    (void)fclose(file);
    file = NULL;
    if(((size_t)st.st_size < 33U) ||
       (memcmp(png, png_signature, sizeof(png_signature)) != 0))
    {
        goto cleanup;
    }

    while((offset + 12U) <= (size_t)st.st_size)
    {
        const uint32_t chunk_len = poom_game_store_read_be32_(png + offset);
        const unsigned char* type = png + offset + 4U;
        const unsigned char* data = png + offset + 8U;
        const size_t remaining = (size_t)st.st_size - offset - 12U;

        if((size_t)chunk_len > remaining)
        {
            goto cleanup;
        }
        if(memcmp(type, "IHDR", 4U) == 0)
        {
            if((chunk_len != 13U) || have_header)
            {
                goto cleanup;
            }
            width = poom_game_store_read_be32_(data);
            height = poom_game_store_read_be32_(data + 4U);
            bit_depth = data[8];
            color_type = data[9];
            if((data[10] != 0U) || (data[11] != 0U) || (data[12] != 0U))
            {
                goto cleanup;
            }
            have_header = true;
        }
        else if(memcmp(type, "PLTE", 4U) == 0)
        {
            palette = data;
            palette_len = chunk_len;
        }
        else if(memcmp(type, "tRNS", 4U) == 0)
        {
            transparency = data;
            transparency_len = chunk_len;
        }
        else if(memcmp(type, "IDAT", 4U) == 0)
        {
            if((size_t)chunk_len > ((size_t)st.st_size - compressed_len))
            {
                goto cleanup;
            }
            compressed_len += chunk_len;
        }
        offset += (size_t)chunk_len + 12U;
    }

    if(!have_header || (width != POOM_GAME_STORE_THUMBNAIL_WIDTH) ||
       (height != POOM_GAME_STORE_THUMBNAIL_HEIGHT) || (compressed_len == 0U))
    {
        goto cleanup;
    }
    switch(color_type)
    {
        case 0U:
            channels = 1U;
            if((bit_depth != 1U) && (bit_depth != 2U) &&
               (bit_depth != 4U) && (bit_depth != 8U))
            {
                goto cleanup;
            }
            break;
        case 2U:
            channels = 3U;
            if(bit_depth != 8U)
            {
                goto cleanup;
            }
            break;
        case 3U:
            channels = 1U;
            if(((bit_depth != 1U) && (bit_depth != 2U) &&
                (bit_depth != 4U) && (bit_depth != 8U)) ||
               (palette == NULL) || (palette_len < 3U))
            {
                goto cleanup;
            }
            break;
        case 4U:
            channels = 2U;
            if(bit_depth != 8U)
            {
                goto cleanup;
            }
            break;
        case 6U:
            channels = 4U;
            if(bit_depth != 8U)
            {
                goto cleanup;
            }
            break;
        default:
            goto cleanup;
    }

    row_bytes = (((size_t)width * channels * bit_depth) + 7U) / 8U;
    scanline_bytes = (row_bytes + 1U) * (size_t)height;
    filter_bytes_per_pixel = ((channels * bit_depth) + 7U) / 8U;
    if(filter_bytes_per_pixel == 0U)
    {
        filter_bytes_per_pixel = 1U;
    }

    compressed = (unsigned char*)malloc(compressed_len);
    scanlines = (unsigned char*)malloc(scanline_bytes);
    inflater = (tinfl_decompressor*)malloc(sizeof(*inflater));
    if((compressed == NULL) || (scanlines == NULL) || (inflater == NULL))
    {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    offset = sizeof(png_signature);
    size_t copied = 0U;
    while((offset + 12U) <= (size_t)st.st_size)
    {
        const uint32_t chunk_len = poom_game_store_read_be32_(png + offset);
        const unsigned char* type = png + offset + 4U;
        if(memcmp(type, "IDAT", 4U) == 0)
        {
            (void)memcpy(compressed + copied, png + offset + 8U, chunk_len);
            copied += chunk_len;
        }
        offset += (size_t)chunk_len + 12U;
    }
    size_t input_bytes = compressed_len;
    size_t output_bytes = scanline_bytes;
    tinfl_init(inflater);
    const tinfl_status inflate_status = tinfl_decompress(
        inflater,
        compressed,
        &input_bytes,
        scanlines,
        scanlines,
        &output_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if((inflate_status != TINFL_STATUS_DONE) || (output_bytes != scanline_bytes))
    {
        goto cleanup;
    }

    for(size_t y = 0U; y < height; y++)
    {
        unsigned char* row = scanlines + y * (row_bytes + 1U) + 1U;
        const unsigned char* above = (y > 0U) ? row - (row_bytes + 1U) : NULL;
        const unsigned filter = row[-1];

        if(filter > 4U)
        {
            goto cleanup;
        }
        for(size_t x = 0U; x < row_bytes; x++)
        {
            const unsigned char left = (x >= filter_bytes_per_pixel) ?
                row[x - filter_bytes_per_pixel] : 0U;
            const unsigned char up = (above != NULL) ? above[x] : 0U;
            const unsigned char upper_left = ((above != NULL) &&
                                               (x >= filter_bytes_per_pixel)) ?
                above[x - filter_bytes_per_pixel] : 0U;

            if(filter == 1U)
            {
                row[x] = (unsigned char)(row[x] + left);
            }
            else if(filter == 2U)
            {
                row[x] = (unsigned char)(row[x] + up);
            }
            else if(filter == 3U)
            {
                row[x] = (unsigned char)(row[x] +
                                         (unsigned char)(((unsigned)left + up) / 2U));
            }
            else if(filter == 4U)
            {
                row[x] = (unsigned char)(row[x] +
                    poom_game_store_png_paeth_(left, up, upper_left));
            }
        }
    }

    (void)memset(out_bitmap, 0, POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE);
    for(size_t y = 0U; y < height; y++)
    {
        const unsigned char* row = scanlines + y * (row_bytes + 1U) + 1U;
        for(size_t x = 0U; x < width; x++)
        {
            unsigned red = 0U;
            unsigned green = 0U;
            unsigned blue = 0U;
            unsigned alpha = 255U;

            if(color_type == 0U)
            {
                const unsigned sample = (bit_depth == 8U) ? row[x] :
                    poom_game_store_png_packed_sample_(row, x, bit_depth);
                const unsigned maximum = (1U << bit_depth) - 1U;
                red = green = blue = (sample * 255U) / maximum;
            }
            else if(color_type == 3U)
            {
                const unsigned index = (bit_depth == 8U) ? row[x] :
                    poom_game_store_png_packed_sample_(row, x, bit_depth);
                if(((index * 3U) + 2U) >= palette_len)
                {
                    goto cleanup;
                }
                red = palette[index * 3U];
                green = palette[index * 3U + 1U];
                blue = palette[index * 3U + 2U];
                if((transparency != NULL) && (index < transparency_len))
                {
                    alpha = transparency[index];
                }
            }
            else
            {
                const size_t pixel = x * channels;
                if((color_type == 2U) || (color_type == 6U))
                {
                    red = row[pixel];
                    green = row[pixel + 1U];
                    blue = row[pixel + 2U];
                    if(color_type == 6U)
                    {
                        alpha = row[pixel + 3U];
                    }
                }
                else
                {
                    red = green = blue = row[pixel];
                    alpha = row[pixel + 1U];
                }
            }

            if((alpha >= 128U) && (((red * 77U) + (green * 150U) + (blue * 29U)) >= 32768U))
            {
                out_bitmap[y * (width / 8U) + (x / 8U)] |=
                    (uint8_t)(1U << (7U - (x & 7U)));
            }
        }
    }
    err = ESP_OK;

cleanup:
    if(file != NULL)
    {
        (void)fclose(file);
    }
    free(inflater);
    free(scanlines);
    free(compressed);
    free(png);
    return err;
}

const char* poom_game_store_catalog_url(void)
{
    return POOM_GAME_STORE_CATALOG_URL;
}

esp_err_t poom_game_store_sync_time(void)
{
    time_t now = 0;
    esp_err_t err;

    (void)time(&now);
    if(now >= POOM_GAME_STORE_VALID_TIME)
    {
        return ESP_OK;
    }

    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(POOM_GAME_STORE_NTP_SERVER);
    err = esp_netif_sntp_init(&config);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(POOM_GAME_STORE_NTP_TIMEOUT_MS));
    if(err != ESP_OK)
    {
        return err;
    }

    (void)time(&now);
    return (now >= POOM_GAME_STORE_VALID_TIME) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t poom_game_store_parse_catalog_(const char* json,
                                                size_t json_len,
                                                poom_game_store_catalog_t* out_catalog)
{
    cJSON* root = NULL;
    const cJSON* games;
    const cJSON* item;
    const cJSON* schema;
    char target[16];
    char platform[16];
    char hash_algorithm[16];
    size_t source_count;
    size_t accepted = 0U;
    esp_err_t err = ESP_OK;

    if((json == NULL) || (json_len == 0U) || (out_catalog == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)memset(out_catalog, 0, sizeof(*out_catalog));
    root = cJSON_ParseWithLength(json, json_len);
    if(!cJSON_IsObject(root))
    {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if(!cJSON_IsNumber(schema) || (schema->valueint != 1))
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    err = poom_game_store_copy_json_string_(root, "target", target, sizeof(target));
    if((err != ESP_OK) || (strcmp(target, "esp32c5") != 0))
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    err = poom_game_store_copy_json_string_(root, "platform", platform, sizeof(platform));
    if((err != ESP_OK) || (strcmp(platform, "POOM") != 0))
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    err = poom_game_store_copy_json_string_(root, "hash_algorithm", hash_algorithm, sizeof(hash_algorithm));
    if((err != ESP_OK) || (strcasecmp(hash_algorithm, "sha256") != 0))
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    err = poom_game_store_copy_json_string_(root, "catalog_version",
                                             out_catalog->version,
                                             sizeof(out_catalog->version));
    if(err != ESP_OK)
    {
        goto cleanup;
    }
    if(!poom_game_store_is_safe_catalog_version_(out_catalog->version))
    {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    games = cJSON_GetObjectItemCaseSensitive(root, "games");
    if(!cJSON_IsArray(games))
    {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    source_count = (size_t)cJSON_GetArraySize(games);
    if((source_count == 0U) || (source_count > POOM_GAME_STORE_MAX_GAMES))
    {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    out_catalog->games = (poom_game_store_game_t*)calloc(source_count, sizeof(*out_catalog->games));
    if(out_catalog->games == NULL)
    {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    cJSON_ArrayForEach(item, games)
    {
        poom_game_store_game_t parsed;
        esp_err_t parse_err = poom_game_store_parse_game_(item, &parsed);
        if(parse_err == ESP_ERR_NOT_SUPPORTED)
        {
            continue;
        }
        if(parse_err != ESP_OK)
        {
            err = parse_err;
            goto cleanup;
        }

        for(size_t i = 0U; i < accepted; i++)
        {
            if(strcmp(out_catalog->games[i].storage_filename, parsed.storage_filename) == 0)
            {
                err = ESP_ERR_INVALID_RESPONSE;
                goto cleanup;
            }
        }

        out_catalog->games[accepted++] = parsed;
    }

    if(accepted == 0U)
    {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    out_catalog->count = accepted;

cleanup:
    if(root != NULL)
    {
        cJSON_Delete(root);
    }
    if(err != ESP_OK)
    {
        poom_game_store_free_catalog(out_catalog);
    }
    return err;
}

static esp_err_t poom_game_store_save_catalog_(const char* json, size_t json_len)
{
    const char part_path[] = POOM_GAME_STORE_CATALOG_PATH ".part";
    FILE* file = NULL;
    esp_err_t err;

    if((json == NULL) || (json_len == 0U) ||
       (json_len > POOM_GAME_STORE_CATALOG_MAX_BYTES))
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = poom_game_store_prepare_catalog_storage_();
    if(err != ESP_OK)
    {
        return err;
    }
    (void)unlink(part_path);
    file = fopen(part_path, "wb");
    if(file == NULL)
    {
        return POOM_GAME_STORE_ERR_SD_OPEN;
    }
    if(fwrite(json, 1U, json_len, file) != json_len)
    {
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_save;
    }
    if((fflush(file) != 0) || (fsync(fileno(file)) != 0))
    {
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_save;
    }
    if(fclose(file) != 0)
    {
        file = NULL;
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_save;
    }
    file = NULL;
    err = poom_game_store_commit_file_(part_path, POOM_GAME_STORE_CATALOG_PATH);

cleanup_save:
    if(file != NULL)
    {
        (void)fclose(file);
    }
    if(err != ESP_OK)
    {
        (void)unlink(part_path);
    }
    return err;
}

esp_err_t poom_game_store_fetch_catalog(poom_game_store_catalog_t* out_catalog)
{
    poom_game_store_json_download_t download = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;

    if(out_catalog == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)memset(out_catalog, 0, sizeof(*out_catalog));
    download.capacity = POOM_GAME_STORE_CATALOG_INITIAL_BYTES;
    download.data = (char*)malloc(download.capacity);
    if(download.data == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    download.data[0] = '\0';

    client = poom_game_store_http_init_(POOM_GAME_STORE_CATALOG_URL,
                                        poom_game_store_json_event_,
                                        &download);
    if(client == NULL)
    {
        err = POOM_GAME_STORE_ERR_HTTP_INIT;
        goto cleanup_fetch;
    }
    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    err = esp_http_client_perform(client);
    if(download.error != ESP_OK)
    {
        err = download.error;
    }
    else if(err == ESP_FAIL)
    {
        err = POOM_GAME_STORE_ERR_HTTP;
    }
    if(err != ESP_OK)
    {
        goto cleanup_fetch;
    }
    if(esp_http_client_get_status_code(client) != 200)
    {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup_fetch;
    }
    (void)esp_http_client_cleanup(client);
    client = NULL;

    err = poom_game_store_parse_catalog_(download.data, download.length, out_catalog);
    if(err == ESP_OK)
    {
        err = poom_game_store_save_catalog_(download.data, download.length);
    }

cleanup_fetch:
    if(client != NULL)
    {
        (void)esp_http_client_cleanup(client);
    }
    free(download.data);
    if(err != ESP_OK)
    {
        poom_game_store_free_catalog(out_catalog);
    }
    return err;
}

esp_err_t poom_game_store_load_cached_catalog(poom_game_store_catalog_t* out_catalog)
{
    FILE* file = NULL;
    char* json = NULL;
    struct stat st;
    esp_err_t err;

    if(out_catalog == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)memset(out_catalog, 0, sizeof(*out_catalog));
    err = poom_game_store_prepare_catalog_storage_();
    if(err != ESP_OK)
    {
        return err;
    }
    if((stat(POOM_GAME_STORE_CATALOG_PATH, &st) != 0) || !S_ISREG(st.st_mode))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if((st.st_size <= 0) || ((uint64_t)st.st_size > POOM_GAME_STORE_CATALOG_MAX_BYTES))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    json = (char*)malloc((size_t)st.st_size + 1U);
    if(json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    file = fopen(POOM_GAME_STORE_CATALOG_PATH, "rb");
    if((file == NULL) || (fread(json, 1U, (size_t)st.st_size, file) != (size_t)st.st_size))
    {
        err = POOM_GAME_STORE_ERR_SD_READ;
        goto cleanup_cached;
    }
    json[st.st_size] = '\0';
    err = poom_game_store_parse_catalog_(json, (size_t)st.st_size, out_catalog);

cleanup_cached:
    if(file != NULL)
    {
        (void)fclose(file);
    }
    free(json);
    return err;
}

void poom_game_store_free_catalog(poom_game_store_catalog_t* catalog)
{
    if(catalog == NULL)
    {
        return;
    }
    free(catalog->games);
    (void)memset(catalog, 0, sizeof(*catalog));
}

esp_err_t poom_game_store_prepare_storage(void)
{
    struct stat st;
    esp_err_t err;

    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    /* Use the same FatFs-native directory helper used by Settings -> SD. */
    err = sd_card_create_dir("/apps");
    if(err != ESP_OK)
    {
        printf("[E] [poom_game_store] FatFs create /apps failed: %s\n", esp_err_to_name(err));
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }
    if((stat(POOM_GAME_STORE_APPS_DIR, &st) != 0) || !S_ISDIR(st.st_mode))
    {
        printf("[E] [poom_game_store] VFS cannot access '%s': errno=%d\n",
               POOM_GAME_STORE_APPS_DIR, errno);
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }

    return ESP_OK;
}

esp_err_t poom_game_store_get_free_bytes(uint64_t* out_free_bytes)
{
    uint64_t total_bytes = 0U;
    esp_err_t err;

    if(out_free_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_free_bytes = 0U;

    err = poom_game_store_prepare_storage();
    if(err != ESP_OK)
    {
        return err;
    }

    err = esp_vfs_fat_info(SD_CARD_PATH, &total_bytes, out_free_bytes);
    if(err != ESP_OK)
    {
        printf("[E] [poom_game_store] FatFs space query failed: %s, errno=%d\n",
               esp_err_to_name(err), errno);
        *out_free_bytes = 0U;
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }

    return ESP_OK;
}

esp_err_t poom_game_store_prepare_game_storage(const poom_game_store_game_t* game)
{
    char fatfs_dir[sizeof("/apps/") + POOM_GAME_STORE_CATEGORY_MAX];
    char category_path[sizeof(POOM_GAME_STORE_APPS_DIR "/") + POOM_GAME_STORE_CATEGORY_MAX];
    char legacy_path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    char final_path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    struct stat st;
    int written;
    esp_err_t err;

    if((game == NULL) || !poom_game_store_is_safe_category_(game->category) ||
       !poom_game_store_is_safe_filename_(game->storage_filename))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_game_store_prepare_storage();
    if(err != ESP_OK)
    {
        return err;
    }

    written = snprintf(fatfs_dir, sizeof(fatfs_dir), "/apps/%s", game->category);
    if((written < 0) || ((size_t)written >= sizeof(fatfs_dir)))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    err = sd_card_create_dir(fatfs_dir);
    if(err != ESP_OK)
    {
        printf("[E] [poom_game_store] create category '%s' failed\n", fatfs_dir);
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }

    written = snprintf(category_path, sizeof(category_path), "%s/%s",
                       POOM_GAME_STORE_APPS_DIR, game->category);
    if((written < 0) || ((size_t)written >= sizeof(category_path)) ||
       (stat(category_path, &st) != 0) || !S_ISDIR(st.st_mode))
    {
        return POOM_GAME_STORE_ERR_SD_PREPARE;
    }

    err = poom_game_store_get_local_path(game, final_path, sizeof(final_path));
    if(err != ESP_OK)
    {
        return err;
    }
    written = snprintf(legacy_path, sizeof(legacy_path), "%s/%s",
                       POOM_GAME_STORE_APPS_DIR, game->storage_filename);
    if((written < 0) || ((size_t)written >= sizeof(legacy_path)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if((stat(final_path, &st) != 0) &&
       (stat(legacy_path, &st) == 0) && S_ISREG(st.st_mode))
    {
        if(rename(legacy_path, final_path) != 0)
        {
            printf("[E] [poom_game_store] migrate '%s' failed: errno=%d\n", legacy_path, errno);
            return POOM_GAME_STORE_ERR_SD_COMMIT;
        }
        printf("[I] [poom_game_store] migrated '%s' to category '%s'\n",
               game->storage_filename, game->category);
    }
    return ESP_OK;
}

esp_err_t poom_game_store_get_local_path(const poom_game_store_game_t* game,
                                         char* out_path,
                                         size_t out_path_len)
{
    int written;

    if((game == NULL) || (out_path == NULL) || (out_path_len == 0U) ||
       !poom_game_store_is_safe_category_(game->category) ||
       !poom_game_store_is_safe_filename_(game->storage_filename))
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_path, out_path_len, "%s/%s/%s",
                       POOM_GAME_STORE_APPS_DIR, game->category, game->storage_filename);
    if((written < 0) || ((size_t)written >= out_path_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t poom_game_store_verify_local(const poom_game_store_game_t* game,
                                       char* out_path,
                                       size_t out_path_len)
{
    mbedtls_sha256_context sha;
    unsigned char digest[32];
    char digest_hex[POOM_GAME_STORE_SHA256_HEX_LEN + 1U];
    unsigned char* buffer = NULL;
    FILE* file = NULL;
    struct stat st;
    size_t received = 0U;
    esp_err_t err;

    if((game == NULL) || (out_path == NULL) || (out_path_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    out_path[0] = '\0';

    err = poom_game_store_prepare_game_storage(game);
    if(err != ESP_OK)
    {
        return err;
    }
    err = poom_game_store_get_local_path(game, out_path, out_path_len);
    if(err != ESP_OK)
    {
        return err;
    }
    if((stat(out_path, &st) != 0) || !S_ISREG(st.st_mode))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if((st.st_size <= 0) || ((size_t)st.st_size != game->size))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(out_path, "rb");
    if(file == NULL)
    {
        printf("[E] [poom_game_store] open local '%s' failed: errno=%d\n", out_path, errno);
        return POOM_GAME_STORE_ERR_SD_READ;
    }
    buffer = (unsigned char*)malloc(POOM_GAME_STORE_HTTP_BUFFER_SIZE);
    if(buffer == NULL)
    {
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_init(&sha);
    if(mbedtls_sha256_starts(&sha, 0) != 0)
    {
        err = POOM_GAME_STORE_ERR_HASH;
        goto cleanup_verify;
    }

    while(received < game->size)
    {
        const size_t remaining = game->size - received;
        const size_t chunk = (remaining < POOM_GAME_STORE_HTTP_BUFFER_SIZE) ?
            remaining : POOM_GAME_STORE_HTTP_BUFFER_SIZE;
        const size_t bytes_read = fread(buffer, 1U, chunk, file);

        if(bytes_read != chunk)
        {
            printf("[E] [poom_game_store] read local '%s' failed: errno=%d\n", out_path, errno);
            err = POOM_GAME_STORE_ERR_SD_READ;
            goto cleanup_verify;
        }
        if(mbedtls_sha256_update(&sha, buffer, bytes_read) != 0)
        {
            err = POOM_GAME_STORE_ERR_HASH;
            goto cleanup_verify;
        }
        received += bytes_read;
    }

    if(mbedtls_sha256_finish(&sha, digest) != 0)
    {
        err = POOM_GAME_STORE_ERR_HASH;
        goto cleanup_verify;
    }
    for(size_t i = 0U; i < sizeof(digest); i++)
    {
        (void)snprintf(digest_hex + (i * 2U), 3U, "%02x", digest[i]);
    }
    digest_hex[POOM_GAME_STORE_SHA256_HEX_LEN] = '\0';
    err = (strcasecmp(digest_hex, game->sha256) == 0) ? ESP_OK : ESP_ERR_INVALID_CRC;

cleanup_verify:
    mbedtls_sha256_free(&sha);
    free(buffer);
    (void)fclose(file);
    return err;
}

esp_err_t poom_game_store_download(const poom_game_store_game_t* game,
                                   poom_game_store_progress_cb_t progress_cb,
                                   void* user_ctx,
                                   char* out_path,
                                   size_t out_path_len)
{
    poom_game_store_bin_download_t download = {0};
    esp_http_client_handle_t client = NULL;
    unsigned char* read_buffer = NULL;
    unsigned char digest[32];
    char digest_hex[POOM_GAME_STORE_SHA256_HEX_LEN + 1U];
    char final_path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    char part_path[POOM_GAME_STORE_LOCAL_PATH_MAX + 6U];
    uint64_t free_bytes;
    int64_t content_length;
    int written;
    int hash_status;
    unsigned read_timeouts = 0U;
    esp_err_t err;

    if((game == NULL) || !poom_game_store_is_safe_filename_(game->storage_filename) ||
       !poom_game_store_is_sha256_(game->sha256) || (game->size == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_game_store_prepare_game_storage(game);
    if(err != ESP_OK)
    {
        return err;
    }
    err = poom_game_store_get_local_path(game, final_path, sizeof(final_path));
    if(err != ESP_OK)
    {
        return err;
    }
    written = snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    if((written < 0) || ((size_t)written >= sizeof(part_path)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* A stale partial file is never trusted and must not consume the new download's space. */
    (void)unlink(part_path);

    err = poom_game_store_get_free_bytes(&free_bytes);
    if(err != ESP_OK)
    {
        return err;
    }
    if(free_bytes < (uint64_t)game->size)
    {
        return ESP_ERR_NO_MEM;
    }

    download.file = fopen(part_path, "wb");
    if(download.file == NULL)
    {
        printf("[E] [poom_game_store] open '%s' failed: errno=%d\n", part_path, errno);
        return POOM_GAME_STORE_ERR_SD_OPEN;
    }
    download.expected = game->size;
    download.progress_cb = progress_cb;
    download.progress_ctx = user_ctx;
    download.error = ESP_OK;
    mbedtls_sha256_init(&download.sha);

    hash_status = mbedtls_sha256_starts(&download.sha, 0);
    if(hash_status != 0)
    {
        err = POOM_GAME_STORE_ERR_HASH;
        goto cleanup;
    }

    read_buffer = (unsigned char*)malloc(POOM_GAME_STORE_HTTP_BUFFER_SIZE);
    if(read_buffer == NULL)
    {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    client = poom_game_store_http_init_(game->bin_url, NULL, NULL);
    if(client == NULL)
    {
        printf("[E] [poom_game_store] binary HTTP client init failed\n");
        err = POOM_GAME_STORE_ERR_HTTP_INIT;
        goto cleanup;
    }

    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    (void)esp_http_client_set_header(client, "Accept", "application/octet-stream");
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    (void)esp_http_client_set_timeout_ms(client, POOM_GAME_STORE_BINARY_CONNECT_TIMEOUT_MS);
    err = esp_http_client_open(client, 0);
    if(err != ESP_OK)
    {
        printf("[E] [poom_game_store] binary HTTP connection failed\n");
        err = POOM_GAME_STORE_ERR_HTTP;
        goto cleanup;
    }
    content_length = esp_http_client_fetch_headers(client);
    if(content_length < 0)
    {
        printf("[E] [poom_game_store] binary HTTP headers failed\n");
        err = POOM_GAME_STORE_ERR_HTTP;
        goto cleanup;
    }
    if(esp_http_client_get_status_code(client) != 200)
    {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if((content_length > 0) && ((uint64_t)content_length != (uint64_t)game->size))
    {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    (void)esp_http_client_set_timeout_ms(client, POOM_GAME_STORE_BINARY_READ_TIMEOUT_MS);
    while(download.received < download.expected)
    {
        const size_t remaining = download.expected - download.received;
        const size_t requested = (remaining < POOM_GAME_STORE_HTTP_BUFFER_SIZE) ?
            remaining : POOM_GAME_STORE_HTTP_BUFFER_SIZE;

        if((download.progress_cb != NULL) &&
           !download.progress_cb(download.received,
                                 download.expected,
                                 download.progress_ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        const int bytes_read = esp_http_client_read(client,
                                                    (char*)read_buffer,
                                                    (int)requested);
        if(bytes_read == -ESP_ERR_HTTP_EAGAIN)
        {
            read_timeouts++;
            if(read_timeouts >= (POOM_GAME_STORE_HTTP_TIMEOUT_MS /
                                 POOM_GAME_STORE_BINARY_READ_TIMEOUT_MS))
            {
                err = ESP_ERR_TIMEOUT;
                goto cleanup;
            }
            continue;
        }
        if(bytes_read < 0)
        {
            err = POOM_GAME_STORE_ERR_HTTP;
            goto cleanup;
        }
        if(bytes_read == 0)
        {
            break;
        }
        read_timeouts = 0U;

        /* B may have been pressed while the socket read was waiting. */
        if((download.progress_cb != NULL) &&
           !download.progress_cb(download.received,
                                 download.expected,
                                 download.progress_ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        if(fwrite(read_buffer, 1U, (size_t)bytes_read, download.file) !=
           (size_t)bytes_read)
        {
            err = POOM_GAME_STORE_ERR_SD_WRITE;
            goto cleanup;
        }
        if(mbedtls_sha256_update(&download.sha,
                                 read_buffer,
                                 (size_t)bytes_read) != 0)
        {
            err = POOM_GAME_STORE_ERR_HASH;
            goto cleanup;
        }
        download.received += (size_t)bytes_read;
        if((download.progress_cb != NULL) &&
           !download.progress_cb(download.received,
                                 download.expected,
                                 download.progress_ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
    }
    if(download.received != game->size)
    {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if(mbedtls_sha256_finish(&download.sha, digest) != 0)
    {
        err = POOM_GAME_STORE_ERR_HASH;
        goto cleanup;
    }
    for(size_t i = 0U; i < sizeof(digest); i++)
    {
        (void)snprintf(digest_hex + (i * 2U), 3U, "%02x", digest[i]);
    }
    digest_hex[POOM_GAME_STORE_SHA256_HEX_LEN] = '\0';
    if(strcasecmp(digest_hex, game->sha256) != 0)
    {
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    if((fflush(download.file) != 0) || (fsync(fileno(download.file)) != 0))
    {
        printf("[E] [poom_game_store] SD sync failed: errno=%d\n", errno);
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup;
    }
    if(fclose(download.file) != 0)
    {
        download.file = NULL;
        printf("[E] [poom_game_store] SD close failed: errno=%d\n", errno);
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup;
    }
    download.file = NULL;

    err = poom_game_store_commit_file_(part_path, final_path);
    if(err != ESP_OK)
    {
        goto cleanup;
    }

    if((out_path != NULL) && (out_path_len > 0U))
    {
        written = snprintf(out_path, out_path_len, "%s", final_path);
        if((written < 0) || ((size_t)written >= out_path_len))
        {
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

cleanup:
    if(client != NULL)
    {
        (void)esp_http_client_cleanup(client);
    }
    free(read_buffer);
    mbedtls_sha256_free(&download.sha);
    if(download.file != NULL)
    {
        (void)fclose(download.file);
    }
    if(err != ESP_OK)
    {
        (void)unlink(part_path);
    }
    return err;
}

esp_err_t poom_game_store_sync_thumbnail(const poom_game_store_game_t* game,
                                         bool* out_downloaded)
{
    poom_game_store_thumbnail_download_t download = {0};
    esp_http_client_handle_t client = NULL;
    uint8_t* validation_bitmap = NULL;
    char final_path[POOM_GAME_STORE_LOCAL_PATH_MAX] = {0};
    char part_path[POOM_GAME_STORE_LOCAL_PATH_MAX + 6U] = {0};
    char url[POOM_GAME_STORE_URL_MAX] = {0};
    int written;
    esp_err_t err;

    if((game == NULL) || !poom_game_store_is_safe_id_(game->id))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if(out_downloaded != NULL)
    {
        *out_downloaded = false;
    }
    err = poom_game_store_prepare_thumbnail_storage_();
    if(err != ESP_OK)
    {
        return err;
    }
    err = poom_game_store_get_thumbnail_path_(game, final_path, sizeof(final_path));
    if(err != ESP_OK)
    {
        return err;
    }

    validation_bitmap = (uint8_t*)malloc(POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE);
    if(validation_bitmap == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    if(poom_game_store_load_thumbnail_path_(final_path,
                                           validation_bitmap,
                                           POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE) == ESP_OK)
    {
        free(validation_bitmap);
        return ESP_OK;
    }

    err = poom_game_store_get_thumbnail_url_(game, url, sizeof(url));
    if(err != ESP_OK)
    {
        goto cleanup_thumbnail;
    }
    written = snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    if((written < 0) || ((size_t)written >= sizeof(part_path)))
    {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup_thumbnail;
    }
    (void)unlink(part_path);
    download.file = fopen(part_path, "wb");
    if(download.file == NULL)
    {
        err = POOM_GAME_STORE_ERR_SD_OPEN;
        goto cleanup_thumbnail;
    }

    client = poom_game_store_http_init_(url,
                                        poom_game_store_thumbnail_event_,
                                        &download);
    if(client == NULL)
    {
        err = POOM_GAME_STORE_ERR_HTTP_INIT;
        goto cleanup_thumbnail;
    }
    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    (void)esp_http_client_set_header(client, "Accept", "image/png");
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    err = esp_http_client_perform(client);
    if(download.error != ESP_OK)
    {
        err = download.error;
    }
    else if(err == ESP_FAIL)
    {
        err = POOM_GAME_STORE_ERR_HTTP;
    }
    if(err != ESP_OK)
    {
        goto cleanup_thumbnail;
    }
    if(esp_http_client_get_status_code(client) != 200)
    {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup_thumbnail;
    }
    if(download.received == 0U)
    {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup_thumbnail;
    }
    (void)esp_http_client_cleanup(client);
    client = NULL;

    if((fflush(download.file) != 0) || (fsync(fileno(download.file)) != 0))
    {
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_thumbnail;
    }
    if(fclose(download.file) != 0)
    {
        download.file = NULL;
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_thumbnail;
    }
    download.file = NULL;

    err = poom_game_store_load_thumbnail_path_(part_path,
                                               validation_bitmap,
                                               POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE);
    if(err != ESP_OK)
    {
        goto cleanup_thumbnail;
    }
    err = poom_game_store_commit_file_(part_path, final_path);
    if((err == ESP_OK) && (out_downloaded != NULL))
    {
        *out_downloaded = true;
    }

cleanup_thumbnail:
    if(client != NULL)
    {
        (void)esp_http_client_cleanup(client);
    }
    if(download.file != NULL)
    {
        (void)fclose(download.file);
    }
    if((err != ESP_OK) && (part_path[0] != '\0'))
    {
        (void)unlink(part_path);
    }
    free(validation_bitmap);
    return err;
}

esp_err_t poom_game_store_load_thumbnail(const poom_game_store_game_t* game,
                                         uint8_t* out_bitmap,
                                         size_t out_bitmap_len)
{
    char path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    esp_err_t err;

    err = poom_game_store_get_thumbnail_path_(game, path, sizeof(path));
    if(err != ESP_OK)
    {
        return err;
    }
    return poom_game_store_load_thumbnail_path_(path, out_bitmap, out_bitmap_len);
}

bool poom_game_store_thumbnails_are_synced(const char* catalog_version,
                                           size_t game_count)
{
    FILE* file = NULL;
    char saved[64];
    char expected[64];
    struct stat st;
    int written;
    bool matches = false;

    if(!poom_game_store_is_safe_catalog_version_(catalog_version) ||
       (game_count == 0U) || (game_count > POOM_GAME_STORE_MAX_GAMES) ||
       (poom_game_store_prepare_catalog_storage_() != ESP_OK))
    {
        return false;
    }
    if((stat(POOM_GAME_STORE_THUMBNAIL_SYNC_PATH, &st) != 0) ||
       !S_ISREG(st.st_mode) || (st.st_size <= 0) ||
       ((size_t)st.st_size >= sizeof(saved)))
    {
        return false;
    }
    file = fopen(POOM_GAME_STORE_THUMBNAIL_SYNC_PATH, "rb");
    if(file == NULL)
    {
        return false;
    }
    if(fread(saved, 1U, (size_t)st.st_size, file) == (size_t)st.st_size)
    {
        saved[(size_t)st.st_size] = '\0';
        written = snprintf(expected, sizeof(expected), "%s\n%u\n",
                           catalog_version, (unsigned)game_count);
        if((written > 0) && ((size_t)written < sizeof(expected)))
        {
            matches = (strcmp(saved, expected) == 0);
        }
    }
    (void)fclose(file);
    return matches;
}

esp_err_t poom_game_store_mark_thumbnails_synced(const char* catalog_version,
                                                 size_t game_count)
{
    const char part_path[] = POOM_GAME_STORE_THUMBNAIL_SYNC_PATH ".part";
    FILE* file = NULL;
    char marker[64];
    int written;
    esp_err_t err;

    if(!poom_game_store_is_safe_catalog_version_(catalog_version) ||
       (game_count == 0U) || (game_count > POOM_GAME_STORE_MAX_GAMES))
    {
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(marker, sizeof(marker), "%s\n%u\n",
                       catalog_version, (unsigned)game_count);
    if((written <= 0) || ((size_t)written >= sizeof(marker)))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    err = poom_game_store_prepare_catalog_storage_();
    if(err != ESP_OK)
    {
        return err;
    }
    (void)unlink(part_path);
    file = fopen(part_path, "wb");
    if(file == NULL)
    {
        return POOM_GAME_STORE_ERR_SD_OPEN;
    }
    if(fwrite(marker, 1U, (size_t)written, file) != (size_t)written)
    {
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_marker;
    }
    if((fflush(file) != 0) || (fsync(fileno(file)) != 0))
    {
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_marker;
    }
    if(fclose(file) != 0)
    {
        file = NULL;
        err = POOM_GAME_STORE_ERR_SD_WRITE;
        goto cleanup_marker;
    }
    file = NULL;
    err = poom_game_store_commit_file_(part_path, POOM_GAME_STORE_THUMBNAIL_SYNC_PATH);

cleanup_marker:
    if(file != NULL)
    {
        (void)fclose(file);
    }
    if(err != ESP_OK)
    {
        (void)unlink(part_path);
    }
    return err;
}

void poom_game_store_invalidate_thumbnail_sync(void)
{
    (void)unlink(POOM_GAME_STORE_THUMBNAIL_SYNC_PATH);
}
