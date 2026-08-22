// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_ESPNOW_MAC_LEN (6U)
#define POOM_ESPNOW_MAX_PAYLOAD (250U)

typedef struct
{
    uint8_t channel;         /* 1..13 (primary) */
    wifi_interface_t ifx;    /* typically WIFI_IF_STA */
    bool add_broadcast_peer; /* add ff:ff:ff:ff:ff:ff peer */
    uint8_t rx_queue_len;    /* internal RX queue depth (0 -> default) */
} poom_espnow_config_t;

typedef struct
{
    uint8_t src_mac[POOM_ESPNOW_MAC_LEN];
    uint16_t len;
    int8_t rssi_dbm; /* 0 if unknown */
    uint8_t channel; /* 0 if unknown */
    uint8_t data[POOM_ESPNOW_MAX_PAYLOAD];
} poom_espnow_rx_frame_t;

typedef void (*poom_espnow_rx_cb_t)(const poom_espnow_rx_frame_t *frame, void *user_ctx);

void poom_espnow_config_default(poom_espnow_config_t *out_cfg);

bool poom_espnow_is_running(void);

/**
 * @brief Starts Wi-Fi (STA) and ESPNOW. RX callbacks are invoked from an internal task.
 */
esp_err_t poom_espnow_start(const poom_espnow_config_t *cfg);

/**
 * @brief Stops ESPNOW and deinitializes Wi-Fi through poom_wifi_ctrl.
 */
esp_err_t poom_espnow_stop(void);

/**
 * @brief Register an RX callback. Called from internal task context.
 */
void poom_espnow_register_rx_cb(poom_espnow_rx_cb_t cb, void *user_ctx);

/**
 * @brief Set primary channel (1..13). Requires running.
 */
esp_err_t poom_espnow_set_channel(uint8_t channel);

/**
 * @brief Add/remove peers.
 *
 * Note: broadcast peer uses ff:ff:ff:ff:ff:ff.
 */
esp_err_t poom_espnow_peer_add(const uint8_t mac[POOM_ESPNOW_MAC_LEN], uint8_t channel, wifi_interface_t ifx, bool encrypt);
esp_err_t poom_espnow_peer_del(const uint8_t mac[POOM_ESPNOW_MAC_LEN]);

/**
 * @brief Send payload to a peer.
 *
 * Payload must be <= POOM_ESPNOW_MAX_PAYLOAD.
 */
esp_err_t poom_espnow_send(const uint8_t dst_mac[POOM_ESPNOW_MAC_LEN], const void *data, size_t len);

#ifdef __cplusplus
}
#endif
