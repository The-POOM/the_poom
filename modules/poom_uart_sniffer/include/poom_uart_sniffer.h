// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_UART_SNIFFER_H
#define POOM_UART_SNIFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Defines supported packet categories for UART framing.
 */
typedef enum
{
    poom_uart_sniffer_packet_type_zigbee = 0,
    poom_uart_sniffer_packet_type_ble,
    poom_uart_sniffer_packet_type_wifi,
    poom_uart_sniffer_packet_type_thread,
} poom_uart_sniffer_packet_type_t;

#define UART_SENDER_PACKET_TYPE_ZIGBEE poom_uart_sniffer_packet_type_zigbee
#define UART_SENDER_PACKET_TYPE_BLE poom_uart_sniffer_packet_type_ble
#define UART_SENDER_PACKET_TYPE_WIFI poom_uart_sniffer_packet_type_wifi
#define UART_SENDER_PACKET_TYPE_THREAD poom_uart_sniffer_packet_type_thread

/**
 * @brief Sends a generic packet over UART using TI-like framing.
 * @param[in] type Packet category.
 * @param[in,out] packet Pointer to payload bytes.
 * @param[in] len Payload length in bytes.
 * @return void
 */
void poom_uart_sniffer_send_packet(poom_uart_sniffer_packet_type_t type, uint8_t *packet, uint8_t len);

/**
 * @brief Sends a BLE advertising report over UART.
 * @param[in] type Packet category.
 * @param[in,out] packet BLE GAP callback payload.
 * @return void
 */
void poom_uart_sniffer_send_packet_ble(poom_uart_sniffer_packet_type_t type, esp_ble_gap_cb_param_t *packet);

/**
 * @brief Sends an IEEE 802.15.4 frame over UART.
 * @param[in] type Packet category.
 * @param[in] packet IEEE 802.15.4 payload without FCS.
 * @param[in] len Payload length in bytes.
 * @param[in] channel Real capture channel (11..26).
 * @param[in] rssi Packet RSSI in dBm.
 * @return void
 */
void poom_uart_sniffer_send_packet_ieee802154(
    poom_uart_sniffer_packet_type_t type,
    const uint8_t *packet,
    uint8_t len,
    uint8_t channel,
    int8_t rssi);

/**
 * @brief Deinitializes the UART sniffer module state.
 * @return void
 */
void poom_uart_sniffer_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
