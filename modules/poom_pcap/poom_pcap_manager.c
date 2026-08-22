// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_pcap_manager.c
 * @brief Implementation for `poom_pcap_manager`.
 */

#include "poom_pcap_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pcap.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"

#define POOM_PCAP_DEFAULT_BUFFER_SIZE (5U * 1024U)
#define POOM_PCAP_DEFAULT_SD_DIR "/pcaps"
#define POOM_PCAP_DEFAULT_BASE_NAME "capture"

#define POOM_PCAP_RADIOTAP_LEN 8U

/* Link-layer types (DLT) used by Wireshark for these payloads. */
#define POOM_PCAP_DLT_IEEE802_11_RADIO 127U
#define POOM_PCAP_DLT_BLUETOOTH 187U
#define POOM_PCAP_DLT_IEEE802_15_4_NOFCS 230U

typedef struct __attribute__((packed))
{
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t payload_len;
} poom_pcap_item_hdr_t;

typedef struct __attribute__((packed))
{
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} poom_pcap_global_header_t;

typedef struct __attribute__((packed))
{
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} poom_pcap_packet_header_t;

/**
 * @brief Module tag for printf-based logs.
 */
static const char *const s_tag = "poom_pcap";

/*
 * Logging note:
 * - When output mode is UART/STREAM, the UART carries binary PCAP bytes.
 *   Any textual log would corrupt the stream, so logs are suppressed.
 */
#define POOM_PCAP_LOG_ALLOWED_() ((!s_ctx.active) || (s_ctx.mode == POOM_PCAP_OUTPUT_FILE))
#define POOM_PCAP_PRINTF_E(fmt, ...) \
    do                           \
    {                            \
        if (POOM_PCAP_LOG_ALLOWED_()) \
        {                        \
            printf("[E] [%s] " fmt "\n", s_tag, ##__VA_ARGS__); \
        }                        \
    } while (0)
#define POOM_PCAP_PRINTF_W(fmt, ...) \
    do                           \
    {                            \
        if (POOM_PCAP_LOG_ALLOWED_()) \
        {                        \
            printf("[W] [%s] " fmt "\n", s_tag, ##__VA_ARGS__); \
        }                        \
    } while (0)
#define POOM_PCAP_PRINTF_I(fmt, ...) \
    do                           \
    {                            \
        if (POOM_PCAP_LOG_ALLOWED_()) \
        {                        \
            printf("[I] [%s] " fmt "\n", s_tag, ##__VA_ARGS__); \
        }                        \
    } while (0)

typedef struct
{
    SemaphoreHandle_t mutex;
    uint8_t *buffer;
    size_t buffer_size;
    size_t used;

    poom_pcap_output_mode_t mode;
    poom_pcap_capture_type_t capture_type;
    bool active;
    bool header_emitted;
    bool uart_markers;

    char sd_dir[48];
    char base_name[32];
    char file_path[128];

    pcap_file_handle_t pcap;
} poom_pcap_manager_ctx_t;

/**
 * @brief Singleton manager state.
 */
static poom_pcap_manager_ctx_t s_ctx = {0};

/**
 * @brief Maps a capture type to a PCAP DLT (link-layer type).
 *
 * @param[in] type Capture type.
 * @return DLT value for the PCAP global header.
 */
static uint32_t poom_pcap_dlt_for_type_(poom_pcap_capture_type_t type)
{
    switch (type)
    {
        case POOM_PCAP_CAPTURE_WIFI:
            return POOM_PCAP_DLT_IEEE802_11_RADIO;
        case POOM_PCAP_CAPTURE_BLUETOOTH:
            return POOM_PCAP_DLT_BLUETOOTH;
        case POOM_PCAP_CAPTURE_IEEE802154:
            return POOM_PCAP_DLT_IEEE802_15_4_NOFCS;
        default:
            return POOM_PCAP_DLT_IEEE802_11_RADIO;
    }
}

/**
 * @brief Writes raw bytes to UART using only `printf`.
 *
 * @note This emits binary bytes and is not line-safe. Host tooling must capture
 *       raw UART bytes.
 *
 * @param[in] data Byte buffer.
 * @param[in] len Number of bytes.
 */
static void poom_pcap_uart_write_bytes_(const void *data, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++)
    {
        printf("%c", (char)bytes[i]);
    }
}

/**
 * @brief Optionally emits a begin marker for UART flush blocks.
 *
 * @param[in] markers When true, prints `[BUFFER/INIT]`.
 */
static void poom_pcap_uart_block_begin_(bool markers)
{
    if (markers)
    {
        printf("[BUFFER/INIT]");
    }
}

/**
 * @brief Optionally emits an end marker for UART flush blocks.
 *
 * @param[in] markers When true, prints `[BUFFER/CLOSE]` and a newline.
 */
static void poom_pcap_uart_block_end_(bool markers)
{
    if (markers)
    {
        printf("[BUFFER/CLOSE]\n");
    }
    (void)fflush(stdout);
}

/**
 * @brief Emits the PCAP global header over UART.
 *
 * @param[in] dlt Link-layer type (DLT).
 * @param[in] markers When true, wraps output in buffer markers.
 * @return ESP_OK on success.
 */
static esp_err_t poom_pcap_emit_global_header_uart_(uint32_t dlt, bool markers)
{
    poom_pcap_global_header_t hdr = {
        .magic_number = 0xa1b2c3d4U,
        .version_major = 2U,
        .version_minor = 4U,
        .thiszone = 0,
        .sigfigs = 0U,
        .snaplen = 0x40000U,
        .network = dlt,
    };

    poom_pcap_uart_block_begin_(markers);
    poom_pcap_uart_write_bytes_(&hdr, sizeof(hdr));
    poom_pcap_uart_block_end_(markers);
    return ESP_OK;
}

/**
 * @brief Checks whether a filesystem path exists.
 *
 * @param[in] path Absolute path to test.
 * @return true When the path exists.
 * @return false Otherwise.
 */
static bool poom_pcap_path_exists_(const char *path)
{
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

/**
 * @brief Ensures SD is mounted and capture directory exists.
 *
 * @param[in] sd_dir Directory path under SD root (e.g. "/pcaps" or "pcaps").
 * @return ESP_OK on success, otherwise an ESP error code.
 */
static esp_err_t poom_pcap_make_sd_dir_(const char *sd_dir)
{
    esp_err_t ret;
    char dirbuf[64];

    if (sd_dir == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (sd_card_is_not_mounted())
    {
        sd_card_begin();
        ret = sd_card_mount();
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    if (sd_dir[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (sd_dir[0] == '/')
    {
        (void)snprintf(dirbuf, sizeof(dirbuf), "%s", sd_dir);
    }
    else
    {
        (void)snprintf(dirbuf, sizeof(dirbuf), "/%s", sd_dir);
    }

    ret = sd_card_create_dir(dirbuf);
    return ret;
}

/**
 * @brief Picks an unused capture filename under the SD mount.
 *
 * The resulting path is absolute and includes `SD_CARD_PATH`.
 *
 * @param[out] out Output buffer.
 * @param[in] out_len Output buffer size.
 * @param[in] sd_dir Capture directory starting with '/' (e.g. "/pcaps").
 * @param[in] base_name Base file name (e.g. "capture").
 * @return ESP_OK on success, otherwise an ESP error code.
 */
static esp_err_t poom_pcap_build_unique_file_path_(char *out, size_t out_len, const char *sd_dir, const char *base_name)
{
    if ((out == NULL) || (out_len == 0U) || (sd_dir == NULL) || (base_name == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 1; i < 10000; i++)
    {
        int written = snprintf(out, out_len, "%s%s/%s_%d.pcap", SD_CARD_PATH, sd_dir, base_name, i);
        if ((written < 0) || ((size_t)written >= out_len))
        {
            return ESP_ERR_NO_MEM;
        }

        if (!poom_pcap_path_exists_(out))
        {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

/**
 * @brief Best-effort WiFi 802.11 frame length calculation.
 *
 * Used to trim noisy capture buffers to a more reasonable 802.11 frame size
 * when the producer includes extra bytes. This is intentionally conservative.
 *
 * @param[in] frame 802.11 frame bytes.
 * @param[in] max_len Max available bytes.
 * @return Estimated frame length (<= max_len), or 0 on invalid input.
 */
static size_t poom_pcap_wifi_calc_len_(const uint8_t *frame, size_t max_len)
{
    if ((frame == NULL) || (max_len < 2U))
    {
        return 0U;
    }

    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (uint8_t)((fc >> 2) & 0x03U);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0x0FU);
    bool to_ds = ((fc >> 8) & 0x01U) != 0U;
    bool from_ds = ((fc >> 9) & 0x01U) != 0U;

    size_t hdr_len = 24U;
    if (type == 0x01U) /* control */
    {
        switch (subtype)
        {
            case 0x0CU: /* CTS */
            case 0x0DU: /* ACK */
                hdr_len = 10U;
                break;
            case 0x0BU: /* RTS */
                hdr_len = 16U;
                break;
            default:
                hdr_len = 16U;
                break;
        }
        return (hdr_len <= max_len) ? hdr_len : max_len;
    }

    if ((type == 0x02U) && to_ds && from_ds)
    {
        hdr_len = 30U; /* Address 4 present */
    }

    if (hdr_len > max_len)
    {
        return max_len;
    }

    if (type == 0x00U) /* management */
    {
        size_t pos = hdr_len;
        if ((subtype == 0x08U) || (subtype == 0x05U)) /* beacon / probe resp */
        {
            if (pos + 12U > max_len)
            {
                return pos;
            }
            pos += 12U; /* fixed params */
        }

        while (pos + 2U <= max_len)
        {
            uint8_t tag_len = frame[pos + 1U];
            if (pos + 2U + (size_t)tag_len > max_len)
            {
                return pos;
            }
            pos += 2U + (size_t)tag_len;
        }
        return pos;
    }

    return max_len;
}

/**
 * @brief Flushes buffered items to the active output target.
 *
 * @note Caller must hold `s_ctx.mutex`.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
static esp_err_t poom_pcap_flush_nolock_(void)
{
    esp_err_t ret = ESP_OK;
    size_t pos = 0;
    const bool markers = (s_ctx.mode == POOM_PCAP_OUTPUT_UART) && s_ctx.uart_markers;

    if (s_ctx.used == 0U)
    {
        return ESP_OK;
    }

    if ((s_ctx.mode == POOM_PCAP_OUTPUT_UART) || (s_ctx.mode == POOM_PCAP_OUTPUT_STREAM))
    {
        poom_pcap_uart_block_begin_(markers);
        while (pos + sizeof(poom_pcap_item_hdr_t) <= s_ctx.used)
        {
            poom_pcap_item_hdr_t item;
            poom_pcap_packet_header_t ph;

            (void)memcpy(&item, &s_ctx.buffer[pos], sizeof(item));
            pos += sizeof(item);

            if (pos + item.payload_len > s_ctx.used)
            {
                break;
            }

            ph.ts_sec = item.ts_sec;
            ph.ts_usec = item.ts_usec;
            ph.incl_len = item.payload_len;
            ph.orig_len = item.payload_len;

            poom_pcap_uart_write_bytes_(&ph, sizeof(ph));
            poom_pcap_uart_write_bytes_(&s_ctx.buffer[pos], item.payload_len);
            pos += item.payload_len;
        }
        poom_pcap_uart_block_end_(markers);
        s_ctx.used = 0U;
        return ESP_OK;
    }

    if (s_ctx.mode != POOM_PCAP_OUTPUT_FILE)
    {
        s_ctx.used = 0U;
        return ESP_OK;
    }

    if (s_ctx.pcap == NULL)
    {
        POOM_PCAP_PRINTF_W("file mode active but pcap handle is NULL; switching to UART");
        s_ctx.mode = POOM_PCAP_OUTPUT_UART;
        return poom_pcap_flush_nolock_();
    }

    while (pos + sizeof(poom_pcap_item_hdr_t) <= s_ctx.used)
    {
        poom_pcap_item_hdr_t item;
        void *payload_ptr;

        (void)memcpy(&item, &s_ctx.buffer[pos], sizeof(item));
        pos += sizeof(item);

        if (pos + item.payload_len > s_ctx.used)
        {
            POOM_PCAP_PRINTF_W("buffer parse error, dropping remainder");
            break;
        }

        payload_ptr = (void *)&s_ctx.buffer[pos];
        pos += item.payload_len;

        ret = pcap_capture_packet(s_ctx.pcap, payload_ptr, item.payload_len, item.ts_sec, item.ts_usec);
        if (ret != ESP_OK)
        {
            size_t item_start;
            size_t remaining;

            POOM_PCAP_PRINTF_E("pcap_capture_packet failed: %s; switching to UART", esp_err_to_name(ret));
            (void)pcap_del_session(s_ctx.pcap);
            s_ctx.pcap = NULL;
            s_ctx.file_path[0] = '\0';
            s_ctx.mode = POOM_PCAP_OUTPUT_UART;
            (void)poom_pcap_emit_global_header_uart_(poom_pcap_dlt_for_type_(s_ctx.capture_type), s_ctx.uart_markers);

            item_start = pos - (sizeof(poom_pcap_item_hdr_t) + (size_t)item.payload_len);
            remaining = s_ctx.used - item_start;
            (void)memmove(s_ctx.buffer, &s_ctx.buffer[item_start], remaining);
            s_ctx.used = remaining;
            return poom_pcap_flush_nolock_();
        }
    }

    s_ctx.used = 0U;
    return ESP_OK;
}

/**
 * @brief Resets capture state and releases backend session handle.
 *
 * @note Caller must hold `s_ctx.mutex`.
 */
static void poom_pcap_reset_capture_state_nolock_(void)
{
    s_ctx.mode = POOM_PCAP_OUTPUT_AUTO;
    s_ctx.capture_type = POOM_PCAP_CAPTURE_WIFI;
    s_ctx.active = false;
    s_ctx.header_emitted = false;
    s_ctx.used = 0U;
    s_ctx.file_path[0] = '\0';
    if (s_ctx.pcap != NULL)
    {
        (void)pcap_del_session(s_ctx.pcap);
        s_ctx.pcap = NULL;
    }
}

esp_err_t poom_pcap_manager_init(const poom_pcap_manager_config_t *config)
{
    if (s_ctx.mutex == NULL)
    {
        s_ctx.mutex = xSemaphoreCreateMutex();
        if (s_ctx.mutex == NULL)
        {
            POOM_PCAP_PRINTF_E("failed to create mutex");
            return ESP_FAIL;
        }
    }

    if (s_ctx.buffer == NULL)
    {
        size_t want = (config && (config->buffer_size != 0U)) ? config->buffer_size : POOM_PCAP_DEFAULT_BUFFER_SIZE;
        s_ctx.buffer = (uint8_t *)malloc(want);
        if (s_ctx.buffer == NULL)
        {
            POOM_PCAP_PRINTF_E("failed to allocate buffer (%u bytes)", (unsigned)want);
            return ESP_ERR_NO_MEM;
        }
        s_ctx.buffer_size = want;
        s_ctx.used = 0U;
    }

    s_ctx.uart_markers = (config != NULL) ? config->uart_markers : true;

    (void)snprintf(s_ctx.sd_dir,
                   sizeof(s_ctx.sd_dir),
                   "%s",
                   (config && config->sd_dir) ? config->sd_dir : POOM_PCAP_DEFAULT_SD_DIR);
    (void)snprintf(s_ctx.base_name,
                   sizeof(s_ctx.base_name),
                   "%s",
                   (config && config->base_name) ? config->base_name : POOM_PCAP_DEFAULT_BASE_NAME);

    if (s_ctx.sd_dir[0] == '\0')
    {
        (void)snprintf(s_ctx.sd_dir, sizeof(s_ctx.sd_dir), "%s", POOM_PCAP_DEFAULT_SD_DIR);
    }
    if (s_ctx.base_name[0] == '\0')
    {
        (void)snprintf(s_ctx.base_name, sizeof(s_ctx.base_name), "%s", POOM_PCAP_DEFAULT_BASE_NAME);
    }

    return ESP_OK;
}

esp_err_t poom_pcap_manager_deinit(void)
{
    esp_err_t ret = poom_pcap_manager_close();

    if (s_ctx.mutex != NULL)
    {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    if (s_ctx.buffer != NULL)
    {
        free(s_ctx.buffer);
        s_ctx.buffer = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    return ret;
}

/**
 * @brief Starts a UART-based capture session (UART or stream).
 *
 * Emits the PCAP global header over UART and switches the manager into the
 * requested mode.
 *
 * @note Caller must hold `s_ctx.mutex`.
 *
 * @param[in] mode Target output mode (`POOM_PCAP_OUTPUT_UART` or `POOM_PCAP_OUTPUT_STREAM`).
 * @param[in] capture_type Capture type controlling DLT/framing rules.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
static esp_err_t poom_pcap_start_uart_or_stream_nolock_(poom_pcap_output_mode_t mode, poom_pcap_capture_type_t capture_type)
{
    uint32_t dlt;

    if (s_ctx.active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.mode = mode;
    s_ctx.capture_type = capture_type;
    s_ctx.used = 0U;
    s_ctx.file_path[0] = '\0';
    s_ctx.pcap = NULL;

    dlt = poom_pcap_dlt_for_type_(capture_type);
    (void)poom_pcap_emit_global_header_uart_(dlt, (mode == POOM_PCAP_OUTPUT_UART) && s_ctx.uart_markers);
    s_ctx.header_emitted = true;
    s_ctx.active = true;
    return ESP_OK;
}

esp_err_t poom_pcap_manager_start_uart(poom_pcap_capture_type_t capture_type)
{
    if (poom_pcap_manager_init(NULL) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

esp_err_t poom_pcap_manager_start_stream(poom_pcap_capture_type_t capture_type)
{
    if (poom_pcap_manager_init(NULL) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_STREAM, capture_type);
    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

esp_err_t poom_pcap_manager_start_auto(poom_pcap_capture_type_t capture_type)
{
    return poom_pcap_manager_start_file(NULL, NULL, capture_type, true);
}

esp_err_t poom_pcap_manager_start_file(const char *base_name,
                                      const char *sd_dir,
                                      poom_pcap_capture_type_t capture_type,
                                      bool allow_uart_fallback)
{
    esp_err_t ret;
    FILE *fp = NULL;
    pcap_config_t cfg = {0};
    uint32_t dlt;
    char local_sd_dir[48];
    char local_base[32];
    char path[128];

    ret = poom_pcap_manager_init(NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ctx.active)
    {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    (void)snprintf(local_sd_dir, sizeof(local_sd_dir), "%s", (sd_dir != NULL) ? sd_dir : s_ctx.sd_dir);
    (void)snprintf(local_base, sizeof(local_base), "%s", (base_name != NULL) ? base_name : s_ctx.base_name);
    if (local_sd_dir[0] == '\0')
    {
        (void)snprintf(local_sd_dir, sizeof(local_sd_dir), "%s", POOM_PCAP_DEFAULT_SD_DIR);
    }
    if (local_base[0] == '\0')
    {
        (void)snprintf(local_base, sizeof(local_base), "%s", POOM_PCAP_DEFAULT_BASE_NAME);
    }

    s_ctx.capture_type = capture_type;
    s_ctx.used = 0U;
    s_ctx.header_emitted = false;
    s_ctx.file_path[0] = '\0';
    s_ctx.pcap = NULL;

    if (poom_pcap_make_sd_dir_(local_sd_dir) != ESP_OK)
    {
        if (!allow_uart_fallback)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ESP_FAIL;
        }
        POOM_PCAP_PRINTF_W("SD unavailable, falling back to UART");
        ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    if (local_sd_dir[0] != '/')
    {
        size_t sd_len = strnlen(local_sd_dir, sizeof(local_sd_dir));
        if ((sd_len + 1U) >= sizeof(local_sd_dir))
        {
            if (!allow_uart_fallback)
            {
                xSemaphoreGive(s_ctx.mutex);
                return ESP_ERR_INVALID_SIZE;
            }
            POOM_PCAP_PRINTF_W("sd_dir too long, falling back to UART");
            ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
            xSemaphoreGive(s_ctx.mutex);
            return ret;
        }

        memmove(&local_sd_dir[1], &local_sd_dir[0], sd_len + 1U); /* include NUL */
        local_sd_dir[0] = '/';
    }

    ret = poom_pcap_build_unique_file_path_(path, sizeof(path), local_sd_dir, local_base);
    if (ret != ESP_OK)
    {
        if (!allow_uart_fallback)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ret;
        }
        POOM_PCAP_PRINTF_W("failed to pick SD filename, falling back to UART");
        ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        POOM_PCAP_PRINTF_W("fopen failed (%d), falling back=%s", errno, allow_uart_fallback ? "true" : "false");
        if (!allow_uart_fallback)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ESP_ERR_FILE_OPEN_FAILED;
        }
        ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    cfg.fp = fp;
    cfg.major_version = PCAP_DEFAULT_VERSION_MAJOR;
    cfg.minor_version = PCAP_DEFAULT_VERSION_MINOR;
    cfg.time_zone = PCAP_DEFAULT_TIME_ZONE_GMT;
    cfg.flags.little_endian = 1U;

    ret = pcap_new_session(&cfg, &s_ctx.pcap);
    if (ret != ESP_OK)
    {
        (void)fclose(fp);
        s_ctx.pcap = NULL;
        if (!allow_uart_fallback)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ret;
        }
        POOM_PCAP_PRINTF_W("pcap_new_session failed, falling back to UART");
        ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    dlt = poom_pcap_dlt_for_type_(capture_type);
    ret = pcap_write_header(s_ctx.pcap, (pcap_link_type_t)dlt);
    if (ret != ESP_OK)
    {
        (void)pcap_del_session(s_ctx.pcap);
        s_ctx.pcap = NULL;
        if (!allow_uart_fallback)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ret;
        }
        POOM_PCAP_PRINTF_W("pcap_write_header failed, falling back to UART");
        ret = poom_pcap_start_uart_or_stream_nolock_(POOM_PCAP_OUTPUT_UART, capture_type);
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    (void)snprintf(s_ctx.file_path, sizeof(s_ctx.file_path), "%s", path);
    s_ctx.mode = POOM_PCAP_OUTPUT_FILE;
    s_ctx.header_emitted = true;
    s_ctx.active = true;

    (void)snprintf(s_ctx.sd_dir, sizeof(s_ctx.sd_dir), "%s", local_sd_dir);
    (void)snprintf(s_ctx.base_name, sizeof(s_ctx.base_name), "%s", local_base);

    POOM_PCAP_PRINTF_I("capture started: %s", s_ctx.file_path);
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t poom_pcap_manager_write_packet(const void *packet, size_t len, poom_pcap_capture_type_t type)
{
    esp_err_t ret;
    size_t payload_len = 0U;
    size_t actual_len = 0U;
    size_t need;
    struct timeval tv;
    poom_pcap_item_hdr_t item;

    if ((packet == NULL) || (len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx.mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ctx.active)
    {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (type != s_ctx.capture_type)
    {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if (type == POOM_PCAP_CAPTURE_WIFI)
    {
        actual_len = poom_pcap_wifi_calc_len_((const uint8_t *)packet, len);
        if (actual_len == 0U)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ESP_ERR_INVALID_ARG;
        }
        payload_len = POOM_PCAP_RADIOTAP_LEN + actual_len;
    }
    else if (type == POOM_PCAP_CAPTURE_IEEE802154)
    {
        payload_len = len;
        actual_len = len;
    }
    else if (type == POOM_PCAP_CAPTURE_BLUETOOTH)
    {
        payload_len = len;
        actual_len = len;
    }
    else
    {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_ARG;
    }

    need = sizeof(poom_pcap_item_hdr_t) + payload_len;
    if (need > s_ctx.buffer_size)
    {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }

    if (gettimeofday(&tv, NULL) != 0)
    {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }

    item.ts_sec = (uint32_t)tv.tv_sec;
    item.ts_usec = (uint32_t)tv.tv_usec;
    item.payload_len = (uint32_t)payload_len;

    if (s_ctx.used + need > s_ctx.buffer_size)
    {
        ret = poom_pcap_flush_nolock_();
        if (ret != ESP_OK)
        {
            xSemaphoreGive(s_ctx.mutex);
            return ret;
        }
    }

    (void)memcpy(&s_ctx.buffer[s_ctx.used], &item, sizeof(item));
    s_ctx.used += sizeof(item);

    if (type == POOM_PCAP_CAPTURE_WIFI)
    {
        const uint8_t radiotap[POOM_PCAP_RADIOTAP_LEN] = {
            0x00U, 0x00U, /* version, pad */
            0x08U, 0x00U, /* length */
            0x00U, 0x00U, 0x00U, 0x00U /* present flags */
        };
        (void)memcpy(&s_ctx.buffer[s_ctx.used], radiotap, sizeof(radiotap));
        s_ctx.used += sizeof(radiotap);
    }

    (void)memcpy(&s_ctx.buffer[s_ctx.used], packet, actual_len);
    s_ctx.used += actual_len;

    if ((s_ctx.mode == POOM_PCAP_OUTPUT_UART) || (s_ctx.mode == POOM_PCAP_OUTPUT_STREAM))
    {
        ret = poom_pcap_flush_nolock_();
        xSemaphoreGive(s_ctx.mutex);
        return ret;
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t poom_pcap_manager_flush(void)
{
    esp_err_t ret;

    if (s_ctx.mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    ret = poom_pcap_flush_nolock_();
    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

esp_err_t poom_pcap_manager_close(void)
{
    if (s_ctx.mutex == NULL)
    {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    (void)poom_pcap_flush_nolock_();
    poom_pcap_reset_capture_state_nolock_();

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

bool poom_pcap_manager_is_active(void)
{
    return s_ctx.active;
}

poom_pcap_output_mode_t poom_pcap_manager_get_mode(void)
{
    return s_ctx.mode;
}

bool poom_pcap_manager_is_stream(void)
{
    return s_ctx.active && (s_ctx.mode == POOM_PCAP_OUTPUT_STREAM);
}

bool poom_pcap_manager_is_uart(void)
{
    return s_ctx.active && (s_ctx.mode == POOM_PCAP_OUTPUT_UART);
}

const char *poom_pcap_manager_get_file_path(void)
{
    if (s_ctx.active && (s_ctx.mode == POOM_PCAP_OUTPUT_FILE) && (s_ctx.file_path[0] != '\0'))
    {
        return s_ctx.file_path;
    }
    return NULL;
}

esp_err_t poom_pcap_manager_wifi_start_monitor_mode(wifi_promiscuous_cb_t callback,
                                                    uint32_t filter_mask)
{
    esp_err_t ret;
    wifi_promiscuous_filter_t filter = {0};

    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = poom_wifi_ctrl_init_sta();
    if (ret != ESP_OK)
    {
        return ret;
    }

    (void)poom_wifi_ctrl_sta_disconnect();

    filter.filter_mask = filter_mask;
    ret = esp_wifi_set_promiscuous_filter(&filter);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_wifi_set_promiscuous_rx_cb(callback);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_wifi_set_promiscuous(true);
    if (ret != ESP_OK)
    {
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        return ret;
    }

    return ESP_OK;
}

esp_err_t poom_pcap_manager_wifi_stop_monitor_mode(void)
{
    esp_err_t ret1 = esp_wifi_set_promiscuous(false);
    esp_err_t ret2 = esp_wifi_set_promiscuous_rx_cb(NULL);

    if (ret1 != ESP_OK)
    {
        return ret1;
    }
    return ret2;
}
