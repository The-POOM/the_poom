// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_uart_sniffer.h"

#include "poom_host_sniffer_transport.h"

static bool s_poom_uart_sniffer_initialized = false;

/**
 * @brief Initializes module state before packet transmission.
 * @return void
 */
static void poom_uart_sniffer_init_(void)
{
    if(!s_poom_uart_sniffer_initialized)
    {
        s_poom_uart_sniffer_initialized = true;
    }
}

/**
 * @brief Calculates BLE CRC24 for payload bytes.
 * @param[in] data Pointer to payload bytes.
 * @param[in] len Number of payload bytes.
 * @return uint32_t
 */
void poom_uart_sniffer_deinit(void)
{
    s_poom_uart_sniffer_initialized = false;
}

/**
 * @brief Sends a generic packet over UART using TI-like framing.
 * @param[in] type Packet category.
 * @param[in,out] packet Pointer to payload bytes.
 * @param[in] len Payload length in bytes.
 * @return void
 */
void poom_uart_sniffer_send_packet(poom_uart_sniffer_packet_type_t type, uint8_t *packet, uint8_t len)
{
    (void)type;

    if(packet == NULL)
    {
        return;
    }

    poom_uart_sniffer_init_();
    (void)poom_host_sniffer_transport_send_frame(
        POOM_HOST_SNIFFER_TRANSPORT_FRAME_DATA,
        0U,
        0U,
        packet,
        len,
        0,
        POOM_HOST_SNIFFER_TRANSPORT_STATUS_OK);
}

/**
 * @brief Sends a BLE advertising report over UART.
 * @param[in] type Packet category.
 * @param[in,out] packet BLE GAP callback payload.
 * @return void
 */
void poom_uart_sniffer_send_packet_ble(poom_uart_sniffer_packet_type_t type, esp_ble_gap_cb_param_t *packet)
{
    (void)type;

    if(packet == NULL)
    {
        return;
    }

    poom_uart_sniffer_init_();
    (void)poom_host_sniffer_transport_send_ble_gap_scan_result(packet);
}

void poom_uart_sniffer_send_packet_ieee802154(
    poom_uart_sniffer_packet_type_t type,
    const uint8_t *packet,
    uint8_t len,
    uint8_t channel,
    int8_t rssi)
{
    (void)type;

    if(packet == NULL)
    {
        return;
    }

    poom_uart_sniffer_init_();
    (void)poom_host_sniffer_transport_send_ieee802154_packet(
        packet,
        len,
        channel,
        rssi);
}
