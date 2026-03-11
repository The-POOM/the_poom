// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_scanner.h"

#include <stdio.h>
#include <string.h>

#include "poom_wifi_ctrl.h"

#if POOM_WIFI_SCANNER_ENABLE_LOG
    static const char *POOM_WIFI_SCANNER_TAG = "poom_wifi_scanner";

    #define POOM_WIFI_SCANNER_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_SCANNER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_SCANNER_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_SCANNER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_SCANNER_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_SCANNER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_WIFI_SCANNER_DEBUG_LOG_ENABLED
        #define POOM_WIFI_SCANNER_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_SCANNER_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_WIFI_SCANNER_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_WIFI_SCANNER_PRINTF_E(...) do { } while (0)
    #define POOM_WIFI_SCANNER_PRINTF_W(...) do { } while (0)
    #define POOM_WIFI_SCANNER_PRINTF_I(...) do { } while (0)
    #define POOM_WIFI_SCANNER_PRINTF_D(...) do { } while (0)
#endif

static poom_wifi_scanner_ap_records_t s_poom_wifi_scanner_records = {0};

/**
 * @brief Runs a synchronous Wi-Fi scan and stores AP records.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_scanner_scan(void)
{
    esp_err_t status;

    status = poom_wifi_ctrl_init_sta();
    if(status != ESP_OK)
    {
        POOM_WIFI_SCANNER_PRINTF_E("poom_wifi_ctrl_init_sta failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_wifi_scanner_records.count = POOM_WIFI_SCANNER_MAX_AP;

    status = esp_wifi_clear_ap_list();
    if((status != ESP_OK) && (status != ESP_ERR_WIFI_NOT_INIT))
    {
        POOM_WIFI_SCANNER_PRINTF_W("esp_wifi_clear_ap_list failed: %s", esp_err_to_name(status));
    }

    status = esp_wifi_scan_start(NULL, true);
    if(status != ESP_OK)
    {
        POOM_WIFI_SCANNER_PRINTF_E("esp_wifi_scan_start failed: %s", esp_err_to_name(status));
        return status;
    }

    status = esp_wifi_scan_get_ap_records(&s_poom_wifi_scanner_records.count,
                                          s_poom_wifi_scanner_records.records);
    if(status != ESP_OK)
    {
        POOM_WIFI_SCANNER_PRINTF_E("esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(status));
        return status;
    }

    POOM_WIFI_SCANNER_PRINTF_I("Found %u APs", s_poom_wifi_scanner_records.count);
    POOM_WIFI_SCANNER_PRINTF_D("Scan completed");
    return ESP_OK;
}

/**
 * @brief Gets the latest AP record list.
 * @param[in/out] none Not used.
 * @return poom_wifi_scanner_ap_records_t*
 */
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void)
{
    return &s_poom_wifi_scanner_records;
}

/**
 * @brief Gets one AP record by index.
 * @param[in] index AP list index.
 * @return wifi_ap_record_t*
 */
wifi_ap_record_t *poom_wifi_scanner_get_ap_record(unsigned index)
{
    if(index >= s_poom_wifi_scanner_records.count)
    {
        POOM_WIFI_SCANNER_PRINTF_E("Index out of bounds: count=%u index=%u",
                                   s_poom_wifi_scanner_records.count,
                                   index);
        return NULL;
    }

    return &s_poom_wifi_scanner_records.records[index];
}

/**
 * @brief Clears cached AP records and driver AP list.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_scanner_clear_ap_records(void)
{
    esp_err_t status;

    (void)memset(&s_poom_wifi_scanner_records, 0, sizeof(s_poom_wifi_scanner_records));

    status = esp_wifi_clear_ap_list();
    if((status == ESP_ERR_WIFI_NOT_INIT) || (status == ESP_ERR_WIFI_NOT_STARTED))
    {
        return ESP_OK;
    }

    return status;
}
