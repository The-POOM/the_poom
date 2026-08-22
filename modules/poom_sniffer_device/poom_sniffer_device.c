// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_sniffer_device.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"

#ifndef POOM_SNIFFER_DEVICE_ENABLE_LOG
#define POOM_SNIFFER_DEVICE_ENABLE_LOG (1)
#endif

#define POOM_SNIFFER_DEVICE_HOP_MS (400U)
#define POOM_SNIFFER_DEVICE_SEEN_CAPACITY (64U)
#define POOM_SNIFFER_DEVICE_REPRINT_MS (3000U)
#define POOM_SNIFFER_DEVICE_WIFI_RETRY_MAX (10U)
#define POOM_SNIFFER_DEVICE_WIFI_WAIT_IP_MS (4000U)
#define POOM_SNIFFER_DEVICE_WIFI_POLL_MS (200U)
#define POOM_SNIFFER_DEVICE_WIFI_SSID_MAX (33U)
#define POOM_SNIFFER_DEVICE_WIFI_PASS_MAX (65U)
#define POOM_SNIFFER_DEVICE_NTP_SERVER "pool.ntp.org"
#define POOM_SNIFFER_DEVICE_NTP_WAIT_MS (10000U)
#define POOM_SNIFFER_DEVICE_TIME_MIN_YEAR (2024)
#define POOM_SNIFFER_DEVICE_CAPTURE_DIR "/poom_capture_snnifer"
#define POOM_SNIFFER_DEVICE_CAPTURE_PATH_MAX (128U)
#define POOM_SNIFFER_DEVICE_CAPTURE_FULL_PATH_MAX (160U)
#define POOM_SNIFFER_DEVICE_CAPTURE_DATA_MAX (320U)
#define POOM_SNIFFER_DEVICE_CAPTURE_QUEUE_LEN (256U)
#define POOM_SNIFFER_DEVICE_CAPTURE_FILE_ROTATE_BYTES (5U * 1024U * 1024U)
#define POOM_SNIFFER_DEVICE_CAPTURE_FILE_BUFFER_BYTES (4096U)
#define POOM_SNIFFER_DEVICE_CAPTURE_FLUSH_LINES (16U)
#define POOM_SNIFFER_DEVICE_CAPTURE_TASK_STACK (4096U)
#define POOM_SNIFFER_DEVICE_CAPTURE_TASK_PRIO (4U)
#define POOM_SNIFFER_DEVICE_CAPTURE_STOP_WAIT_MS (5000U)
#define POOM_SNIFFER_DEVICE_CAPTURE_STOP_POLL_MS (50U)
#define POOM_SNIFFER_DEVICE_RAW_PREVIEW_DEFAULT (16U)
#define POOM_SNIFFER_DEVICE_RAW_PREVIEW_MAX     (24U)

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t s_poom_sniffer_device_wifi_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
#else
static const uint8_t s_poom_sniffer_device_wifi_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};
#endif

static const size_t s_poom_sniffer_device_wifi_channels_count =
    sizeof(s_poom_sniffer_device_wifi_channels) / sizeof(s_poom_sniffer_device_wifi_channels[0]);

#if POOM_SNIFFER_DEVICE_ENABLE_LOG
    static const char *POOM_SNIFFER_DEVICE_TAG = "poom_sniffer_device";

    #define POOM_SNIFFER_DEVICE_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_SNIFFER_DEVICE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_SNIFFER_DEVICE_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_SNIFFER_DEVICE_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
    #define POOM_SNIFFER_DEVICE_PRINTF_E(...) do { } while (0)
    #define POOM_SNIFFER_DEVICE_PRINTF_I(...) do { } while (0)
#endif

typedef struct
{
    bool used;
    uint8_t mac[6];
    char ssid[40];
    uint32_t last_seen_ms;
} poom_sniffer_device_seen_entry_t;

typedef struct
{
    uint8_t source_mac[6];
    char ssid[40];
    char timestamp[24];
    uint32_t hash;
    int rssi;
    uint16_t sequence_number;
    uint16_t ht_cap_info;
    uint8_t channel;
} poom_sniffer_device_capture_item_t;

static bool s_poom_sniffer_device_running = false;
static bool s_poom_sniffer_device_sd_ready = false;
static bool s_poom_sniffer_device_stop_requested = false;
static uint8_t s_poom_sniffer_device_channel = 1U;
static size_t s_poom_sniffer_device_channel_idx = 0U;
static uint32_t s_poom_sniffer_device_session_start_ms = 0U;
static uint32_t s_poom_sniffer_device_capture_index = 0U;
static uint32_t s_poom_sniffer_device_session_index = 0U;
static TaskHandle_t s_poom_sniffer_device_hop_task = NULL;
static TaskHandle_t s_poom_sniffer_device_capture_task = NULL;
static QueueHandle_t s_poom_sniffer_device_capture_queue = NULL;
static bool s_poom_sniffer_device_capture_queue_full_reported = false;
static char s_poom_sniffer_device_capture_file_path[POOM_SNIFFER_DEVICE_CAPTURE_PATH_MAX];
static poom_sniffer_device_seen_entry_t s_poom_sniffer_device_seen[POOM_SNIFFER_DEVICE_SEEN_CAPACITY];
static poom_sniffer_device_config_t s_poom_sniffer_device_cfg = {
    .filter = POOM_SNIFFER_DEVICE_FILTER_PROBE,
    .channel = 0U,
    .save_to_sd = true,
    .dedupe_enabled = true,
    .attempt_time_sync = true,
    .print_packet_summary = false,
    .raw_preview_len = POOM_SNIFFER_DEVICE_RAW_PREVIEW_DEFAULT,
};

/**
 * @brief Returns current system time in milliseconds.
 * @param[in/out] none Not used.
 * @return uint32_t
 */
static uint32_t poom_sniffer_device_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief Internal helper for `poom_sniffer_device_channel_supported`.
 *
 * @param[in] channel Parameter passed to the function.
 * @param[in] out_index Parameter passed to the function.
 * @return bool
 */
static bool poom_sniffer_device_channel_supported_(uint8_t channel, size_t *out_index)
{
    size_t i;

    for(i = 0U; i < s_poom_sniffer_device_wifi_channels_count; i++)
    {
        if(s_poom_sniffer_device_wifi_channels[i] == channel)
        {
            if(out_index != NULL)
            {
                *out_index = i;
            }
            return true;
        }
    }

    return false;
}

/**
 * @brief Internal helper for `poom_sniffer_device_filter_mask`.
 *
 * @param[in] filter Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t poom_sniffer_device_filter_mask_(poom_sniffer_device_filter_t filter)
{
    switch(filter)
    {
        case POOM_SNIFFER_DEVICE_FILTER_DATA:
            return WIFI_PROMIS_FILTER_MASK_DATA;
        case POOM_SNIFFER_DEVICE_FILTER_CTRL:
            return WIFI_PROMIS_FILTER_MASK_CTRL;
        case POOM_SNIFFER_DEVICE_FILTER_ALL:
            return WIFI_PROMIS_FILTER_MASK_ALL;
        case POOM_SNIFFER_DEVICE_FILTER_BEACON:
        case POOM_SNIFFER_DEVICE_FILTER_MGMT:
        case POOM_SNIFFER_DEVICE_FILTER_PROBE:
        default:
            return WIFI_PROMIS_FILTER_MASK_MGMT;
    }
}

/**
 * @brief Internal helper for `poom_sniffer_device_frame_control`.
 *
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @param[in] out_type Parameter passed to the function.
 * @param[in] out_subtype Parameter passed to the function.
 * @return bool
 */
static bool poom_sniffer_device_frame_control_(const uint8_t *payload,
                                               size_t payload_len,
                                               uint8_t *out_type,
                                               uint8_t *out_subtype)
{
    if((payload == NULL) || (payload_len < 2U) || (out_type == NULL) || (out_subtype == NULL))
    {
        return false;
    }

    *out_type = (uint8_t)((payload[0] >> 2U) & 0x03U);
    *out_subtype = (uint8_t)((payload[0] >> 4U) & 0x0FU);
    return true;
}

/**
 * @brief Internal helper for `poom_sniffer_device_summary_type_name`.
 *
 * @param[in] pkt_type Parameter passed to the function.
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @return const char *
 */
static const char *poom_sniffer_device_summary_type_name_(wifi_promiscuous_pkt_type_t pkt_type,
                                                           const uint8_t *payload,
                                                           size_t payload_len)
{
    uint8_t type = 0U;
    uint8_t subtype = 0U;

    if(!poom_sniffer_device_frame_control_(payload, payload_len, &type, &subtype))
    {
        return (pkt_type == WIFI_PKT_DATA) ? "DATA" :
               (pkt_type == WIFI_PKT_CTRL) ? "CTRL" :
               (pkt_type == WIFI_PKT_MGMT) ? "MGMT" : "MISC";
    }

    if(type == 0U)
    {
        switch(subtype)
        {
            case 4U: return "PROBE_REQ";
            case 5U: return "PROBE_RESP";
            case 8U: return "BEACON";
            default: return "MGMT";
        }
    }

    if(type == 1U)
    {
        return "CTRL";
    }

    if(type == 2U)
    {
        return "DATA";
    }

    return "MISC";
}

/**
 * @brief Internal helper for `poom_sniffer_device_filter_accepts`.
 *
 * @param[in] pkt_type Parameter passed to the function.
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @return bool
 */
static bool poom_sniffer_device_filter_accepts_(wifi_promiscuous_pkt_type_t pkt_type,
                                                const uint8_t *payload,
                                                size_t payload_len)
{
    uint8_t type = 0U;
    uint8_t subtype = 0U;

    switch(s_poom_sniffer_device_cfg.filter)
    {
        case POOM_SNIFFER_DEVICE_FILTER_ALL:
            return true;
        case POOM_SNIFFER_DEVICE_FILTER_MGMT:
            return pkt_type == WIFI_PKT_MGMT;
        case POOM_SNIFFER_DEVICE_FILTER_DATA:
            return pkt_type == WIFI_PKT_DATA;
        case POOM_SNIFFER_DEVICE_FILTER_CTRL:
            return pkt_type == WIFI_PKT_CTRL;
        case POOM_SNIFFER_DEVICE_FILTER_BEACON:
            return (pkt_type == WIFI_PKT_MGMT) &&
                   poom_sniffer_device_frame_control_(payload, payload_len, &type, &subtype) &&
                   (type == 0U) && (subtype == 8U);
        case POOM_SNIFFER_DEVICE_FILTER_PROBE:
        default:
            return (pkt_type == WIFI_PKT_MGMT) &&
                   poom_sniffer_device_frame_control_(payload, payload_len, &type, &subtype) &&
                   (type == 0U) && ((subtype == 4U) || (subtype == 5U));
    }
}

/**
 * @brief Internal helper for `poom_sniffer_device_extract_addrs`.
 *
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @param[in] out_src Parameter passed to the function.
 * @param[in] out_dst Parameter passed to the function.
 * @return void
 */
static void poom_sniffer_device_extract_addrs_(const uint8_t *payload,
                                               size_t payload_len,
                                               uint8_t out_src[6],
                                               uint8_t out_dst[6])
{
    (void)memset(out_src, 0, 6U);
    (void)memset(out_dst, 0, 6U);

    if((payload == NULL) || (payload_len < 10U))
    {
        return;
    }

    (void)memcpy(out_dst, &payload[4], 6U);
    if(payload_len >= 16U)
    {
        (void)memcpy(out_src, &payload[10], 6U);
    }
}

/**
 * @brief Internal helper for `poom_sniffer_device_make_elapsed`.
 *
 * @param[in] out_text Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @param[in] now_ms Parameter passed to the function.
 * @return void
 */
static void poom_sniffer_device_make_elapsed_(char *out_text, size_t out_len, uint32_t now_ms)
{
    uint32_t elapsed_ms;
    uint32_t sec;
    uint32_t min;

    if((out_text == NULL) || (out_len == 0U))
    {
        return;
    }

    elapsed_ms = now_ms - s_poom_sniffer_device_session_start_ms;
    sec = elapsed_ms / 1000U;
    min = sec / 60U;
    sec %= 60U;
    elapsed_ms %= 1000U;
    (void)snprintf(out_text, out_len, "%02lu:%02lu.%03lu",
                   (unsigned long)min,
                   (unsigned long)sec,
                   (unsigned long)elapsed_ms);
}

/**
 * @brief Internal helper for `poom_sniffer_device_make_hex_line_alloc`.
 *
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @return char *
 */
static char *poom_sniffer_device_make_hex_line_alloc_(const uint8_t *payload, size_t payload_len)
{
    char *out_text;
    size_t i;
    size_t out_len;
    size_t written = 0U;

    if((payload == NULL) || (payload_len == 0U))
    {
        return NULL;
    }

    out_len = (payload_len * 3U) + 1U;
    out_text = (char *)malloc(out_len);
    if(out_text == NULL)
    {
        return NULL;
    }

    out_text[0] = '\0';

    for(i = 0U; i < payload_len; i++)
    {
        int n = snprintf(&out_text[written], out_len - written,
                         (i + 1U < payload_len) ? "%02X " : "%02X",
                         payload[i]);
        if((n < 0) || ((size_t)n >= (out_len - written)))
        {
            break;
        }
        written += (size_t)n;
    }

    return out_text;
}

/**
 * @brief Internal helper for `poom_sniffer_device_print_summary`.
 *
 * @param[in] packet Parameter passed to the function.
 * @param[in] pkt_type Parameter passed to the function.
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @return void
 */
static void poom_sniffer_device_print_summary_(const wifi_promiscuous_pkt_t *packet,
                                               wifi_promiscuous_pkt_type_t pkt_type,
                                               const uint8_t *payload,
                                               size_t payload_len)
{
    uint8_t src[6];
    uint8_t dst[6];
    uint32_t now_ms;
    char elapsed[16];
    char fallback_preview[(POOM_SNIFFER_DEVICE_RAW_PREVIEW_MAX * 3U) + 8U];
    char *hex_line;

    if((packet == NULL) || (payload == NULL))
    {
        return;
    }

    poom_sniffer_device_extract_addrs_(payload, payload_len, src, dst);
    now_ms = poom_sniffer_device_now_ms_();
    poom_sniffer_device_make_elapsed_(elapsed, sizeof(elapsed), now_ms);
    hex_line = poom_sniffer_device_make_hex_line_alloc_(payload, payload_len);
    if(hex_line == NULL)
    {
        size_t max_bytes = payload_len;
        if(max_bytes > POOM_SNIFFER_DEVICE_RAW_PREVIEW_MAX)
        {
            max_bytes = POOM_SNIFFER_DEVICE_RAW_PREVIEW_MAX;
        }

        fallback_preview[0] = '\0';
        for(size_t i = 0U, written = 0U; i < max_bytes; i++)
        {
            int n = snprintf(&fallback_preview[written],
                             sizeof(fallback_preview) - written,
                             (i + 1U < max_bytes) ? "%02X " : "%02X",
                             payload[i]);
            if((n < 0) || ((size_t)n >= (sizeof(fallback_preview) - written)))
            {
                break;
            }
            written += (size_t)n;
        }
        if(payload_len > max_bytes)
        {
            const size_t used = strlen(fallback_preview);
            if(used + 4U < sizeof(fallback_preview))
            {
                (void)snprintf(&fallback_preview[used], sizeof(fallback_preview) - used, " ...");
            }
        }
        hex_line = fallback_preview;
    }

    printf("[%s] CH=%u RSSI=%d TYPE=%s SRC=%02X:%02X:%02X:%02X:%02X:%02X DST=%02X:%02X:%02X:%02X:%02X:%02X DATA=%s\n",
           elapsed,
           (unsigned)packet->rx_ctrl.channel,
           packet->rx_ctrl.rssi,
           poom_sniffer_device_summary_type_name_(pkt_type, payload, payload_len),
           src[0], src[1], src[2], src[3], src[4], src[5],
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
           hex_line);

    if(hex_line != fallback_preview)
    {
        free(hex_line);
    }
}

/**
 * @brief Returns true when system clock already has a valid calendar year.
 * @param[in/out] none Not used.
 * @return bool
 */
static bool poom_sniffer_device_is_time_synced_(void)
{
    time_t now;
    struct tm now_tm = {0};

    (void)time(&now);
    if(localtime_r(&now, &now_tm) == NULL)
    {
        return false;
    }

    return (now_tm.tm_year + 1900) >= POOM_SNIFFER_DEVICE_TIME_MIN_YEAR;
}

/**
 * @brief Formats current time string for probe print lines.
 * @param[in/out] out_ts Output timestamp buffer.
 * @param[in] out_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_make_timestamp_(char *out_ts, size_t out_len)
{
    time_t now;
    struct tm now_tm = {0};

    if((out_ts == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!poom_sniffer_device_is_time_synced_())
    {
        (void)snprintf(out_ts, out_len, "%s", "unsynced");
        return ESP_OK;
    }

    (void)time(&now);
    if(localtime_r(&now, &now_tm) == NULL)
    {
        (void)snprintf(out_ts, out_len, "%s", "unsynced");
        return ESP_FAIL;
    }

    if(strftime(out_ts, out_len, "%Y-%m-%d %H:%M:%S", &now_tm) == 0U)
    {
        (void)snprintf(out_ts, out_len, "%s", "unsynced");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Prepares SD folder used to store per-read capture files.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_prepare_capture_storage_(void)
{
    esp_err_t status;

    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        status = sd_card_mount();
        if(status != ESP_OK)
        {
            POOM_SNIFFER_DEVICE_PRINTF_E("sd_card_mount failed: %s", esp_err_to_name(status));
            return status;
        }
    }

    status = sd_card_create_dir(POOM_SNIFFER_DEVICE_CAPTURE_DIR);
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("sd_card_create_dir failed: %s", esp_err_to_name(status));
        return status;
    }

    return ESP_OK;
}

/**
 * @brief Builds unique capture file path for current sniffer session.
 * @param[in/out] out_path Output file path buffer.
 * @param[in] out_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_build_capture_file_path_(char *out_path, size_t out_len)
{
    uint32_t token;
    uint32_t local_session;
    int written;
    esp_err_t status;

    if((out_path == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    local_session = ++s_poom_sniffer_device_session_index;
    token = poom_sniffer_device_is_time_synced_()
                ? ((uint32_t)time(NULL) & 0xFFFFFU)
                : (poom_sniffer_device_now_ms_() & 0xFFFFFU);

    written = snprintf(out_path,
                       out_len,
                       "%s/r%05lX%02lX.txt",
                       POOM_SNIFFER_DEVICE_CAPTURE_DIR,
                       (unsigned long)token,
                       (unsigned long)(local_session & 0xFFU));
    if((written < 0) || ((size_t)written >= out_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    status = sd_card_create_file(out_path);
    if(status == ESP_OK)
    {
        return ESP_OK;
    }

    if(status != ESP_ERR_FILE_EXISTS)
    {
        return status;
    }

    for(local_session = 0U; local_session < 32U; local_session++)
    {
        written = snprintf(out_path,
                           out_len,
                           "%s/r%05lX%02lX.txt",
                           POOM_SNIFFER_DEVICE_CAPTURE_DIR,
                           (unsigned long)token,
                           (unsigned long)((s_poom_sniffer_device_session_index + local_session + 1U) & 0xFFU));
        if((written < 0) || ((size_t)written >= out_len))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        status = sd_card_create_file(out_path);
        if(status == ESP_OK)
        {
            s_poom_sniffer_device_session_index += (local_session + 1U);
            return ESP_OK;
        }
        if(status != ESP_ERR_FILE_EXISTS)
        {
            return status;
        }
    }

    return ESP_FAIL;
}

/**
 * @brief Formats one capture item as a CSV-like line.
 * @param[in] item Capture item.
 * @param[in/out] out_line Output line buffer.
 * @param[in] out_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_format_capture_line_(const poom_sniffer_device_capture_item_t *item,
                                                           char *out_line,
                                                           size_t out_len)
{
    int written;

    if((item == NULL) || (out_line == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_line,
                       out_len,
                       "ADDR=%02X:%02X:%02X:%02X:%02X:%02X,SSID=%s,TIMESTAMP=%s,HASH=%08lX,RSSI=%d,SN=%u,HT_CAP_INFO=0x%04X,CH=%u\n",
                       item->source_mac[0], item->source_mac[1], item->source_mac[2],
                       item->source_mac[3], item->source_mac[4], item->source_mac[5],
                       item->ssid,
                       item->timestamp,
                       (unsigned long)item->hash,
                       item->rssi,
                       (unsigned)item->sequence_number,
                       (unsigned)item->ht_cap_info,
                       (unsigned)item->channel);
    if((written < 0) || ((size_t)written >= out_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/**
 * @brief Builds absolute SD path for a relative capture file path.
 * @param[in] relative_path Capture path relative to SD mount point.
 * @param[out] out_full_path Destination absolute path buffer.
 * @param[in] out_len Destination buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_build_capture_full_path_(const char *relative_path,
                                                              char *out_full_path,
                                                              size_t out_len)
{
    int path_written;

    if((relative_path == NULL) || (out_full_path == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    path_written = snprintf(out_full_path, out_len, "%s%s", SD_CARD_PATH, relative_path);
    if((path_written < 0) || ((size_t)path_written >= out_len))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/**
 * @brief Opens the current capture file and installs a larger stdio buffer.
 * @param[in] relative_path Capture path relative to SD mount point.
 * @param[out] out_capture_file Open FILE handle.
 * @param[out] out_full_path Absolute path for logs.
 * @param[in] out_full_path_len Absolute path buffer length.
 * @param[out] out_bytes_written Current file size in bytes.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_open_capture_file_(const char *relative_path,
                                                        FILE **out_capture_file,
                                                        char *out_full_path,
                                                        size_t out_full_path_len,
                                                        size_t *out_bytes_written)
{
    static char s_capture_file_buffer[POOM_SNIFFER_DEVICE_CAPTURE_FILE_BUFFER_BYTES];
    FILE *capture_file;
    long current_offset;
    esp_err_t status;

    if((relative_path == NULL) || (out_capture_file == NULL) || (out_full_path == NULL) ||
       (out_full_path_len == 0U) || (out_bytes_written == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    status = poom_sniffer_device_build_capture_full_path_(relative_path, out_full_path, out_full_path_len);
    if(status != ESP_OK)
    {
        return status;
    }

    capture_file = fopen(out_full_path, "a");
    if(capture_file == NULL)
    {
        return ESP_FAIL;
    }

    (void)setvbuf(capture_file, s_capture_file_buffer, _IOFBF, sizeof(s_capture_file_buffer));

    if(fseek(capture_file, 0L, SEEK_END) != 0)
    {
        (void)fclose(capture_file);
        return ESP_FAIL;
    }

    current_offset = ftell(capture_file);
    if(current_offset < 0L)
    {
        (void)fclose(capture_file);
        return ESP_FAIL;
    }

    *out_capture_file = capture_file;
    *out_bytes_written = (size_t)current_offset;
    return ESP_OK;
}

/**
 * @brief Writes queued capture items to SD in task context.
 * @param[in/out] task_arg Not used.
 * @return void
 */
static void poom_sniffer_device_capture_task_(void *task_arg)
{
    poom_sniffer_device_capture_item_t item;
    char line[POOM_SNIFFER_DEVICE_CAPTURE_DATA_MAX];
    char current_relative_path[POOM_SNIFFER_DEVICE_CAPTURE_PATH_MAX];
    char full_path[POOM_SNIFFER_DEVICE_CAPTURE_FULL_PATH_MAX];
    FILE *capture_file = NULL;
    BaseType_t got_item;
    size_t bytes_written = 0U;
    uint32_t buffered_lines = 0U;
    bool flush_pending = false;
    esp_err_t status;

    (void)task_arg;

    (void)snprintf(current_relative_path, sizeof(current_relative_path), "%s", s_poom_sniffer_device_capture_file_path);
    status = poom_sniffer_device_open_capture_file_(current_relative_path,
                                                    &capture_file,
                                                    full_path,
                                                    sizeof(full_path),
                                                    &bytes_written);
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("fopen failed for %s", current_relative_path);
        s_poom_sniffer_device_capture_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while(true)
    {
        got_item = xQueueReceive(s_poom_sniffer_device_capture_queue, &item, pdMS_TO_TICKS(200));
        if(got_item == pdTRUE)
        {
            status = poom_sniffer_device_format_capture_line_(&item, line, sizeof(line));
            if(status != ESP_OK)
            {
                POOM_SNIFFER_DEVICE_PRINTF_E("capture format failed: %s", esp_err_to_name(status));
            }
            else
            {
                size_t line_len = strlen(line);
                if(fwrite(line, 1U, line_len, capture_file) != line_len)
                {
                    POOM_SNIFFER_DEVICE_PRINTF_E("capture fwrite failed");
                }
                else
                {
                    bytes_written += line_len;
                    buffered_lines++;
                    flush_pending = true;
                }

                if(flush_pending &&
                   ((buffered_lines >= POOM_SNIFFER_DEVICE_CAPTURE_FLUSH_LINES) ||
                    (bytes_written >= POOM_SNIFFER_DEVICE_CAPTURE_FILE_ROTATE_BYTES)))
                {
                    if(fflush(capture_file) != 0)
                    {
                        POOM_SNIFFER_DEVICE_PRINTF_E("capture fflush failed");
                    }
                    buffered_lines = 0U;
                    flush_pending = false;
                }

                if(bytes_written >= POOM_SNIFFER_DEVICE_CAPTURE_FILE_ROTATE_BYTES)
                {
                    if(fclose(capture_file) != 0)
                    {
                        POOM_SNIFFER_DEVICE_PRINTF_E("capture fclose failed");
                    }

                    status = poom_sniffer_device_build_capture_file_path_(current_relative_path,
                                                                          sizeof(current_relative_path));
                    if(status != ESP_OK)
                    {
                        POOM_SNIFFER_DEVICE_PRINTF_E("capture rotate failed: %s", esp_err_to_name(status));
                        s_poom_sniffer_device_capture_task = NULL;
                        vTaskDelete(NULL);
                        return;
                    }

                    (void)snprintf(s_poom_sniffer_device_capture_file_path,
                                   sizeof(s_poom_sniffer_device_capture_file_path),
                                   "%s",
                                   current_relative_path);

                    status = poom_sniffer_device_open_capture_file_(current_relative_path,
                                                                    &capture_file,
                                                                    full_path,
                                                                    sizeof(full_path),
                                                                    &bytes_written);
                    if(status != ESP_OK)
                    {
                        POOM_SNIFFER_DEVICE_PRINTF_E("capture reopen failed for %s", current_relative_path);
                        s_poom_sniffer_device_capture_task = NULL;
                        vTaskDelete(NULL);
                        return;
                    }

                    buffered_lines = 0U;
                    flush_pending = false;
                    POOM_SNIFFER_DEVICE_PRINTF_I("Capture rotated to %s", current_relative_path);
                }
            }
        }
        else if(flush_pending)
        {
            if(fflush(capture_file) != 0)
            {
                POOM_SNIFFER_DEVICE_PRINTF_E("capture periodic fflush failed");
            }
            buffered_lines = 0U;
            flush_pending = false;
        }

        if((s_poom_sniffer_device_stop_requested) &&
           (s_poom_sniffer_device_capture_queue != NULL) &&
           (uxQueueMessagesWaiting(s_poom_sniffer_device_capture_queue) == 0U))
        {
            break;
        }
    }

    if(flush_pending && (fflush(capture_file) != 0))
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("capture final fflush failed");
    }
    (void)fclose(capture_file);
    s_poom_sniffer_device_capture_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Synchronizes system clock with NTP while STA has connectivity.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_sync_ntp_time_(void)
{
    esp_err_t status;
    uint32_t start_ms;
    char ts[24];

    if(poom_sniffer_device_is_time_synced_())
    {
        (void)poom_sniffer_device_make_timestamp_(ts, sizeof(ts));
        POOM_SNIFFER_DEVICE_PRINTF_I("System time already synced: %s", ts);
        return ESP_OK;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(POOM_SNIFFER_DEVICE_NTP_SERVER);
    status = esp_netif_sntp_init(&config);
    if((status != ESP_OK) && (status != ESP_ERR_INVALID_STATE))
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("esp_netif_sntp_init failed: %s", esp_err_to_name(status));
        return status;
    }

    start_ms = poom_sniffer_device_now_ms_();
    while((poom_sniffer_device_now_ms_() - start_ms) < POOM_SNIFFER_DEVICE_NTP_WAIT_MS)
    {
        if(poom_sniffer_device_is_time_synced_())
        {
            (void)poom_sniffer_device_make_timestamp_(ts, sizeof(ts));
            POOM_SNIFFER_DEVICE_PRINTF_I("NTP synced time: %s", ts);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(POOM_SNIFFER_DEVICE_WIFI_POLL_MS));
    }

    POOM_SNIFFER_DEVICE_PRINTF_E("NTP sync timeout after %u ms", (unsigned)POOM_SNIFFER_DEVICE_NTP_WAIT_MS);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Waits for STA IP with timeout.
 * @param[in] timeout_ms Wait timeout in milliseconds.
 * @return bool
 */
static bool poom_sniffer_device_wait_ip_(uint32_t timeout_ms)
{
    uint32_t start_ms = poom_sniffer_device_now_ms_();

    while((poom_sniffer_device_now_ms_() - start_ms) < timeout_ms)
    {
        if(poom_wifi_ctrl_sta_has_ip())
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(POOM_SNIFFER_DEVICE_WIFI_POLL_MS));
    }

    return poom_wifi_ctrl_sta_has_ip();
}

/**
 * @brief Tries to connect STA using stored secrets with bounded retries.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_try_connect_from_secrets_(void)
{
    char ssid[POOM_SNIFFER_DEVICE_WIFI_SSID_MAX];
    char pass[POOM_SNIFFER_DEVICE_WIFI_PASS_MAX];
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    uint32_t attempt;
    esp_err_t status;
    esp_err_t final_status = ESP_FAIL;

    status = poom_wifi_ctrl_register_cb(NULL, NULL);
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("poom_wifi_ctrl_register_cb failed: %s", esp_err_to_name(status));
        return status;
    }

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("poom_secrets_init failed: %s", esp_err_to_name(status));
        final_status = status;
        goto cleanup;
    }

    status = poom_secrets_get_wifi_ssid(ssid, &ssid_len);
    if((status != ESP_OK) || (ssid_len == 0U) || (ssid[0] == '\0'))
    {
        POOM_SNIFFER_DEVICE_PRINTF_I("No stored WiFi SSID, skip connect");
        final_status = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    status = poom_secrets_get_wifi_pass(pass, &pass_len);
    if(status != ESP_OK)
    {
        pass[0] = '\0';
    }

    for(attempt = 1U; attempt <= POOM_SNIFFER_DEVICE_WIFI_RETRY_MAX; attempt++)
    {
        status = poom_wifi_ctrl_sta_connect(ssid, (pass[0] != '\0') ? pass : NULL);
        POOM_SNIFFER_DEVICE_PRINTF_I("WiFi connect attempt %u/%u: %s",
                                     (unsigned)attempt,
                                     (unsigned)POOM_SNIFFER_DEVICE_WIFI_RETRY_MAX,
                                     esp_err_to_name(status));
        if(status != ESP_OK)
        {
            continue;
        }

        if(poom_sniffer_device_wait_ip_(POOM_SNIFFER_DEVICE_WIFI_WAIT_IP_MS))
        {
            status = poom_sniffer_device_sync_ntp_time_();
            if(status != ESP_OK)
            {
                POOM_SNIFFER_DEVICE_PRINTF_E("NTP sync failed: %s", esp_err_to_name(status));
            }
            POOM_SNIFFER_DEVICE_PRINTF_I("WiFi connected from secrets");
            final_status = ESP_OK;
            goto cleanup;
        }

        (void)poom_wifi_ctrl_sta_disconnect();
    }

    POOM_SNIFFER_DEVICE_PRINTF_I("WiFi connection from secrets failed");
    final_status = ESP_FAIL;

cleanup:
    (void)poom_wifi_ctrl_unregister_cb();
    return final_status;
}

/**
 * @brief Copies SSID bytes to printable string.
 * @param[in/out] out_ssid Output string buffer.
 * @param[in] out_len Output buffer size.
 * @param[in] raw_ssid Raw SSID byte pointer.
 * @param[in] raw_len SSID length in bytes.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_copy_printable_ssid_(char *out_ssid,
                                                           size_t out_len,
                                                           const uint8_t *raw_ssid,
                                                           size_t raw_len)
{
    size_t i;

    if((out_ssid == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if((raw_ssid == NULL) || (raw_len == 0U))
    {
        (void)snprintf(out_ssid, out_len, "%s", "<broadcast>");
        return ESP_OK;
    }

    if(raw_len > 32U)
    {
        raw_len = 32U;
    }

    for(i = 0U; (i < raw_len) && (i < (out_len - 1U)); i++)
    {
        uint8_t c = raw_ssid[i];
        out_ssid[i] = ((c >= 32U) && (c <= 126U)) ? (char)c : '.';
    }
    out_ssid[i] = '\0';
    return ESP_OK;
}

/**
 * @brief Returns true if a probe request should be printed now.
 * @param[in] mac Source MAC address.
 * @param[in] ssid Parsed SSID string for the current probe request.
 * @param[in] now_ms Current system time in milliseconds.
 * @return bool
 */
static bool poom_sniffer_device_should_print_(const uint8_t *mac, const char *ssid, uint32_t now_ms)
{
    size_t i;
    int free_index = -1;
    size_t oldest_index = 0U;
    uint32_t oldest_ms = UINT32_MAX;

    if((mac == NULL) || (ssid == NULL))
    {
        return false;
    }

    for(i = 0U; i < POOM_SNIFFER_DEVICE_SEEN_CAPACITY; i++)
    {
        poom_sniffer_device_seen_entry_t *entry = &s_poom_sniffer_device_seen[i];

        if(entry->used)
        {
            if((memcmp(entry->mac, mac, sizeof(entry->mac)) == 0) &&
               (strncmp(entry->ssid, ssid, sizeof(entry->ssid)) == 0))
            {
                if((now_ms - entry->last_seen_ms) < POOM_SNIFFER_DEVICE_REPRINT_MS)
                {
                    return false;
                }

                entry->last_seen_ms = now_ms;
                return true;
            }

            if(entry->last_seen_ms < oldest_ms)
            {
                oldest_ms = entry->last_seen_ms;
                oldest_index = i;
            }
        }
        else if(free_index < 0)
        {
            free_index = (int)i;
        }
    }

    if(free_index >= 0)
    {
        poom_sniffer_device_seen_entry_t *entry = &s_poom_sniffer_device_seen[free_index];
        entry->used = true;
        memcpy(entry->mac, mac, sizeof(entry->mac));
        (void)snprintf(entry->ssid, sizeof(entry->ssid), "%s", ssid);
        entry->last_seen_ms = now_ms;
        return true;
    }

    memcpy(s_poom_sniffer_device_seen[oldest_index].mac, mac, sizeof(s_poom_sniffer_device_seen[oldest_index].mac));
    (void)snprintf(s_poom_sniffer_device_seen[oldest_index].ssid,
                   sizeof(s_poom_sniffer_device_seen[oldest_index].ssid),
                   "%s",
                   ssid);
    s_poom_sniffer_device_seen[oldest_index].last_seen_ms = now_ms;
    return true;
}

/**
 * @brief Parses SSID from probe request information elements.
 * @param[in] payload 802.11 frame payload.
 * @param[in] payload_len Payload length in bytes.
 * @param[in/out] out_ssid Output string buffer.
 * @param[in] out_len Output buffer size.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_parse_probe_ssid_(const uint8_t *payload,
                                                        size_t payload_len,
                                                        char *out_ssid,
                                                        size_t out_len)
{
    size_t offset = 24U;

    if((payload == NULL) || (out_ssid == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(payload_len <= offset)
    {
        (void)snprintf(out_ssid, out_len, "%s", "<unknown>");
        return ESP_OK;
    }

    while((offset + 1U) < payload_len)
    {
        uint8_t id = payload[offset];
        uint8_t len = payload[offset + 1U];
        size_t value_offset = offset + 2U;
        size_t end = value_offset + len;

        if(end > payload_len)
        {
            break;
        }

        if(id == 0U)
        {
            return poom_sniffer_device_copy_printable_ssid_(out_ssid, out_len, &payload[value_offset], len);
        }

        offset = end;
    }

    (void)snprintf(out_ssid, out_len, "%s", "<unknown>");
    return ESP_OK;
}

/**
 * @brief Parses sequence number and HT capabilities info from probe request.
 * @param[in] payload 802.11 frame payload.
 * @param[in] payload_len Payload length.
 * @param[in/out] out_sn Output sequence number.
 * @param[in/out] out_ht_cap_info Output HT capabilities info.
 * @return esp_err_t
 */
static esp_err_t poom_sniffer_device_parse_probe_meta_(const uint8_t *payload,
                                                        size_t payload_len,
                                                        uint16_t *out_sn,
                                                        uint16_t *out_ht_cap_info)
{
    size_t offset = 24U;
    uint16_t sn = 0U;
    uint16_t ht_cap_info = 0U;

    if((payload == NULL) || (out_sn == NULL) || (out_ht_cap_info == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(payload_len >= 24U)
    {
        sn = (uint16_t)(((uint16_t)payload[23] << 4U) | ((uint16_t)payload[22] >> 4U));
    }

    while((offset + 1U) < payload_len)
    {
        uint8_t id = payload[offset];
        uint8_t len = payload[offset + 1U];
        size_t value_offset = offset + 2U;
        size_t end = value_offset + len;

        if(end > payload_len)
        {
            break;
        }

        if((id == 45U) && (len >= 2U))
        {
            ht_cap_info = (uint16_t)payload[value_offset] |
                          (uint16_t)((uint16_t)payload[value_offset + 1U] << 8U);
            break;
        }

        offset = end;
    }

    *out_sn = sn;
    *out_ht_cap_info = ht_cap_info;
    return ESP_OK;
}

/**
 * @brief Builds a compact stable hash from MAC + SSID.
 * @param[in] mac Source MAC address.
 * @param[in] ssid Parsed SSID string.
 * @return uint32_t
 */
static uint32_t poom_sniffer_device_hash_(const uint8_t *mac, const char *ssid)
{
    uint32_t hash = 2166136261UL;
    size_t i;

    if(mac != NULL)
    {
        for(i = 0U; i < 6U; i++)
        {
            hash ^= mac[i];
            hash *= 16777619UL;
        }
    }

    if(ssid != NULL)
    {
        for(i = 0U; ssid[i] != '\0'; i++)
        {
            hash ^= (uint8_t)ssid[i];
            hash *= 16777619UL;
        }
    }

    return hash;
}

/**
 * @brief Checks if a frame payload corresponds to probe request subtype.
 * @param[in] payload 802.11 frame payload.
 * @param[in] payload_len Payload length.
 * @return bool
 */
static bool poom_sniffer_device_is_probe_request_(const uint8_t *payload, size_t payload_len)
{
    uint8_t type;
    uint8_t subtype;

    if((payload == NULL) || (payload_len < 24U))
    {
        return false;
    }

    type = (uint8_t)((payload[0] >> 2U) & 0x03U);
    subtype = (uint8_t)((payload[0] >> 4U) & 0x0FU);
    return (type == 0U) && (subtype == 4U);
}

/**
 * @brief Receives Wi-Fi packets in promiscuous mode and prints probe requests.
 * @param[in/out] buf Raw packet buffer.
 * @param[in/out] type Packet type identifier.
 * @return void
 */
static void poom_sniffer_device_promiscuous_cb_(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *packet;
    const uint8_t *payload;
    const uint8_t *source_mac;
    size_t payload_len;
    uint32_t now_ms;
    uint16_t sequence_number;
    uint16_t ht_cap_info;
    uint32_t hash;
    char ssid[40];
    char timestamp[24];

    if(!s_poom_sniffer_device_running || (buf == NULL))
    {
        return;
    }

    packet = (const wifi_promiscuous_pkt_t *)buf;
    if(packet->rx_ctrl.sig_len <= 0)
    {
        return;
    }

    payload = packet->payload;
    payload_len = (size_t)packet->rx_ctrl.sig_len;
    if(s_poom_sniffer_device_cfg.print_packet_summary)
    {
        if(!poom_sniffer_device_filter_accepts_(type, payload, payload_len))
        {
            return;
        }

        poom_sniffer_device_print_summary_(packet, type, payload, payload_len);
        return;
    }

    if((type != WIFI_PKT_MGMT) || !poom_sniffer_device_is_probe_request_(payload, payload_len))
    {
        return;
    }

    source_mac = &payload[10];
    (void)poom_sniffer_device_parse_probe_ssid_(payload, payload_len, ssid, sizeof(ssid));
    now_ms = poom_sniffer_device_now_ms_();
    if(s_poom_sniffer_device_cfg.dedupe_enabled &&
       !poom_sniffer_device_should_print_(source_mac, ssid, now_ms))
    {
        return;
    }

    (void)poom_sniffer_device_parse_probe_meta_(payload, payload_len, &sequence_number, &ht_cap_info);
    (void)poom_sniffer_device_make_timestamp_(timestamp, sizeof(timestamp));
    hash = poom_sniffer_device_hash_(source_mac, ssid);
    printf("[I] [poom_sniffer_device] ADDR=%02X:%02X:%02X:%02X:%02X:%02X, SSID=%s, TIMESTAMP=%s, HASH=%08lX, RSSI=%d, SN=%u, HT_CAP_INFO=0x%04X, CH=%u\n",
           source_mac[0], source_mac[1], source_mac[2], source_mac[3], source_mac[4], source_mac[5],
           ssid,
           timestamp,
           (unsigned long)hash,
           packet->rx_ctrl.rssi,
           (unsigned)sequence_number,
           (unsigned)ht_cap_info,
           (unsigned)packet->rx_ctrl.channel);

    if(s_poom_sniffer_device_sd_ready && (s_poom_sniffer_device_capture_queue != NULL))
    {
        poom_sniffer_device_capture_item_t item;
        (void)memset(&item, 0, sizeof(item));

        (void)memcpy(item.source_mac, source_mac, sizeof(item.source_mac));
        (void)snprintf(item.ssid, sizeof(item.ssid), "%s", ssid);
        (void)snprintf(item.timestamp, sizeof(item.timestamp), "%s", timestamp);
        item.hash = hash;
        item.rssi = packet->rx_ctrl.rssi;
        item.sequence_number = sequence_number;
        item.ht_cap_info = ht_cap_info;
        item.channel = packet->rx_ctrl.channel;

        if(xQueueSend(s_poom_sniffer_device_capture_queue, &item, 0) != pdTRUE)
        {
            if(!s_poom_sniffer_device_capture_queue_full_reported)
            {
                POOM_SNIFFER_DEVICE_PRINTF_E("capture queue full: dropping entries");
                s_poom_sniffer_device_capture_queue_full_reported = true;
            }
        }
        else
        {
            s_poom_sniffer_device_capture_queue_full_reported = false;
        }
    }
}

/**
 * @brief Runs periodic channel hopping while sniffer is active.
 * @param[in/out] task_arg Task argument not used.
 * @return void
 */
static void poom_sniffer_device_hop_task_(void *task_arg)
{
    (void)task_arg;

    while(s_poom_sniffer_device_running)
    {
        esp_err_t status;

        vTaskDelay(pdMS_TO_TICKS(POOM_SNIFFER_DEVICE_HOP_MS));

        s_poom_sniffer_device_channel_idx =
            (s_poom_sniffer_device_channel_idx + 1U) % s_poom_sniffer_device_wifi_channels_count;
        s_poom_sniffer_device_channel = s_poom_sniffer_device_wifi_channels[s_poom_sniffer_device_channel_idx];

        status = esp_wifi_set_channel(s_poom_sniffer_device_channel, WIFI_SECOND_CHAN_NONE);
        if(status != ESP_OK)
        {
            POOM_SNIFFER_DEVICE_PRINTF_E("esp_wifi_set_channel failed: %s", esp_err_to_name(status));
        }
    }

    s_poom_sniffer_device_hop_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t poom_sniffer_device_config_default(poom_sniffer_device_config_t *out_cfg)
{
    if(out_cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_cfg = (poom_sniffer_device_config_t){
        .filter = POOM_SNIFFER_DEVICE_FILTER_PROBE,
        .channel = 0U,
        .save_to_sd = true,
        .dedupe_enabled = true,
        .attempt_time_sync = true,
        .print_packet_summary = false,
        .raw_preview_len = POOM_SNIFFER_DEVICE_RAW_PREVIEW_DEFAULT,
    };
    return ESP_OK;
}

esp_err_t poom_sniffer_device_start_ex(const poom_sniffer_device_config_t *cfg)
{
    esp_err_t status;
    wifi_promiscuous_filter_t filter = {0};
    size_t fixed_channel_idx = 0U;

    if(cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if((cfg->channel != 0U) && !poom_sniffer_device_channel_supported_(cfg->channel, &fixed_channel_idx))
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("unsupported channel: %u", (unsigned)cfg->channel);
        return ESP_ERR_INVALID_ARG;
    }

    if(s_poom_sniffer_device_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_poom_sniffer_device_cfg = *cfg;
    if(s_poom_sniffer_device_cfg.raw_preview_len == 0U)
    {
        s_poom_sniffer_device_cfg.raw_preview_len = POOM_SNIFFER_DEVICE_RAW_PREVIEW_DEFAULT;
    }

    filter.filter_mask = poom_sniffer_device_filter_mask_(s_poom_sniffer_device_cfg.filter);

    if(s_poom_sniffer_device_cfg.attempt_time_sync)
    {
        (void)poom_sniffer_device_try_connect_from_secrets_();
    }
    (void)poom_wifi_ctrl_sta_disconnect();

    status = poom_wifi_ctrl_init_null();
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("poom_wifi_ctrl_init_null failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_sniffer_device_channel_idx = (s_poom_sniffer_device_cfg.channel == 0U) ? 0U : fixed_channel_idx;
    s_poom_sniffer_device_channel = s_poom_sniffer_device_wifi_channels[s_poom_sniffer_device_channel_idx];
    s_poom_sniffer_device_session_start_ms = poom_sniffer_device_now_ms_();
    s_poom_sniffer_device_capture_index = 0U;
    (void)memset(s_poom_sniffer_device_seen, 0, sizeof(s_poom_sniffer_device_seen));
    s_poom_sniffer_device_sd_ready = false;
    s_poom_sniffer_device_stop_requested = false;
    s_poom_sniffer_device_capture_queue_full_reported = false;
    s_poom_sniffer_device_capture_queue = NULL;
    s_poom_sniffer_device_capture_task = NULL;
    s_poom_sniffer_device_capture_file_path[0] = '\0';

    if(s_poom_sniffer_device_cfg.save_to_sd)
    {
        status = poom_sniffer_device_prepare_capture_storage_();
        if(status == ESP_OK)
        {
            status = poom_sniffer_device_build_capture_file_path_(s_poom_sniffer_device_capture_file_path,
                                                                   sizeof(s_poom_sniffer_device_capture_file_path));
            if(status != ESP_OK)
            {
                POOM_SNIFFER_DEVICE_PRINTF_E("capture file init failed: %s", esp_err_to_name(status));
            }
            else
            {
                s_poom_sniffer_device_sd_ready = true;
                s_poom_sniffer_device_capture_queue =
                    xQueueCreate(POOM_SNIFFER_DEVICE_CAPTURE_QUEUE_LEN, sizeof(poom_sniffer_device_capture_item_t));
                if(s_poom_sniffer_device_capture_queue == NULL)
                {
                    POOM_SNIFFER_DEVICE_PRINTF_E("capture queue alloc failed");
                    s_poom_sniffer_device_sd_ready = false;
                }
            }
        }
        else
        {
            POOM_SNIFFER_DEVICE_PRINTF_E("SD capture disabled: %s", esp_err_to_name(status));
        }
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5)
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    status = esp_wifi_set_channel(s_poom_sniffer_device_channel, WIFI_SECOND_CHAN_NONE);
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("esp_wifi_set_channel failed: %s", esp_err_to_name(status));
        return status;
    }

    status = esp_wifi_set_promiscuous_filter(&filter);
    if(status != ESP_OK)
    {
        POOM_SNIFFER_DEVICE_PRINTF_E("esp_wifi_set_promiscuous_filter failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_sniffer_device_running = true;

    if(s_poom_sniffer_device_sd_ready && (s_poom_sniffer_device_capture_queue != NULL))
    {
        if(xTaskCreate(poom_sniffer_device_capture_task_,
                       "poom_sniffer_sd",
                       POOM_SNIFFER_DEVICE_CAPTURE_TASK_STACK,
                       NULL,
                       POOM_SNIFFER_DEVICE_CAPTURE_TASK_PRIO,
                       &s_poom_sniffer_device_capture_task) != pdPASS)
        {
            POOM_SNIFFER_DEVICE_PRINTF_E("capture task create failed");
            if(s_poom_sniffer_device_capture_queue != NULL)
            {
                vQueueDelete(s_poom_sniffer_device_capture_queue);
                s_poom_sniffer_device_capture_queue = NULL;
            }
            s_poom_sniffer_device_sd_ready = false;
        }
    }

    status = esp_wifi_set_promiscuous_rx_cb(poom_sniffer_device_promiscuous_cb_);
    if(status != ESP_OK)
    {
        s_poom_sniffer_device_running = false;
        POOM_SNIFFER_DEVICE_PRINTF_E("esp_wifi_set_promiscuous_rx_cb failed: %s", esp_err_to_name(status));
        return status;
    }

    status = esp_wifi_set_promiscuous(true);
    if(status != ESP_OK)
    {
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        s_poom_sniffer_device_running = false;
        POOM_SNIFFER_DEVICE_PRINTF_E("esp_wifi_set_promiscuous(true) failed: %s", esp_err_to_name(status));
        return status;
    }

    if((s_poom_sniffer_device_cfg.channel == 0U) &&
       (xTaskCreate(poom_sniffer_device_hop_task_,
                    "poom_sniffer_hop",
                    3072,
                    NULL,
                    5,
                    &s_poom_sniffer_device_hop_task) != pdPASS))
    {
        (void)esp_wifi_set_promiscuous(false);
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        s_poom_sniffer_device_running = false;
        s_poom_sniffer_device_stop_requested = true;
        if(s_poom_sniffer_device_capture_task != NULL)
        {
            vTaskDelete(s_poom_sniffer_device_capture_task);
            s_poom_sniffer_device_capture_task = NULL;
        }
        if(s_poom_sniffer_device_capture_queue != NULL)
        {
            vQueueDelete(s_poom_sniffer_device_capture_queue);
            s_poom_sniffer_device_capture_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    if(s_poom_sniffer_device_sd_ready)
    {
        POOM_SNIFFER_DEVICE_PRINTF_I("Capturing to %s", s_poom_sniffer_device_capture_file_path);
    }

    POOM_SNIFFER_DEVICE_PRINTF_I("Sniffer started");
    return ESP_OK;
}

esp_err_t poom_sniffer_device_start(void)
{
    poom_sniffer_device_config_t cfg;
    esp_err_t err = poom_sniffer_device_config_default(&cfg);
    if(err != ESP_OK)
    {
        return err;
    }

    return poom_sniffer_device_start_ex(&cfg);
}

esp_err_t poom_sniffer_device_stop(void)
{
    uint32_t wait_ms = 0U;
    poom_sniffer_device_capture_item_t pending_item;

    if(!s_poom_sniffer_device_running)
    {
        return ESP_OK;
    }

    s_poom_sniffer_device_running = false;
    s_poom_sniffer_device_stop_requested = true;

    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);

    if(s_poom_sniffer_device_hop_task != NULL)
    {
        vTaskDelete(s_poom_sniffer_device_hop_task);
        s_poom_sniffer_device_hop_task = NULL;
    }

    if(s_poom_sniffer_device_capture_task != NULL)
    {
        while((s_poom_sniffer_device_capture_task != NULL) &&
              (wait_ms < POOM_SNIFFER_DEVICE_CAPTURE_STOP_WAIT_MS))
        {
            vTaskDelay(pdMS_TO_TICKS(POOM_SNIFFER_DEVICE_CAPTURE_STOP_POLL_MS));
            wait_ms += POOM_SNIFFER_DEVICE_CAPTURE_STOP_POLL_MS;
        }

        if(s_poom_sniffer_device_capture_task != NULL)
        {
            vTaskDelete(s_poom_sniffer_device_capture_task);
            s_poom_sniffer_device_capture_task = NULL;
        }
    }

    while((s_poom_sniffer_device_capture_queue != NULL) &&
          (xQueueReceive(s_poom_sniffer_device_capture_queue, &pending_item, 0) == pdTRUE))
    {
        char line[POOM_SNIFFER_DEVICE_CAPTURE_DATA_MAX];
        if(poom_sniffer_device_format_capture_line_(&pending_item, line, sizeof(line)) == ESP_OK)
        {
            (void)sd_card_append_to_file(s_poom_sniffer_device_capture_file_path, line);
        }
    }

    if(s_poom_sniffer_device_capture_queue != NULL)
    {
        vQueueDelete(s_poom_sniffer_device_capture_queue);
        s_poom_sniffer_device_capture_queue = NULL;
    }

    s_poom_sniffer_device_sd_ready = false;
    s_poom_sniffer_device_capture_file_path[0] = '\0';

    POOM_SNIFFER_DEVICE_PRINTF_I("Sniffer stopped");
    return ESP_OK;
}

esp_err_t poom_sniffer_device_get_running(bool *out_running)
{
    if(out_running == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_running = s_poom_sniffer_device_running;
    return ESP_OK;
}
