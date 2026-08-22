// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_mac.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *POOM_WIFI_MAC_TAG = "poom_wifi_mac";

#ifndef POOM_WIFI_MAC_ENABLE_LOG
#define POOM_WIFI_MAC_ENABLE_LOG (1)
#endif

#define POOM_WIFI_MAC_TASK_STACK_WORDS (4096U)
#define POOM_WIFI_MAC_TASK_PRIORITY (5U)
#define POOM_WIFI_MAC_STOP_WAIT_MS (20U)
#define POOM_WIFI_MAC_LEN (6U)
#define POOM_WIFI_MAC_SSID_MAX_LEN (32U)
#define POOM_WIFI_MAC_PASSWORD_MAX_LEN (64U)
#define POOM_WIFI_MAC_NOTIFY_GOT_IP (1UL << 0)

#if POOM_WIFI_MAC_ENABLE_LOG
    #define POOM_WIFI_MAC_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_MAC_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_MAC_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_MAC_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_MAC_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_MAC_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
    #define POOM_WIFI_MAC_PRINTF_E(...) do { (void)POOM_WIFI_MAC_TAG; } while (0)
    #define POOM_WIFI_MAC_PRINTF_W(...) do { (void)POOM_WIFI_MAC_TAG; } while (0)
    #define POOM_WIFI_MAC_PRINTF_I(...) do { (void)POOM_WIFI_MAC_TAG; } while (0)
#endif

typedef struct
{
    bool original_sta_mac_saved;
    bool task_running;
    uint8_t original_sta_mac[POOM_WIFI_MAC_LEN];
    char ssid[POOM_WIFI_MAC_SSID_MAX_LEN + 1U];
    char password[POOM_WIFI_MAC_PASSWORD_MAX_LEN + 1U];
    uint32_t cycle_delay_ms;
    TaskHandle_t task_handle;
    poom_wifi_mac_ip_cb_t on_ip;
    void *user_ctx;
} poom_wifi_mac_state_t;

static poom_wifi_mac_state_t s_poom_wifi_mac = {0};

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] dst Parameter passed to the function.
 * @param[in] dst_len Parameter passed to the function.
 * @param[in] src Parameter passed to the function.
 * @return void
 */
static void poom_wifi_mac_copy_str_(char *dst, size_t dst_len, const char *src)
{
    size_t index;

    if((dst == NULL) || (dst_len == 0U))
    {
        return;
    }

    memset(dst, 0, dst_len);

    if(src == NULL)
    {
        return;
    }

    for(index = 0U; (index < (dst_len - 1U)) && (src[index] != '\0'); index++)
    {
        dst[index] = src[index];
    }
}

/**
 * @brief Internal helper for `poom_wifi_mac_generate_random`.
 *
 * @param[in] out_mac Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_mac_generate_random_(uint8_t out_mac[POOM_WIFI_MAC_LEN])
{
    size_t index;

    if(out_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for(index = 0U; index < POOM_WIFI_MAC_LEN; index++)
    {
        out_mac[index] = (uint8_t)(esp_random() & 0xFFU);
    }

    out_mac[0] &= 0xFEU;
    out_mac[0] |= 0x02U;

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_mac_cache_original_mac`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_mac_cache_original_mac_(void)
{
    esp_err_t status;

    if(s_poom_wifi_mac.original_sta_mac_saved)
    {
        return ESP_OK;
    }

    status = poom_wifi_ctrl_get_sta_mac(s_poom_wifi_mac.original_sta_mac);
    if(status != ESP_OK)
    {
        POOM_WIFI_MAC_PRINTF_E("poom_wifi_ctrl_get_sta_mac failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_wifi_mac.original_sta_mac_saved = true;
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_mac_restore_sta`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_mac_restore_sta_(void)
{
    if(!s_poom_wifi_mac.original_sta_mac_saved)
    {
        return ESP_OK;
    }

    return esp_wifi_set_mac(WIFI_IF_STA, s_poom_wifi_mac.original_sta_mac);
}

/**
 * @brief Internal helper for `poom_wifi_mac_prepare_random_sta`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_mac_prepare_random_sta_(void)
{
    esp_err_t status;
    uint8_t random_mac[POOM_WIFI_MAC_LEN];

    status = poom_wifi_ctrl_init_sta();
    if(status != ESP_OK)
    {
        return status;
    }

    status = poom_wifi_mac_cache_original_mac_();
    if(status != ESP_OK)
    {
        return status;
    }

    status = poom_wifi_mac_generate_random_(random_mac);
    if(status != ESP_OK)
    {
        return status;
    }

    status = esp_wifi_set_mac(WIFI_IF_STA, random_mac);
    if(status != ESP_OK)
    {
        POOM_WIFI_MAC_PRINTF_E("esp_wifi_set_mac failed: %s", esp_err_to_name(status));
        return status;
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_mac_connect_router`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_mac_connect_router_(void)
{
    return poom_wifi_ctrl_sta_connect(s_poom_wifi_mac.ssid,
                                      (s_poom_wifi_mac.password[0] != '\0') ? s_poom_wifi_mac.password : NULL);
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] info Parameter passed to the function.
 * @param[in] user_ctx Parameter passed to the function.
 * @return void
 */
static void poom_wifi_mac_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if((info == NULL) || (!s_poom_wifi_mac.task_running))
    {
        return;
    }

    if(info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)
    {
        if(s_poom_wifi_mac.on_ip != NULL)
        {
            s_poom_wifi_mac.on_ip(info, s_poom_wifi_mac.user_ctx);
        }

        if(s_poom_wifi_mac.task_handle != NULL)
        {
            xTaskNotify(s_poom_wifi_mac.task_handle, POOM_WIFI_MAC_NOTIFY_GOT_IP, eSetBits);
        }
    }
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_wifi_mac_rotation_task_(void *arg)
{
    uint32_t notify_value;

    (void)arg;

    while(s_poom_wifi_mac.task_running)
    {
        if(xTaskNotifyWait(0U, UINT32_MAX, &notify_value, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if((notify_value & POOM_WIFI_MAC_NOTIFY_GOT_IP) == 0U)
        {
            continue;
        }

        if(!s_poom_wifi_mac.task_running)
        {
            break;
        }

        if(s_poom_wifi_mac.cycle_delay_ms > 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(s_poom_wifi_mac.cycle_delay_ms));
        }

        (void)poom_wifi_ctrl_sta_disconnect();
        (void)poom_wifi_ctrl_deinit();

        if(!s_poom_wifi_mac.task_running)
        {
            break;
        }

        if(poom_wifi_mac_prepare_random_sta_() != ESP_OK)
        {
            POOM_WIFI_MAC_PRINTF_W("Failed to prepare next random MAC");
            continue;
        }

        if(poom_wifi_mac_connect_router_() != ESP_OK)
        {
            POOM_WIFI_MAC_PRINTF_W("Failed to reconnect after MAC rotation");
        }
    }

    s_poom_wifi_mac.task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t poom_wifi_mac_start(const char *ssid,
                              const char *password,
                              uint32_t cycle_delay_ms,
                              poom_wifi_mac_ip_cb_t on_ip,
                              void *user_ctx)
{
    BaseType_t task_create_status;
    esp_err_t status;

    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_poom_wifi_mac.task_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    status = poom_wifi_ctrl_init_sta();
    if(status != ESP_OK)
    {
        return status;
    }

    status = poom_wifi_mac_cache_original_mac_();
    if(status != ESP_OK)
    {
        return status;
    }

    poom_wifi_mac_copy_str_(s_poom_wifi_mac.ssid, sizeof(s_poom_wifi_mac.ssid), ssid);
    poom_wifi_mac_copy_str_(s_poom_wifi_mac.password, sizeof(s_poom_wifi_mac.password), password);
    s_poom_wifi_mac.cycle_delay_ms = cycle_delay_ms;
    s_poom_wifi_mac.on_ip = on_ip;
    s_poom_wifi_mac.user_ctx = user_ctx;
    s_poom_wifi_mac.task_running = true;

    status = poom_wifi_ctrl_register_cb(poom_wifi_mac_wifi_evt_cb_, NULL);
    if(status != ESP_OK)
    {
        s_poom_wifi_mac.task_running = false;
        return status;
    }

    task_create_status = xTaskCreate(poom_wifi_mac_rotation_task_,
                                     "poom_wifi_mac",
                                     POOM_WIFI_MAC_TASK_STACK_WORDS,
                                     NULL,
                                     POOM_WIFI_MAC_TASK_PRIORITY,
                                     &s_poom_wifi_mac.task_handle);
    if(task_create_status != pdPASS)
    {
        s_poom_wifi_mac.task_running = false;
        s_poom_wifi_mac.task_handle = NULL;
        (void)poom_wifi_ctrl_unregister_cb();
        return ESP_FAIL;
    }

    status = poom_wifi_mac_connect_router_();
    if(status != ESP_OK)
    {
        s_poom_wifi_mac.task_running = false;
        if(s_poom_wifi_mac.task_handle != NULL)
        {
            xTaskNotify(s_poom_wifi_mac.task_handle, POOM_WIFI_MAC_NOTIFY_GOT_IP, eSetBits);

            while(s_poom_wifi_mac.task_handle != NULL)
            {
                vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_MAC_STOP_WAIT_MS));
            }
        }

        (void)poom_wifi_ctrl_unregister_cb();
        return status;
    }

    POOM_WIFI_MAC_PRINTF_I("Asynchronous MAC rotation started");
    return ESP_OK;
}

esp_err_t poom_wifi_mac_stop(void)
{
    esp_err_t status = ESP_OK;

    if(s_poom_wifi_mac.task_running)
    {
        s_poom_wifi_mac.task_running = false;

        if(s_poom_wifi_mac.task_handle != NULL)
        {
            xTaskNotify(s_poom_wifi_mac.task_handle, POOM_WIFI_MAC_NOTIFY_GOT_IP, eSetBits);
        }

        while(s_poom_wifi_mac.task_handle != NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_MAC_STOP_WAIT_MS));
        }
    }

    (void)poom_wifi_ctrl_unregister_cb();
    (void)poom_wifi_ctrl_deinit();

    status = poom_wifi_ctrl_init_sta();
    if(status != ESP_OK)
    {
        return status;
    }

    status = poom_wifi_mac_restore_sta_();
    if(status != ESP_OK)
    {
        return status;
    }

    s_poom_wifi_mac.on_ip = NULL;
    s_poom_wifi_mac.user_ctx = NULL;
    s_poom_wifi_mac.ssid[0] = '\0';
    s_poom_wifi_mac.password[0] = '\0';
    s_poom_wifi_mac.cycle_delay_ms = 0U;

    POOM_WIFI_MAC_PRINTF_I("Asynchronous MAC rotation stopped");
    return ESP_OK;
}
