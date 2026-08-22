// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_spam.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"

#if POOM_WIFI_SPAM_ENABLE_LOG
    static const char *POOM_WIFI_SPAM_TAG = "poom_wifi_spam";

    #define POOM_WIFI_SPAM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_SPAM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_WIFI_SPAM_DEBUG_LOG_ENABLED
        #define POOM_WIFI_SPAM_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_WIFI_SPAM_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_WIFI_SPAM_PRINTF_E(...) do { } while (0)
    #define POOM_WIFI_SPAM_PRINTF_W(...) do { } while (0)
    #define POOM_WIFI_SPAM_PRINTF_D(...) do { } while (0)
#endif

#define POOM_WIFI_SPAM_SRCADDR_OFFSET      (10U)
#define POOM_WIFI_SPAM_BSSID_OFFSET        (16U)
#define POOM_WIFI_SPAM_SEQNUM_OFFSET       (22U)
#define POOM_WIFI_SPAM_SSID_LEN_OFFSET     (37U)
#define POOM_WIFI_SPAM_SSID_VALUE_OFFSET   (38U)

#define POOM_WIFI_SPAM_TASK_STACK          (4096U)
#define POOM_WIFI_SPAM_TASK_PRIO           (5U)
#define POOM_WIFI_SPAM_TASK_INTERVAL_MS    (100U)
#define POOM_WIFI_SPAM_MAX_BEACON_LEN      (256U)
#define POOM_WIFI_SPAM_MAX_SSID_LEN        (POOM_WIFI_SPAM_SSID_MAX_LEN)

#define POOM_WIFI_SPAM_SSIDS_KEY "wifi_spam_ssids"
#define POOM_WIFI_SPAM_SSIDS_MAGIC (0x57535331UL) /* WSS1 */
#define POOM_WIFI_SPAM_SSIDS_VERSION (1U)

static const uint8_t s_poom_wifi_spam_beacon_template[] = {
    0x80, 0x00,
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xba, 0xde, 0xaf, 0xfe, 0x00, 0x06,
    0xba, 0xde, 0xaf, 0xfe, 0x00, 0x06,
    0x00, 0x00,
    0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07,
    0x64, 0x00,
    0x31, 0x04,
    0x00, 0x00,
    0x01, 0x08,
    0x82, 0x84, 0x8b, 0x96,
    0x0c, 0x12, 0x18, 0x24,
    0x03, 0x01, 0x01,
    0x05, 0x04, 0x01, 0x02, 0x00, 0x00,
    0x32, 0x04,
    0x30, 0x48, 0x60, 0x6c,
    0x07, 0x06,
    'U', 'S', ' ',
    0x01, 0x0b, 0x14
};

static const char *const s_poom_wifi_spam_default_ssids[] = {
    "Starbucks_Free_WiFi",
    "Coffee_Shop_Guest",
    "Airport_Public_HighSpeed",
    "Hotel_Lobby_FreeWiFi",
    "McDonalds_Free_Internet",
    "Free_Public_WiFi",
    "Guest_Access_Point",
    "Coffee_Break_Network",
    "Open_WiFi_Connect",
    "Free_City_WiFi_Portal",
    "Welcome_Guest_Access",
    "Digital_Nomad_Hotspot",
    "Premium_Coffee_Link",
    "Downtown_Guest_WiFi",
    "Fast_Internet_No_Pass",
    "Wireless_Internet_Guest"
};

static TaskHandle_t s_poom_wifi_spam_task_handle = NULL;
static bool s_poom_wifi_spam_running = false;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    char ssids[POOM_WIFI_SPAM_SSIDS_MAX][POOM_WIFI_SPAM_SSID_MAX_LEN + 1U];
} poom_wifi_spam_ssids_blob_v1_t;

static bool s_poom_wifi_spam_ssids_loaded = false;
static poom_wifi_spam_ssid_list_t s_poom_wifi_spam_ssids;

/**
 * @brief Internal helper for `poom_wifi_spam_ssids_set_defaults`.
 *
 * @return void
 */
static void poom_wifi_spam_ssids_set_defaults_(void)
{
    (void)memset(&s_poom_wifi_spam_ssids, 0, sizeof(s_poom_wifi_spam_ssids));

    const size_t def_cnt =
        sizeof(s_poom_wifi_spam_default_ssids) / sizeof(s_poom_wifi_spam_default_ssids[0]);
    const size_t n = (def_cnt > (size_t)POOM_WIFI_SPAM_SSIDS_MAX) ? (size_t)POOM_WIFI_SPAM_SSIDS_MAX : def_cnt;

    s_poom_wifi_spam_ssids.count = (uint8_t)n;
    for(size_t i = 0; i < n; i++)
    {
        (void)snprintf(s_poom_wifi_spam_ssids.ssids[i],
                       sizeof(s_poom_wifi_spam_ssids.ssids[i]),
                       "%s",
                       (s_poom_wifi_spam_default_ssids[i] != NULL) ? s_poom_wifi_spam_default_ssids[i] : "");
    }
}

/**
 * @brief Saves internal data used by this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_ssids_save_(void)
{
    poom_wifi_spam_ssids_blob_v1_t blob;
    (void)memset(&blob, 0, sizeof(blob));
    blob.magic = POOM_WIFI_SPAM_SSIDS_MAGIC;
    blob.version = POOM_WIFI_SPAM_SSIDS_VERSION;
    blob.count = s_poom_wifi_spam_ssids.count;
    (void)memcpy(blob.ssids, s_poom_wifi_spam_ssids.ssids, sizeof(blob.ssids));

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }

    return poom_secrets_set_blob(POOM_WIFI_SPAM_SSIDS_KEY, &blob, sizeof(blob));
}

/**
 * @brief Loads internal data used by this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_ssids_load_(void)
{
    poom_wifi_spam_ssids_blob_v1_t blob;
    size_t blob_len = sizeof(blob);

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return err;
    }

    (void)memset(&blob, 0, sizeof(blob));
    err = poom_secrets_get_blob(POOM_WIFI_SPAM_SSIDS_KEY, &blob, &blob_len);
    if(err == ESP_ERR_NVS_NOT_FOUND)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return ESP_OK;
    }
    if(err != ESP_OK)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return err;
    }

    if((blob_len != sizeof(blob)) || (blob.magic != POOM_WIFI_SPAM_SSIDS_MAGIC) ||
       (blob.version != POOM_WIFI_SPAM_SSIDS_VERSION))
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return ESP_OK;
    }

    (void)memset(&s_poom_wifi_spam_ssids, 0, sizeof(s_poom_wifi_spam_ssids));
    s_poom_wifi_spam_ssids.count = blob.count;
    if(s_poom_wifi_spam_ssids.count > POOM_WIFI_SPAM_SSIDS_MAX)
    {
        s_poom_wifi_spam_ssids.count = POOM_WIFI_SPAM_SSIDS_MAX;
    }
    (void)memcpy(s_poom_wifi_spam_ssids.ssids, blob.ssids, sizeof(blob.ssids));

    for(uint8_t i = 0; i < s_poom_wifi_spam_ssids.count; i++)
    {
        s_poom_wifi_spam_ssids.ssids[i][POOM_WIFI_SPAM_SSID_MAX_LEN] = '\0';
    }

    s_poom_wifi_spam_ssids_loaded = true;
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_spam_ssids_ensure_loaded`.
 *
 * @return void
 */
static void poom_wifi_spam_ssids_ensure_loaded_(void)
{
    if(s_poom_wifi_spam_ssids_loaded)
    {
        return;
    }
    (void)poom_wifi_spam_ssids_load_();
}

/**
 * @brief Writes the 12-bit sequence number in beacon header format.
 * @param[in/out] header Beacon frame header buffer.
 * @param[in] sequence Sequence number to write.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_set_seqnum_(uint8_t *header, uint16_t sequence)
{
    if(header == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    sequence &= 0x0fffU;
    header[POOM_WIFI_SPAM_SEQNUM_OFFSET] = (uint8_t)((sequence & 0x000fU) << 4U);
    header[POOM_WIFI_SPAM_SEQNUM_OFFSET + 1U] = (uint8_t)((sequence & 0x0ff0U) >> 4U);
    return ESP_OK;
}

/**
 * @brief Builds one beacon frame using a dynamic SSID and virtual MAC suffix.
 * @param[in] ssid SSID string to inject into the beacon.
 * @param[in] line_index SSID list index used as MAC suffix.
 * @param[in] sequence Sequence number for this beacon.
 * @param[in/out] out_buffer Output frame buffer.
 * @param[in/out] out_len Output frame length.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_build_beacon_(const char *ssid,
                                              uint8_t line_index,
                                              uint16_t sequence,
                                              uint8_t *out_buffer,
                                              size_t *out_len)
{
    size_t ssid_len;
    size_t tail_len;
    size_t packet_len;
    esp_err_t status;

    if((ssid == NULL) || (out_buffer == NULL) || (out_len == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    if(ssid_len > POOM_WIFI_SPAM_MAX_SSID_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_buffer, s_poom_wifi_spam_beacon_template, POOM_WIFI_SPAM_SSID_LEN_OFFSET);
    out_buffer[POOM_WIFI_SPAM_SSID_LEN_OFFSET] = (uint8_t)ssid_len;
    memcpy(&out_buffer[POOM_WIFI_SPAM_SSID_VALUE_OFFSET], ssid, ssid_len);

    tail_len = sizeof(s_poom_wifi_spam_beacon_template) - POOM_WIFI_SPAM_SSID_VALUE_OFFSET;
    memcpy(&out_buffer[POOM_WIFI_SPAM_SSID_VALUE_OFFSET + ssid_len],
           &s_poom_wifi_spam_beacon_template[POOM_WIFI_SPAM_SSID_VALUE_OFFSET],
           tail_len);

    packet_len = POOM_WIFI_SPAM_SSID_VALUE_OFFSET + ssid_len + tail_len;
    if(packet_len > POOM_WIFI_SPAM_MAX_BEACON_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    out_buffer[POOM_WIFI_SPAM_SRCADDR_OFFSET + 5U] = line_index;
    out_buffer[POOM_WIFI_SPAM_BSSID_OFFSET + 5U] = line_index;

    status = poom_wifi_spam_set_seqnum_(out_buffer, sequence);
    if(status != ESP_OK)
    {
        return status;
    }

    *out_len = packet_len;
    return ESP_OK;
}

/**
 * @brief Starts AP mode required to transmit raw spoofed beacon frames.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_prepare_ap_(void)
{
    esp_err_t status;
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "wifi-spam",
            .ssid_len = 0,
            .password = "dummypassword",
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 1,
            .max_connection = 4,
            .beacon_interval = 60000
        }
    };

    status = poom_wifi_ctrl_ap_start(&ap_config);
    if(status != ESP_OK)
    {
        POOM_WIFI_SPAM_PRINTF_E("poom_wifi_ctrl_ap_start failed: %s", esp_err_to_name(status));
        return status;
    }

    status = esp_wifi_set_ps(WIFI_PS_NONE);
    if(status != ESP_OK)
    {
        POOM_WIFI_SPAM_PRINTF_W("esp_wifi_set_ps failed: %s", esp_err_to_name(status));
    }

    return ESP_OK;
}

/**
 * @brief FreeRTOS task that continuously transmits spoofed beacons.
 * @param[in/out] task_arg Task argument (unused).
 * @return esp_err_t
 */
static void poom_wifi_spam_task_(void *task_arg)
{
    uint8_t line_index = 0U;
    uint16_t sequence_numbers[POOM_WIFI_SPAM_SSIDS_MAX] = {0};
    uint8_t beacon_buffer[POOM_WIFI_SPAM_MAX_BEACON_LEN];
    const uint32_t total_lines = (uint32_t)s_poom_wifi_spam_ssids.count;
    uint32_t per_line_ms = POOM_WIFI_SPAM_TASK_INTERVAL_MS / total_lines;

    (void)task_arg;

    if(per_line_ms == 0U)
    {
        per_line_ms = 1U;
    }

    while(s_poom_wifi_spam_running)
    {
        const char *ssid = s_poom_wifi_spam_ssids.ssids[line_index];
        size_t packet_len = 0U;
        esp_err_t status;

        vTaskDelay(pdMS_TO_TICKS(per_line_ms));

        status = poom_wifi_spam_build_beacon_(ssid,
                                              line_index,
                                              sequence_numbers[line_index]++,
                                              beacon_buffer,
                                              &packet_len);
        if(status != ESP_OK)
        {
            POOM_WIFI_SPAM_PRINTF_W("Beacon build failed for line %u", (unsigned)line_index);
        }
        else
        {
            status = esp_wifi_80211_tx(WIFI_IF_AP, beacon_buffer, packet_len, false);
            if(status != ESP_OK)
            {
                POOM_WIFI_SPAM_PRINTF_E("esp_wifi_80211_tx failed: %s", esp_err_to_name(status));
            }
        }

        line_index++;
        if(line_index >= total_lines)
        {
            line_index = 0U;
        }
    }

    s_poom_wifi_spam_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Starts the Wi-Fi spam module.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_start(void)
{
    esp_err_t status;

    if(s_poom_wifi_spam_running)
    {
        return ESP_OK;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    if(s_poom_wifi_spam_ssids.count == 0U)
    {
        POOM_WIFI_SPAM_PRINTF_E("SSID list is empty (configure via CLI)");
        return ESP_ERR_INVALID_STATE;
    }

    status = poom_wifi_spam_prepare_ap_();
    if(status != ESP_OK)
    {
        return status;
    }

    s_poom_wifi_spam_running = true;
    if(xTaskCreate(poom_wifi_spam_task_,
                   "poom_wifi_spam",
                   POOM_WIFI_SPAM_TASK_STACK,
                   NULL,
                   POOM_WIFI_SPAM_TASK_PRIO,
                   &s_poom_wifi_spam_task_handle) != pdPASS)
    {
        s_poom_wifi_spam_running = false;
        (void)poom_wifi_ctrl_ap_stop();
        POOM_WIFI_SPAM_PRINTF_E("xTaskCreate failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Stops the Wi-Fi spam module.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_stop(void)
{
    esp_err_t status = ESP_OK;

    if(!s_poom_wifi_spam_running)
    {
        return ESP_OK;
    }

    s_poom_wifi_spam_running = false;

    if(s_poom_wifi_spam_task_handle != NULL)
    {
        vTaskDelete(s_poom_wifi_spam_task_handle);
        s_poom_wifi_spam_task_handle = NULL;
    }

    status = poom_wifi_ctrl_ap_stop();
    if(status != ESP_OK)
    {
        POOM_WIFI_SPAM_PRINTF_W("poom_wifi_ctrl_ap_stop failed: %s", esp_err_to_name(status));
    }

    return status;
}

/**
 * @brief Reads the running state of the Wi-Fi spam module.
 * @param[in/out] out_running Output running flag pointer.
 * @return esp_err_t
 */
esp_err_t poom_wifi_spam_get_running(bool *out_running)
{
    if(out_running == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_running = s_poom_wifi_spam_running;
    return ESP_OK;
}

esp_err_t poom_wifi_spam_ssids_get(poom_wifi_spam_ssid_list_t* out_list)
{
    if(out_list == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    (void)memcpy(out_list, &s_poom_wifi_spam_ssids, sizeof(*out_list));
    return ESP_OK;
}

esp_err_t poom_wifi_spam_ssids_set(const poom_wifi_spam_ssid_list_t* list)
{
    if(list == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if(s_poom_wifi_spam_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_ensure_loaded_();

    if(list->count > POOM_WIFI_SPAM_SSIDS_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    poom_wifi_spam_ssid_list_t tmp;
    (void)memset(&tmp, 0, sizeof(tmp));
    tmp.count = list->count;

    for(uint8_t i = 0; i < tmp.count; i++)
    {
        const char* s = list->ssids[i];
        if((s == NULL) || (s[0] == '\0'))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if(strlen(s) > POOM_WIFI_SPAM_SSID_MAX_LEN)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        (void)snprintf(tmp.ssids[i], sizeof(tmp.ssids[i]), "%s", s);
    }

    s_poom_wifi_spam_ssids = tmp;
    s_poom_wifi_spam_ssids_loaded = true;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_remove(uint8_t index)
{
    if(s_poom_wifi_spam_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    if(index >= s_poom_wifi_spam_ssids.count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for(uint8_t i = index; (i + 1U) < s_poom_wifi_spam_ssids.count; i++)
    {
        (void)memcpy(s_poom_wifi_spam_ssids.ssids[i],
                     s_poom_wifi_spam_ssids.ssids[i + 1U],
                     sizeof(s_poom_wifi_spam_ssids.ssids[i]));
    }
    (void)memset(s_poom_wifi_spam_ssids.ssids[s_poom_wifi_spam_ssids.count - 1U], 0,
                 sizeof(s_poom_wifi_spam_ssids.ssids[0]));
    s_poom_wifi_spam_ssids.count--;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_add(const char* ssid)
{
    if(s_poom_wifi_spam_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    if(s_poom_wifi_spam_ssids.count >= POOM_WIFI_SPAM_SSIDS_MAX)
    {
        return ESP_ERR_NO_MEM;
    }
    if(strlen(ssid) > POOM_WIFI_SPAM_SSID_MAX_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)snprintf(s_poom_wifi_spam_ssids.ssids[s_poom_wifi_spam_ssids.count],
                   sizeof(s_poom_wifi_spam_ssids.ssids[0]),
                   "%s",
                   ssid);
    s_poom_wifi_spam_ssids.count++;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_reset_defaults(void)
{
    if(s_poom_wifi_spam_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_set_defaults_();
    s_poom_wifi_spam_ssids_loaded = true;

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }
    (void)poom_secrets_erase_key(POOM_WIFI_SPAM_SSIDS_KEY);
    return ESP_OK;
}
