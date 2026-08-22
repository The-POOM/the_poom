// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_espnow.h"

#include <string.h>

#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "poom_wifi_ctrl.h"

#define POOM_ESPNOW_TASK_STACK (3072U)
#define POOM_ESPNOW_TASK_PRIO (4U)
#define POOM_ESPNOW_RX_QUEUE_DEFAULT (16U)

typedef struct
{
    uint8_t mac[POOM_ESPNOW_MAC_LEN];
    uint16_t len;
    int8_t rssi_dbm;
    uint8_t channel;
    uint8_t data[POOM_ESPNOW_MAX_PAYLOAD];
} rx_item_t;

static volatile bool s_running = false;
static TaskHandle_t s_task = NULL;
static QueueHandle_t s_rx_q = NULL;

static poom_espnow_rx_cb_t s_rx_cb = NULL;
static void *s_rx_cb_ctx = NULL;

static const uint8_t s_bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] info Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return void
 */
static void rx_cb_internal_v2_(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_running || (s_rx_q == NULL) || (info == NULL) || (data == NULL) || (len <= 0))
    {
        return;
    }

    rx_item_t item = {0};
    memcpy(item.mac, info->src_addr, POOM_ESPNOW_MAC_LEN);
    if (info->rx_ctrl != NULL)
    {
        item.rssi_dbm = (int8_t)info->rx_ctrl->rssi;
        item.channel = (uint8_t)info->rx_ctrl->channel;
    }
    size_t copy_len = (size_t)len;
    if (copy_len > POOM_ESPNOW_MAX_PAYLOAD)
    {
        copy_len = POOM_ESPNOW_MAX_PAYLOAD;
    }
    item.len = (uint16_t)copy_len;
    memcpy(item.data, data, copy_len);
    (void)xQueueSend(s_rx_q, &item, 0U);
}
#else

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] mac_addr Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return void
 */
static void rx_cb_internal_v1_(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (!s_running || (s_rx_q == NULL) || (mac_addr == NULL) || (data == NULL) || (len <= 0))
    {
        return;
    }

    rx_item_t item = {0};
    memcpy(item.mac, mac_addr, POOM_ESPNOW_MAC_LEN);
    size_t copy_len = (size_t)len;
    if (copy_len > POOM_ESPNOW_MAX_PAYLOAD)
    {
        copy_len = POOM_ESPNOW_MAX_PAYLOAD;
    }
    item.len = (uint16_t)copy_len;
    memcpy(item.data, data, copy_len);
    (void)xQueueSend(s_rx_q, &item, 0U);
}
#endif

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void task_(void *arg)
{
    (void)arg;

    while (s_running)
    {
        rx_item_t item = {0};
        if ((s_rx_q != NULL) && (xQueueReceive(s_rx_q, &item, pdMS_TO_TICKS(200U)) == pdPASS))
        {
            if (s_rx_cb != NULL)
            {
                poom_espnow_rx_frame_t f = {0};
                memcpy(f.src_mac, item.mac, sizeof(f.src_mac));
                f.len = item.len;
                f.rssi_dbm = item.rssi_dbm;
                f.channel = item.channel;
                memcpy(f.data, item.data, (size_t)f.len);
                s_rx_cb(&f, s_rx_cb_ctx);
            }
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void poom_espnow_config_default(poom_espnow_config_t *out_cfg)
{
    if (out_cfg == NULL)
    {
        return;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->channel = 6U;
    out_cfg->ifx = WIFI_IF_STA;
    out_cfg->add_broadcast_peer = true;
    out_cfg->rx_queue_len = POOM_ESPNOW_RX_QUEUE_DEFAULT;
}

bool poom_espnow_is_running(void)
{
    return s_running;
}

void poom_espnow_register_rx_cb(poom_espnow_rx_cb_t cb, void *user_ctx)
{
    s_rx_cb = cb;
    s_rx_cb_ctx = user_ctx;
}

esp_err_t poom_espnow_set_channel(uint8_t channel)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return poom_wifi_ctrl_set_channel(channel);
}

esp_err_t poom_espnow_peer_add(const uint8_t mac[POOM_ESPNOW_MAC_LEN], uint8_t channel, wifi_interface_t ifx, bool encrypt)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, POOM_ESPNOW_MAC_LEN);
    peer.ifidx = ifx;
    peer.channel = channel;
    peer.encrypt = encrypt;

    const esp_err_t exists = esp_now_is_peer_exist(mac) ? ESP_OK : ESP_FAIL;
    if (exists == ESP_OK)
    {
        (void)esp_now_del_peer(mac);
    }

    return esp_now_add_peer(&peer);
}

esp_err_t poom_espnow_peer_del(const uint8_t mac[POOM_ESPNOW_MAC_LEN])
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_now_del_peer(mac);
}

esp_err_t poom_espnow_send(const uint8_t dst_mac[POOM_ESPNOW_MAC_LEN], const void *data, size_t len)
{
    if ((dst_mac == NULL) || (data == NULL) || (len == 0U) || (len > POOM_ESPNOW_MAX_PAYLOAD))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_now_send(dst_mac, (const uint8_t *)data, len);
}

esp_err_t poom_espnow_start(const poom_espnow_config_t *cfg)
{
    if (s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_espnow_config_t local;
    poom_espnow_config_default(&local);
    if (cfg != NULL)
    {
        local = *cfg;
    }

    if ((local.channel < 1U) || (local.channel > 13U))
    {
        local.channel = 6U;
    }

    if (local.rx_queue_len == 0U)
    {
        local.rx_queue_len = POOM_ESPNOW_RX_QUEUE_DEFAULT;
    }

    esp_err_t err = poom_wifi_ctrl_init_sta();
    if (err != ESP_OK)
    {
        return err;
    }

    (void)poom_wifi_ctrl_sta_disconnect();
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    err = poom_wifi_ctrl_set_channel(local.channel);
    if (err != ESP_OK)
    {
        (void)poom_wifi_ctrl_deinit();
        return err;
    }

    err = esp_now_init();
    if (err != ESP_OK)
    {
        (void)poom_wifi_ctrl_deinit();
        return err;
    }

    if (local.add_broadcast_peer)
    {
        (void)poom_espnow_peer_add(s_bcast_mac, local.channel, local.ifx, false);
    }

    s_rx_q = xQueueCreate(local.rx_queue_len, sizeof(rx_item_t));
    if (s_rx_q == NULL)
    {
        (void)esp_now_deinit();
        (void)poom_wifi_ctrl_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    if (xTaskCreate(task_, "poom_espnow", POOM_ESPNOW_TASK_STACK, NULL, POOM_ESPNOW_TASK_PRIO, &s_task) != pdPASS)
    {
        s_running = false;
        vQueueDelete(s_rx_q);
        s_rx_q = NULL;
        (void)esp_now_deinit();
        (void)poom_wifi_ctrl_deinit();
        return ESP_FAIL;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    err = esp_now_register_recv_cb(rx_cb_internal_v2_);
#else
    err = esp_now_register_recv_cb(rx_cb_internal_v1_);
#endif
    if (err != ESP_OK)
    {
        (void)poom_espnow_stop();
        return err;
    }

    return ESP_OK;
}

esp_err_t poom_espnow_stop(void)
{
    if (!s_running)
    {
        return ESP_OK;
    }

    s_running = false;

    if (s_task != NULL)
    {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_rx_q != NULL)
    {
        vQueueDelete(s_rx_q);
        s_rx_q = NULL;
    }

    (void)esp_now_unregister_recv_cb();
    (void)esp_now_deinit();
    return poom_wifi_ctrl_deinit();
}
