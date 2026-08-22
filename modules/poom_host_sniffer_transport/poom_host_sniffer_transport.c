// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_host_sniffer_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define POOM_HOST_SNIFFER_TRANSPORT_BLE_HCI_MAX_LEN (64U)
#define POOM_HOST_SNIFFER_TRANSPORT_RAW_OVERHEAD (7U)
#define POOM_HOST_SNIFFER_TRANSPORT_CRC_LEN (2U)
#define POOM_HOST_SNIFFER_TRANSPORT_MAX_RAW_LEN \
    (POOM_HOST_SNIFFER_TRANSPORT_RAW_OVERHEAD + POOM_HOST_SNIFFER_TRANSPORT_MAX_PAYLOAD_LEN + POOM_HOST_SNIFFER_TRANSPORT_CRC_LEN)
#define POOM_HOST_SNIFFER_TRANSPORT_MAX_ENCODED_LEN (POOM_HOST_SNIFFER_TRANSPORT_MAX_RAW_LEN + 4U)
#define POOM_HOST_SNIFFER_TRANSPORT_BLE_ADV_MAX_LEN (31U)

/**
 * @brief Prepares a BLE advertising payload for transport.
 *
 * @param[in] src Source payload fields.
 * @param[in] src_len Source payload length.
 * @param[in] flags BLE advertising flags.
 * @param[out] dst Destination payload buffer.
 * @param[in] dst_len Destination buffer length.
 * @return uint8_t
 */
static uint8_t poom_host_sniffer_transport_ble_prepare_adv_payload_(
    const uint8_t *src,
    uint8_t src_len,
    int flags,
    uint8_t *dst,
    size_t dst_len)
{
    bool has_flags = false;
    size_t i = 0U;

    if ((dst == NULL) || (dst_len == 0U))
    {
        return 0U;
    }

    if (src_len > (uint8_t)dst_len)
    {
        src_len = (uint8_t)dst_len;
    }

    while ((src != NULL) && (i < src_len))
    {
        uint8_t field_len = src[i];
        size_t end;

        if (field_len == 0U)
        {
            break;
        }

        end = i + 1U + (size_t)field_len;
        if (end > src_len)
        {
            break;
        }

        if ((field_len >= 1U) && (src[i + 1U] == ESP_BLE_AD_TYPE_FLAG))
        {
            has_flags = true;
            break;
        }

        i = end;
    }

    if ((src != NULL) && (src_len > 0U))
    {
        memcpy(dst, src, src_len);
    }

    if (!has_flags && (flags > 0) && (src_len <= (uint8_t)(dst_len - 3U)))
    {
        memmove(dst + 3U, dst, src_len);
        dst[0] = 0x02U;
        dst[1] = ESP_BLE_AD_TYPE_FLAG;
        dst[2] = (uint8_t)(flags & 0xFF);
        src_len = (uint8_t)(src_len + 3U);
    }

    return src_len;
}

/**
 * @brief Internal helper for `poom_host_sniffer_transport_crc16_ccitt`.
 *
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return uint16_t
 */
static uint16_t poom_host_sniffer_transport_crc16_ccitt_(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    if (data == NULL)
    {
        return crc;
    }

    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)(((crc << 1U) ^ 0x1021U) & 0xFFFFU);
            }
            else
            {
                crc = (uint16_t)((crc << 1U) & 0xFFFFU);
            }
        }
    }

    return crc;
}

/**
 * @brief Internal helper for `poom_host_sniffer_transport_cobs_encode`.
 *
 * @param[in] input Parameter passed to the function.
 * @param[in] input_len Parameter passed to the function.
 * @param[in] output Parameter passed to the function.
 * @param[in] output_len Parameter passed to the function.
 * @return size_t
 */
static size_t poom_host_sniffer_transport_cobs_encode_(
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_len)
{
    size_t read_index = 0U;
    size_t write_index = 1U;
    size_t code_index = 0U;
    uint8_t code = 1U;

    if ((input == NULL) || (output == NULL) || (output_len == 0U))
    {
        return 0U;
    }

    while (read_index < input_len)
    {
        if (write_index >= output_len)
        {
            return 0U;
        }

        if (input[read_index] == 0U)
        {
            output[code_index] = code;
            code = 1U;
            code_index = write_index++;
            read_index++;
            continue;
        }

        output[write_index++] = input[read_index++];
        code++;

        if (code == 0xFFU)
        {
            output[code_index] = code;
            code = 1U;
            code_index = write_index++;
            if (write_index > output_len)
            {
                return 0U;
            }
        }
    }

    if (code_index >= output_len)
    {
        return 0U;
    }

    output[code_index] = code;
    return write_index;
}

esp_err_t poom_host_sniffer_transport_send_frame(
    poom_host_sniffer_transport_frame_type_t frame_type,
    uint8_t channel,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_len,
    int8_t rssi,
    uint8_t status)
{
    uint8_t raw[POOM_HOST_SNIFFER_TRANSPORT_MAX_RAW_LEN];
    uint8_t encoded[POOM_HOST_SNIFFER_TRANSPORT_MAX_ENCODED_LEN];
    size_t raw_len = 0U;
    size_t encoded_len;
    uint16_t crc;

    if ((payload == NULL) && (payload_len > 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_len > POOM_HOST_SNIFFER_TRANSPORT_MAX_PAYLOAD_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    raw[raw_len++] = POOM_HOST_SNIFFER_TRANSPORT_VERSION;
    raw[raw_len++] = (uint8_t)frame_type;
    raw[raw_len++] = channel;
    raw[raw_len++] = flags;
    raw[raw_len++] = (uint8_t)payload_len;

    if (payload_len > 0U)
    {
        memcpy(&raw[raw_len], payload, payload_len);
        raw_len += payload_len;
    }

    raw[raw_len++] = (uint8_t)rssi;
    raw[raw_len++] = status;

    crc = poom_host_sniffer_transport_crc16_ccitt_(raw, raw_len);
    raw[raw_len++] = (uint8_t)(crc & 0xFFU);
    raw[raw_len++] = (uint8_t)((crc >> 8U) & 0xFFU);

    encoded_len = poom_host_sniffer_transport_cobs_encode_(
        raw,
        raw_len,
        encoded,
        sizeof(encoded));
    if (encoded_len == 0U)
    {
        return ESP_FAIL;
    }

    (void)fwrite(encoded, 1U, encoded_len, stdout);
    (void)fputc(0, stdout);
    (void)fflush(stdout);

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_host_sniffer_transport_send_ble_hci_packet`.
 *
 * @param[in] packet Parameter passed to the function.
 * @param[in] packet_len Parameter passed to the function.
 * @param[in] rssi Parameter passed to the function.
 * @param[in] flags Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_host_sniffer_transport_send_ble_hci_packet_(
    const uint8_t *packet,
    size_t packet_len,
    int8_t rssi,
    uint8_t flags)
{
    return poom_host_sniffer_transport_send_frame(
        POOM_HOST_SNIFFER_TRANSPORT_FRAME_DATA,
        POOM_HOST_SNIFFER_TRANSPORT_BLE_CHANNEL_UNKNOWN,
        flags,
        packet,
        packet_len,
        rssi,
        POOM_HOST_SNIFFER_TRANSPORT_STATUS_OK);
}

esp_err_t poom_host_sniffer_transport_send_ble_gap_scan_result(
    const esp_ble_gap_cb_param_t *scan_result)
{
    uint8_t adv_len;
    uint8_t scan_rsp_len;
    uint8_t addr_type;
    uint8_t evt_type;
    int8_t rssi;
    size_t scan_rsp_off;
    uint8_t adv_payload[POOM_HOST_SNIFFER_TRANSPORT_BLE_ADV_MAX_LEN];
    const size_t ble_adv_len = sizeof(scan_result->scan_rst.ble_adv);

    if (scan_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    adv_len = scan_result->scan_rst.adv_data_len;
    scan_rsp_len = scan_result->scan_rst.scan_rsp_len;
    evt_type = (uint8_t)scan_result->scan_rst.ble_evt_type;
    addr_type = (uint8_t)scan_result->scan_rst.ble_addr_type;
    rssi = (int8_t)scan_result->scan_rst.rssi;
    scan_rsp_off = scan_result->scan_rst.adv_data_len;
    if (scan_rsp_off > ble_adv_len)
    {
        scan_rsp_off = ble_adv_len;
    }

    if (adv_len > 31U)
    {
        adv_len = 31U;
    }
    if (scan_rsp_len > 31U)
    {
        scan_rsp_len = 31U;
    }

    adv_len = poom_host_sniffer_transport_ble_prepare_adv_payload_(
        scan_result->scan_rst.ble_adv,
        adv_len,
        scan_result->scan_rst.flag,
        adv_payload,
        sizeof(adv_payload));

    if (adv_len > 0U)
    {
        uint8_t hci[POOM_HOST_SNIFFER_TRANSPORT_BLE_HCI_MAX_LEN];
        size_t idx = 0U;

        hci[idx++] = 0x04U;
        hci[idx++] = 0x3EU;
        hci[idx++] = 0x00U;
        hci[idx++] = 0x02U;
        hci[idx++] = 0x01U;
        hci[idx++] = evt_type;
        hci[idx++] = addr_type;
        memcpy(&hci[idx], scan_result->scan_rst.bda, 6U);
        idx += 6U;
        hci[idx++] = adv_len;
        memcpy(&hci[idx], adv_payload, adv_len);
        idx += adv_len;
        hci[idx++] = (uint8_t)rssi;
        hci[2] = (uint8_t)(idx - 3U);

        esp_err_t ret = poom_host_sniffer_transport_send_ble_hci_packet_(hci, idx, rssi, evt_type);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    if (scan_rsp_len > 0U)
    {
        const size_t avail = ble_adv_len - scan_rsp_off;
        uint8_t hci[POOM_HOST_SNIFFER_TRANSPORT_BLE_HCI_MAX_LEN];
        size_t idx = 0U;

        if ((size_t)scan_rsp_len > avail)
        {
            scan_rsp_len = (uint8_t)avail;
        }

        if (scan_rsp_len == 0U)
        {
            return ESP_OK;
        }

        hci[idx++] = 0x04U;
        hci[idx++] = 0x3EU;
        hci[idx++] = 0x00U;
        hci[idx++] = 0x02U;
        hci[idx++] = 0x01U;
        hci[idx++] = 0x04U;
        hci[idx++] = addr_type;
        memcpy(&hci[idx], scan_result->scan_rst.bda, 6U);
        idx += 6U;
        hci[idx++] = scan_rsp_len;
        memcpy(&hci[idx], &scan_result->scan_rst.ble_adv[scan_rsp_off], scan_rsp_len);
        idx += scan_rsp_len;
        hci[idx++] = (uint8_t)rssi;
        hci[2] = (uint8_t)(idx - 3U);

        return poom_host_sniffer_transport_send_ble_hci_packet_(hci, idx, rssi, 0x04U);
    }

    return ESP_OK;
}

esp_err_t poom_host_sniffer_transport_send_ieee802154_packet(
    const uint8_t *packet,
    size_t packet_len,
    uint8_t channel,
    int8_t rssi)
{
    return poom_host_sniffer_transport_send_frame(
        POOM_HOST_SNIFFER_TRANSPORT_FRAME_DATA,
        channel,
        0U,
        packet,
        packet_len,
        rssi,
        POOM_HOST_SNIFFER_TRANSPORT_STATUS_OK);
}
