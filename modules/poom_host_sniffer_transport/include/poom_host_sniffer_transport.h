// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_HOST_SNIFFER_TRANSPORT_H
#define POOM_HOST_SNIFFER_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_HOST_SNIFFER_TRANSPORT_VERSION (0x01U)
#define POOM_HOST_SNIFFER_TRANSPORT_MAX_PAYLOAD_LEN (255U)
#define POOM_HOST_SNIFFER_TRANSPORT_STATUS_OK (0x80U)
#define POOM_HOST_SNIFFER_TRANSPORT_BLE_CHANNEL_UNKNOWN (0U)

typedef enum
{
    POOM_HOST_SNIFFER_TRANSPORT_FRAME_DATA = 0x01,
    POOM_HOST_SNIFFER_TRANSPORT_FRAME_COMMAND_RESPONSE = 0x02,
    POOM_HOST_SNIFFER_TRANSPORT_FRAME_ERROR = 0x03,
} poom_host_sniffer_transport_frame_type_t;

esp_err_t poom_host_sniffer_transport_send_frame(
    poom_host_sniffer_transport_frame_type_t frame_type,
    uint8_t channel,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_len,
    int8_t rssi,
    uint8_t status);

esp_err_t poom_host_sniffer_transport_send_ble_gap_scan_result(
    const esp_ble_gap_cb_param_t *scan_result);

esp_err_t poom_host_sniffer_transport_send_ieee802154_packet(
    const uint8_t *packet,
    size_t packet_len,
    uint8_t channel,
    int8_t rssi);

#ifdef __cplusplus
}
#endif

#endif
