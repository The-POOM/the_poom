// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ieee802154_sniffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_ieee802154.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "poom_scanner_core_ieee802154_isr.h"
#include "poom_uart_sniffer.h"

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)

#define POOM_IEEE802154_SNIFFER_MAX_FRAME_LEN (127U)
#define POOM_IEEE802154_SNIFFER_QUEUE_LEN (32U)
#define POOM_IEEE802154_SNIFFER_TASK_STACK (3584U)
#define POOM_IEEE802154_SNIFFER_TASK_PRIO (6U)

typedef struct
{
    uint8_t len;
    uint8_t channel;
    int8_t rssi;
    uint8_t data[POOM_IEEE802154_SNIFFER_MAX_FRAME_LEN];
} poom_ieee802154_sniffer_item_t;

static QueueHandle_t s_ieee802154_sniffer_q = NULL;
static TaskHandle_t s_ieee802154_sniffer_task = NULL;
static volatile bool s_ieee802154_sniffer_active = false;
static bool s_ieee802154_sniffer_uart_forward_enabled = false;
static uint8_t s_ieee802154_sniffer_channel = 0U;
static int8_t s_ieee802154_sniffer_recent_rssi = -127;
static uint32_t s_ieee802154_sniffer_packet_count = 0U;

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_ieee802154_sniffer_task_(void *arg)
{
    (void)arg;

    while (s_ieee802154_sniffer_active)
    {
        poom_ieee802154_sniffer_item_t item = {0};

        if ((s_ieee802154_sniffer_q == NULL) ||
            (xQueueReceive(s_ieee802154_sniffer_q, &item, pdMS_TO_TICKS(100)) != pdTRUE))
        {
            continue;
        }

        if (item.len == 0U)
        {
            continue;
        }

        s_ieee802154_sniffer_recent_rssi = item.rssi;
        s_ieee802154_sniffer_packet_count++;

        if (s_ieee802154_sniffer_uart_forward_enabled)
        {
            poom_uart_sniffer_send_packet_ieee802154(
                poom_uart_sniffer_packet_type_zigbee,
                item.data,
                item.len,
                item.channel,
                item.rssi);
        }
    }

    s_ieee802154_sniffer_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Internal helper for `poom_ieee802154_sniffer_isr_consumer`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] frame_info Parameter passed to the function.
 * @param[in] woken Parameter passed to the function.
 * @param[in] user Parameter passed to the function.
 * @return void
 */
static void poom_ieee802154_sniffer_isr_consumer_(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *frame_info,
    BaseType_t *woken,
    void *user)
{
    (void)frame_info;
    (void)user;

    if ((frame == NULL) || !s_ieee802154_sniffer_active || (s_ieee802154_sniffer_q == NULL) || (woken == NULL))
    {
        return;
    }

    poom_ieee802154_sniffer_item_t item = {0};
    const uint8_t raw_len = frame[0];
    uint8_t payload_len = raw_len;

    if (payload_len >= 2U)
    {
        payload_len = (uint8_t)(payload_len - 2U);
        item.rssi = (int8_t)frame[1U + raw_len - 2U];
    }
    else
    {
        item.rssi = -127;
        payload_len = 0U;
    }

    if (payload_len > POOM_IEEE802154_SNIFFER_MAX_FRAME_LEN)
    {
        payload_len = POOM_IEEE802154_SNIFFER_MAX_FRAME_LEN;
    }

    item.len = payload_len;
    item.channel = s_ieee802154_sniffer_channel;

    if (payload_len > 0U)
    {
        memcpy(item.data, frame + 1, payload_len);
    }

    (void)xQueueSendFromISR(s_ieee802154_sniffer_q, &item, woken);
}

void poom_ieee802154_sniffer_set_uart_forward_enabled(bool enabled)
{
    s_ieee802154_sniffer_uart_forward_enabled = enabled;
}

esp_err_t poom_ieee802154_sniffer_start(uint8_t channel)
{
    esp_err_t ret;

    if ((channel < POOM_IEEE802154_SNIFFER_CHANNEL_MIN) || (channel > POOM_IEEE802154_SNIFFER_CHANNEL_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ieee802154_sniffer_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ieee802154_sniffer_q == NULL)
    {
        s_ieee802154_sniffer_q = xQueueCreate(
            POOM_IEEE802154_SNIFFER_QUEUE_LEN,
            sizeof(poom_ieee802154_sniffer_item_t));
        if (s_ieee802154_sniffer_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    s_ieee802154_sniffer_channel = channel;
    s_ieee802154_sniffer_recent_rssi = -127;
    s_ieee802154_sniffer_packet_count = 0U;

    ret = esp_ieee802154_enable();
    if (ret != ESP_OK)
    {
        (void)poom_ieee802154_sniffer_stop();
        return ret;
    }

    (void)esp_ieee802154_set_coordinator(false);
    (void)esp_ieee802154_set_promiscuous(true);
    (void)esp_ieee802154_set_rx_when_idle(true);
    (void)esp_ieee802154_set_channel(channel);

    uint8_t eui64[8] = {0};
    uint8_t eui64_rev[8] = {0};
    (void)esp_read_mac(eui64, ESP_MAC_IEEE802154);
    for (int i = 0; i < 8; i++)
    {
        eui64_rev[7 - i] = eui64[i];
    }
    (void)esp_ieee802154_set_extended_address(eui64_rev);

    if (s_ieee802154_sniffer_task == NULL)
    {
        if (xTaskCreate(
                poom_ieee802154_sniffer_task_,
                "poom_154_rt",
                POOM_IEEE802154_SNIFFER_TASK_STACK,
                NULL,
                POOM_IEEE802154_SNIFFER_TASK_PRIO,
                &s_ieee802154_sniffer_task) != pdPASS)
        {
            s_ieee802154_sniffer_task = NULL;
            (void)poom_ieee802154_sniffer_stop();
            return ESP_ERR_NO_MEM;
        }
    }

    s_ieee802154_sniffer_active = true;

    ret = poom_scanner_core_ieee802154_register_isr_consumer(
        poom_ieee802154_sniffer_isr_consumer_,
        NULL);
    if (ret != ESP_OK)
    {
        (void)poom_ieee802154_sniffer_stop();
        return ret;
    }

    (void)esp_ieee802154_receive();
    return ESP_OK;
}

esp_err_t poom_ieee802154_sniffer_stop(void)
{
    s_ieee802154_sniffer_active = false;
    s_ieee802154_sniffer_uart_forward_enabled = false;
    s_ieee802154_sniffer_channel = 0U;

    poom_scanner_core_ieee802154_unregister_isr_consumer(
        poom_ieee802154_sniffer_isr_consumer_);

    (void)esp_ieee802154_set_rx_when_idle(false);
    (void)esp_ieee802154_set_promiscuous(false);
    (void)esp_ieee802154_disable();

    if (s_ieee802154_sniffer_task != NULL)
    {
        TaskHandle_t task = s_ieee802154_sniffer_task;
        s_ieee802154_sniffer_task = NULL;
        vTaskDelete(task);
    }

    if (s_ieee802154_sniffer_q != NULL)
    {
        vQueueDelete(s_ieee802154_sniffer_q);
        s_ieee802154_sniffer_q = NULL;
    }

    return ESP_OK;
}

bool poom_ieee802154_sniffer_is_active(void)
{
    return s_ieee802154_sniffer_active;
}

uint8_t poom_ieee802154_sniffer_get_channel(void)
{
    if (!s_ieee802154_sniffer_active)
    {
        return 0U;
    }

    return s_ieee802154_sniffer_channel;
}

int8_t poom_ieee802154_sniffer_get_recent_rssi(void)
{
    return s_ieee802154_sniffer_recent_rssi;
}

uint32_t poom_ieee802154_sniffer_get_packet_count(void)
{
    return s_ieee802154_sniffer_packet_count;
}

#else

void poom_ieee802154_sniffer_set_uart_forward_enabled(bool enabled)
{
    (void)enabled;
}

esp_err_t poom_ieee802154_sniffer_start(uint8_t channel)
{
    (void)channel;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t poom_ieee802154_sniffer_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool poom_ieee802154_sniffer_is_active(void)
{
    return false;
}

uint8_t poom_ieee802154_sniffer_get_channel(void)
{
    return 0U;
}

int8_t poom_ieee802154_sniffer_get_recent_rssi(void)
{
    return -127;
}

uint32_t poom_ieee802154_sniffer_get_packet_count(void)
{
    return 0U;
}

#endif
