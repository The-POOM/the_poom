// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_scan.h"

#include <stdbool.h>
#include <stdio.h>

#include "poom_ble_gatt_client.h"
#include "poom_uart_sniffer.h"

/**
 * @file poom_ble_scan.c
 * @brief Generic BLE advertisement scanner helper implementation.
 */

#if defined(CONFIG_POOM_BLE_SCAN_ENABLE_LOG) && (CONFIG_POOM_BLE_SCAN_ENABLE_LOG == 1)

    static const char *POOM_BLE_SCAN_TAG = "poom_ble_scan";

    #define POOM_BLE_SCAN_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_SCAN_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_SCAN_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_BLE_SCAN_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_SCAN_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_SCAN_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_BLE_SCAN_PRINTF_E(...)
    #define POOM_BLE_SCAN_PRINTF_W(...)
    #define POOM_BLE_SCAN_PRINTF_I(...)

#endif

static poom_ble_scan_result_cb_t s_poom_ble_scan_cb = NULL;
static bool s_poom_ble_scan_active = false;
static bool s_poom_ble_scan_uart_forward_enabled = false;
static esp_ble_scan_filter_t s_poom_ble_scan_filter_type = BLE_SCAN_FILTER_ALLOW_ALL;
static esp_ble_scan_type_t s_poom_ble_scan_type = BLE_SCAN_TYPE_ACTIVE;

/**
 * @brief Handles BLE GAP events and forwards advertising reports.
 *
 * @param[in] event_type BLE GAP event type.
 * @param[in] param Event payload.
 * @return void
 */
static void poom_ble_scan_gap_event_handler_(esp_gap_ble_cb_event_t event_type,
                                             esp_ble_gap_cb_param_t *param)
{
    if((event_type != ESP_GAP_BLE_SCAN_RESULT_EVT) || (param == NULL) || !s_poom_ble_scan_active)
    {
        return;
    }

    switch(param->scan_rst.search_evt)
    {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            if(s_poom_ble_scan_uart_forward_enabled)
            {
                poom_uart_sniffer_send_packet_ble(poom_uart_sniffer_packet_type_ble, param);
            }

            if(s_poom_ble_scan_cb != NULL)
            {
                s_poom_ble_scan_cb(param);
            }

            POOM_BLE_SCAN_PRINTF_I("ADV found RSSI=%d type=%u",
                                   param->scan_rst.rssi,
                                   (unsigned)param->scan_rst.ble_evt_type);
            break;

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            POOM_BLE_SCAN_PRINTF_I("BLE scan window complete");
            break;

        default:
            break;
    }
}

/**
 * @brief Applies current scan configuration to BLE GATT client helper.
 *
 * @return void
 */
static void poom_ble_scan_apply_config_(void)
{
    poom_ble_gatt_client_scan_params_t scan_params;
    poom_ble_gatt_client_event_cb_t event_cb = {0};

    scan_params.remote_filter_service_uuid = poom_ble_gatt_client_default_service_uuid();
    scan_params.remote_filter_char_uuid = poom_ble_gatt_client_default_char_uuid();
    scan_params.notify_descr_uuid = poom_ble_gatt_client_default_notify_descr_uuid();
    scan_params.ble_scan_params = poom_ble_gatt_client_default_scan_params();
    scan_params.ble_scan_params.scan_filter_policy = s_poom_ble_scan_filter_type;
    scan_params.ble_scan_params.scan_type = s_poom_ble_scan_type;

    poom_ble_gatt_client_set_remote_device_name(NULL);
    poom_ble_gatt_client_set_scan_params(&scan_params);

    event_cb.handler_client_cb = NULL;
    event_cb.handler_gap_cb = poom_ble_scan_gap_event_handler_;
    poom_ble_gatt_client_set_callbacks(event_cb);
}

void poom_ble_scan_register_cb(poom_ble_scan_result_cb_t callback)
{
    s_poom_ble_scan_cb = callback;
}

void poom_ble_scan_set_filter_type(esp_ble_scan_filter_t filter_type)
{
    s_poom_ble_scan_filter_type = filter_type;
}

void poom_ble_scan_set_scan_type(esp_ble_scan_type_t scan_type)
{
    s_poom_ble_scan_type = scan_type;
}

void poom_ble_scan_set_uart_forward_enabled(bool enabled)
{
    s_poom_ble_scan_uart_forward_enabled = enabled;
}

esp_err_t poom_ble_scan_start(void)
{
    esp_err_t ret;

    if(s_poom_ble_scan_active)
    {
        POOM_BLE_SCAN_PRINTF_W("BLE scanner already active");
        return ESP_OK;
    }

    poom_ble_scan_apply_config_();
    ret = poom_ble_gatt_client_start();
    if(ret != ESP_OK)
    {
        POOM_BLE_SCAN_PRINTF_E("BLE scanner start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_poom_ble_scan_active = true;

    POOM_BLE_SCAN_PRINTF_I("BLE scanner started");
    return ESP_OK;
}

esp_err_t poom_ble_scan_stop(void)
{
    esp_err_t ret;
    poom_ble_gatt_client_event_cb_t empty_cb = {0};

    if(!s_poom_ble_scan_active)
    {
        return ESP_OK;
    }

    s_poom_ble_scan_active = false;
    s_poom_ble_scan_uart_forward_enabled = false;
    poom_ble_gatt_client_set_callbacks(empty_cb);
    ret = poom_ble_gatt_client_stop();

    POOM_BLE_SCAN_PRINTF_I("BLE scanner stopped");
    return ret;
}

bool poom_ble_scan_is_active(void)
{
    return s_poom_ble_scan_active;
}
