// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_pcap_capture.c
 * @brief Capture helpers (WiFi / BLE / IEEE 802.15.4) built on `poom_pcap_manager`.
 *
 * This file intentionally keeps protocol capture details out of UI/menu code.
 */

#include "poom_pcap_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"

#include "poom_ble_scan.h"
#include "poom_wifi_ctrl.h"


#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "poom_scanner_core_ieee802154_isr.h"

typedef enum
{
    POOM_PCAP_SNIFFER_MODE_NONE = 0,
    POOM_PCAP_SNIFFER_MODE_WIFI,
    POOM_PCAP_SNIFFER_MODE_BLE,
    POOM_PCAP_SNIFFER_MODE_ZIGBEE,
} poom_pcap_sniffer_mode_t;

static poom_pcap_sniffer_mode_t s_mode = POOM_PCAP_SNIFFER_MODE_NONE;

// =========================
// WiFi
// =========================

static poom_pcap_wifi_capture_t s_wifi_capture = POOM_PCAP_WIFI_CAPTURE_RAW;

/**
 * @brief Internal helper for `poom_pcap_wifi_get_frame_control`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @param[in] out_fc Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_get_frame_control_(const uint8_t *frame, size_t len, uint16_t *out_fc)
{
    if ((frame == NULL) || (out_fc == NULL) || (len < 2U))
    {
        return false;
    }

    *out_fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    return true;
}

/**
 * @brief Internal helper for `poom_pcap_wifi_is_protected`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_is_protected_(const uint8_t *frame, size_t len)
{
    uint16_t fc = 0;
    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc))
    {
        return false;
    }

    return ((fc >> 14) & 0x01U) != 0U;
}

/**
 * @brief Internal helper for `poom_pcap_wifi_is_mgmt_subtype`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @param[in] subtype Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_is_mgmt_subtype_(const uint8_t *frame, size_t len, uint8_t subtype)
{
    if ((frame == NULL) || (len < 2U))
    {
        return false;
    }

    uint16_t fc = 0;
    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc))
    {
        return false;
    }
    const uint8_t type = (uint8_t)((fc >> 2) & 0x03U);
    const uint8_t sub = (uint8_t)((fc >> 4) & 0x0FU);
    return (type == 0U) && (sub == subtype);
}

/**
 * @brief Internal helper for `poom_pcap_wifi_get_llc_ethertype`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @param[in] out_ethertype Parameter passed to the function.
 * @param[in] out_payload_off Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_get_llc_ethertype_(const uint8_t *frame, size_t len, uint16_t *out_ethertype, size_t *out_payload_off)
{
    if ((frame == NULL) || (len < 2U) || (out_ethertype == NULL))
    {
        return false;
    }

    uint16_t fc = 0;
    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc))
    {
        return false;
    }
    const uint8_t type = (uint8_t)((fc >> 2) & 0x03U);
    const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0FU);

    if (type != 2U) // Data
    {
        return false;
    }

    const bool to_ds = ((fc >> 8) & 0x01U) != 0U;
    const bool from_ds = ((fc >> 9) & 0x01U) != 0U;
    const bool qos = (subtype & 0x08U) != 0U;

    size_t hdr_len = (to_ds && from_ds) ? 30U : 24U;
    if (qos)
    {
        hdr_len += 2U;
    }

    if (len < (hdr_len + 8U))
    {
        return false;
    }

    const uint8_t *llc = frame + hdr_len;
    if ((llc[0] != 0xAAU) || (llc[1] != 0xAAU) || (llc[2] != 0x03U) || (llc[3] != 0x00U) || (llc[4] != 0x00U) || (llc[5] != 0x00U))
    {
        return false;
    }

    *out_ethertype = ((uint16_t)llc[6] << 8) | (uint16_t)llc[7];
    if (out_payload_off != NULL)
    {
        *out_payload_off = hdr_len + 8U;
    }
    return true;
}

/**
 * @brief Internal helper for `poom_pcap_wifi_is_eapol_any`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_is_eapol_any_(const uint8_t *frame, size_t len)
{
    uint16_t ethertype = 0U;
    if (!poom_pcap_wifi_get_llc_ethertype_(frame, len, &ethertype, NULL))
    {
        return false;
    }
    return (ethertype == 0x888EU);
}

/**
 * @brief Internal helper for `poom_pcap_wifi_is_wps_eap`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_is_wps_eap_(const uint8_t *frame, size_t len)
{
    uint16_t ethertype = 0U;
    size_t off = 0U;
    if (!poom_pcap_wifi_get_llc_ethertype_(frame, len, &ethertype, &off))
    {
        return false;
    }

    if (ethertype != 0x888EU)
    {
        return false;
    }

    if (len < (off + 4U + 12U))
    {
        return false;
    }

    const uint8_t eapol_type = frame[off + 1U];
    if (eapol_type != 0x00U) // EAP packet
    {
        return false;
    }

    const size_t eap_off = off + 4U;
    const uint8_t eap_type = frame[eap_off + 4U];
    if (eap_type != 0xFEU) // Expanded
    {
        return false;
    }

    if ((frame[eap_off + 5U] == 0x00U) && (frame[eap_off + 6U] == 0x37U) && (frame[eap_off + 7U] == 0x2AU))
    {
        return true;
    }

    return false;
}

/**
 * @brief Internal helper for `poom_pcap_wifi_should_capture`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return bool
 */
static bool poom_pcap_wifi_should_capture_(const uint8_t *frame, size_t len)
{
    switch (s_wifi_capture)
    {
        case POOM_PCAP_WIFI_CAPTURE_BEACON:
            return poom_pcap_wifi_is_mgmt_subtype_(frame, len, 8U);
        case POOM_PCAP_WIFI_CAPTURE_PROBE:
            return poom_pcap_wifi_is_mgmt_subtype_(frame, len, 4U);
        case POOM_PCAP_WIFI_CAPTURE_DEAUTH:
            return poom_pcap_wifi_is_mgmt_subtype_(frame, len, 12U);
        case POOM_PCAP_WIFI_CAPTURE_EAPOL:
            return poom_pcap_wifi_is_protected_(frame, len) || poom_pcap_wifi_is_eapol_any_(frame, len);
        case POOM_PCAP_WIFI_CAPTURE_WPS:
            return poom_pcap_wifi_is_wps_eap_(frame, len);
        case POOM_PCAP_WIFI_CAPTURE_RAW:
        default:
            return true;
    }
}

/**
 * @brief WiFi promiscuous RX callback for PCAP capture.
 *
 * Receives 802.11 frames from the ESP-IDF WiFi driver and forwards them to
 * `poom_pcap_manager_write_packet()` (which adds radiotap for WiFi captures).
 */
static void poom_pcap_sniffer_wifi_promisc_cb_(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;

    if (s_mode != POOM_PCAP_SNIFFER_MODE_WIFI)
    {
        return;
    }

    if ((pkt == NULL) || (type == WIFI_PKT_MISC))
    {
        return;
    }

    const size_t len = (size_t)pkt->rx_ctrl.sig_len;
    if (len == 0U)
    {
        return;
    }

    if (!poom_pcap_wifi_should_capture_(pkt->payload, len))
    {
        return;
    }

    (void)poom_pcap_manager_write_packet(pkt->payload, len, POOM_PCAP_CAPTURE_WIFI);
}

// =========================
// BLE (HCI LE Advertising Report, H4 over PCAP)
// =========================

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] src Parameter passed to the function.
 * @param[in] src_len Parameter passed to the function.
 * @param[in] flags Parameter passed to the function.
 * @param[in] dst Parameter passed to the function.
 * @param[in] dst_len Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t poom_pcap_ble_prepare_adv_payload_(
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
 * @brief BLE scan callback that converts GAP scan reports into HCI H4 events.
 *
 * Wireshark can decode these PCAP packets as Bluetooth HCI (DLT_BLUETOOTH_HCI_H4).
 */
static void poom_pcap_sniffer_ble_scan_cb_(const esp_ble_gap_cb_param_t *scan_result)
{
    if ((scan_result == NULL) || (s_mode != POOM_PCAP_SNIFFER_MODE_BLE))
    {
        return;
    }

    uint8_t adv_len = scan_result->scan_rst.adv_data_len;
    uint8_t scan_rsp_len = scan_result->scan_rst.scan_rsp_len;
    uint8_t evt_type = (uint8_t)scan_result->scan_rst.ble_evt_type;
    uint8_t addr_type = (uint8_t)scan_result->scan_rst.ble_addr_type;
    int8_t rssi = (int8_t)scan_result->scan_rst.rssi;
    uint8_t adv_payload[31];

    const size_t ble_adv_len = sizeof(scan_result->scan_rst.ble_adv);
    size_t scan_rsp_off = scan_result->scan_rst.adv_data_len;
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

    adv_len = poom_pcap_ble_prepare_adv_payload_(
        scan_result->scan_rst.ble_adv,
        adv_len,
        scan_result->scan_rst.flag,
        adv_payload,
        sizeof(adv_payload));

    if (adv_len > 0U)
    {
        uint8_t hci[64];
        size_t idx = 0;
        size_t param_len;

        hci[idx++] = 0x04U; // H4: Event
        hci[idx++] = 0x3EU; // LE Meta Event
        hci[idx++] = 0x00U; // plen placeholder
        hci[idx++] = 0x02U; // subevent: LE Advertising Report
        hci[idx++] = 0x01U; // num reports

        hci[idx++] = evt_type;
        hci[idx++] = addr_type;
        memcpy(&hci[idx], scan_result->scan_rst.bda, 6);
        idx += 6;
        hci[idx++] = adv_len;
        memcpy(&hci[idx], adv_payload, adv_len);
        idx += adv_len;
        hci[idx++] = (uint8_t)rssi;

        param_len = idx - 3U;
        hci[2] = (uint8_t)param_len;
        (void)poom_pcap_manager_write_packet(hci, idx, POOM_PCAP_CAPTURE_BLUETOOTH);
    }

    if (scan_rsp_len > 0U)
    {
        const size_t avail = ble_adv_len - scan_rsp_off;
        if ((size_t)scan_rsp_len > avail)
        {
            scan_rsp_len = (uint8_t)avail;
        }

        if (scan_rsp_len > 0U)
        {
            uint8_t hci[64];
            size_t idx = 0;
            size_t param_len;

            hci[idx++] = 0x04U; // H4: Event
            hci[idx++] = 0x3EU; // LE Meta Event
            hci[idx++] = 0x00U; // plen placeholder
            hci[idx++] = 0x02U; // subevent: LE Advertising Report
            hci[idx++] = 0x01U; // num reports

            hci[idx++] = 0x04U; // Event_Type: Scan Response
            hci[idx++] = addr_type;
            memcpy(&hci[idx], scan_result->scan_rst.bda, 6);
            idx += 6;
            hci[idx++] = scan_rsp_len;
            memcpy(&hci[idx], &scan_result->scan_rst.ble_adv[scan_rsp_off], scan_rsp_len);
            idx += scan_rsp_len;
            hci[idx++] = (uint8_t)rssi;

            param_len = idx - 3U;
            hci[2] = (uint8_t)param_len;
            (void)poom_pcap_manager_write_packet(hci, idx, POOM_PCAP_CAPTURE_BLUETOOTH);
        }
    }
}

// =========================
// Zigbee / IEEE 802.15.4 (NOFCS)
// =========================

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)

    #define POOM_PCAP_ZB_MAX_FRAME_LEN (127U)
    #define POOM_PCAP_ZB_QUEUE_LEN (32U)
    #define POOM_PCAP_ZB_TASK_STACK (3584U)
    #define POOM_PCAP_ZB_TASK_PRIO (6U)
    #define POOM_PCAP_ZB_HOP_MS_DEFAULT (200U)

typedef struct
{
    uint8_t len;
    uint8_t data[POOM_PCAP_ZB_MAX_FRAME_LEN];
} poom_pcap_zb_item_t;

static QueueHandle_t s_zb_q = NULL;
static TaskHandle_t s_zb_task = NULL;
static TimerHandle_t s_zb_hop_timer = NULL;
static volatile bool s_zb_running = false;
static bool s_zb_hop_enabled = false;
static uint8_t s_zb_channel = 15U;

/**
 * @brief Zigbee/802.15.4 capture task that drains the ISR queue and writes PCAP packets.
 */
static void poom_pcap_zb_task_(void *arg)
{
    (void)arg;

    while (s_zb_running)
    {
        poom_pcap_zb_item_t item;
        if ((s_zb_q != NULL) && (xQueueReceive(s_zb_q, &item, pdMS_TO_TICKS(100)) == pdTRUE))
        {
            if (item.len > 0U)
            {
                (void)poom_pcap_manager_write_packet(item.data, item.len, POOM_PCAP_CAPTURE_IEEE802154);
            }
        }
    }

    s_zb_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Periodic channel hopping callback (11..26) for 802.15.4 capture.
 */
static void poom_pcap_zb_hop_timer_cb_(TimerHandle_t tmr)
{
    (void)tmr;

    if (!s_zb_running || !s_zb_hop_enabled)
    {
        return;
    }

    if ((s_zb_channel < 11U) || (s_zb_channel > 26U))
    {
        s_zb_channel = 11U;
    }
    else
    {
        s_zb_channel++;
        if (s_zb_channel > 26U)
        {
            s_zb_channel = 11U;
        }
    }

    (void)esp_ieee802154_set_channel(s_zb_channel);
    (void)esp_ieee802154_receive();
}

/**
 * @brief Internal helper for `poom_pcap_zb_isr_consumer`.
 *
 * @param[in] frame Parameter passed to the function.
 * @param[in] frame_info Parameter passed to the function.
 * @param[in] woken Parameter passed to the function.
 * @param[in] user Parameter passed to the function.
 * @return void
 */
static void poom_pcap_zb_isr_consumer_(uint8_t *frame,
                                      esp_ieee802154_frame_info_t *frame_info,
                                      BaseType_t *woken,
                                      void *user)
{
    (void)frame_info;
    (void)user;

    if (frame == NULL)
    {
        return;
    }

    if (!s_zb_running || (s_zb_q == NULL) || (s_mode != POOM_PCAP_SNIFFER_MODE_ZIGBEE))
    {
        return;
    }

    poom_pcap_zb_item_t item = {0};
    uint8_t len = frame[0];

    if (len >= 2U)
    {
        len = (uint8_t)(len - 2U);
    }
    if (len > POOM_PCAP_ZB_MAX_FRAME_LEN)
    {
        len = POOM_PCAP_ZB_MAX_FRAME_LEN;
    }

    item.len = len;
    if (len > 0U)
    {
        memcpy(item.data, frame + 1, len);
    }

    if (woken == NULL)
    {
        return;
    }

    (void)xQueueSendFromISR(s_zb_q, &item, woken);
}

#endif

// =========================
// Public API (manager-style wrappers)
// =========================

esp_err_t poom_pcap_manager_sniffer_start_wifi_capture(uint8_t channel,
                                                       uint32_t filter_mask,
                                                       poom_pcap_wifi_capture_t capture_mode)
{
    if (s_mode != POOM_PCAP_SNIFFER_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (capture_mode >= POOM_PCAP_WIFI_CAPTURE_COUNT)
    {
        capture_mode = POOM_PCAP_WIFI_CAPTURE_RAW;
    }

    esp_err_t ret = poom_pcap_manager_init(NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = poom_pcap_manager_start_auto(POOM_PCAP_CAPTURE_WIFI);
    if (ret != ESP_OK)
    {
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    s_mode = POOM_PCAP_SNIFFER_MODE_WIFI;
    s_wifi_capture = capture_mode;

    ret = poom_pcap_manager_wifi_start_monitor_mode(poom_pcap_sniffer_wifi_promisc_cb_, filter_mask);
    if (ret != ESP_OK)
    {
        s_mode = POOM_PCAP_SNIFFER_MODE_NONE;
        s_wifi_capture = POOM_PCAP_WIFI_CAPTURE_RAW;
        (void)poom_pcap_manager_close();
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    (void)poom_wifi_ctrl_sta_disconnect();
    if (channel != 0U)
    {
        ret = poom_wifi_ctrl_set_channel(channel);
        if (ret != ESP_OK)
        {
            (void)poom_pcap_manager_sniffer_stop();
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t poom_pcap_manager_sniffer_start_wifi(uint8_t channel, uint32_t filter_mask)
{
    return poom_pcap_manager_sniffer_start_wifi_capture(channel, filter_mask, POOM_PCAP_WIFI_CAPTURE_RAW);
}

esp_err_t poom_pcap_manager_sniffer_start_ble(void)
{
    esp_err_t ret;

    if (s_mode != POOM_PCAP_SNIFFER_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ret = poom_pcap_manager_init(NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = poom_pcap_manager_start_auto(POOM_PCAP_CAPTURE_BLUETOOTH);
    if (ret != ESP_OK)
    {
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    poom_ble_scan_set_filter_type(BLE_SCAN_FILTER_ALLOW_ALL);
    poom_ble_scan_set_scan_type(BLE_SCAN_TYPE_ACTIVE);
    poom_ble_scan_register_cb(poom_pcap_sniffer_ble_scan_cb_);
    ret = poom_ble_scan_start();
    if (ret != ESP_OK)
    {
        poom_ble_scan_register_cb(NULL);
        (void)poom_pcap_manager_close();
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    s_mode = POOM_PCAP_SNIFFER_MODE_BLE;

    return ESP_OK;
}

esp_err_t poom_pcap_manager_sniffer_start_zigbee(uint8_t channel, bool enable_hopping, uint32_t hop_ms)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (s_mode != POOM_PCAP_SNIFFER_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = poom_pcap_manager_init(NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = poom_pcap_manager_start_auto(POOM_PCAP_CAPTURE_IEEE802154);
    if (ret != ESP_OK)
    {
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    if (s_zb_q == NULL)
    {
        s_zb_q = xQueueCreate(POOM_PCAP_ZB_QUEUE_LEN, sizeof(poom_pcap_zb_item_t));
        if (s_zb_q == NULL)
        {
            (void)poom_pcap_manager_close();
            (void)poom_pcap_manager_deinit();
            return ESP_ERR_NO_MEM;
        }
    }

    if (!enable_hopping)
    {
        if ((channel < 11U) || (channel > 26U))
        {
            channel = 15U;
        }
    }

    s_mode = POOM_PCAP_SNIFFER_MODE_ZIGBEE;
    s_zb_running = true;
    s_zb_hop_enabled = enable_hopping;
    s_zb_channel = enable_hopping ? 11U : channel;

    if (hop_ms == 0U)
    {
        hop_ms = POOM_PCAP_ZB_HOP_MS_DEFAULT;
    }

    ret = esp_ieee802154_enable();
    if (ret != ESP_OK)
    {
        (void)poom_pcap_manager_sniffer_stop();
        return ret;
    }

    (void)esp_ieee802154_set_coordinator(false);
    (void)esp_ieee802154_set_promiscuous(true);
    (void)esp_ieee802154_set_rx_when_idle(true);
    (void)esp_ieee802154_set_channel(s_zb_channel);

    uint8_t eui64[8] = {0};
    uint8_t eui64_rev[8] = {0};
    (void)esp_read_mac(eui64, ESP_MAC_IEEE802154);
    for (int i = 0; i < 8; i++)
    {
        eui64_rev[7 - i] = eui64[i];
    }
    (void)esp_ieee802154_set_extended_address(eui64_rev);

    if (s_zb_task == NULL)
    {
        (void)xTaskCreate(poom_pcap_zb_task_, "poom_pcap_zb", POOM_PCAP_ZB_TASK_STACK, NULL, POOM_PCAP_ZB_TASK_PRIO, &s_zb_task);
    }

    ret = poom_scanner_core_ieee802154_register_isr_consumer(poom_pcap_zb_isr_consumer_, NULL);
    if (ret != ESP_OK)
    {
        (void)poom_pcap_manager_sniffer_stop();
        return ret;
    }

    (void)esp_ieee802154_receive();

    if (s_zb_hop_enabled)
    {
        if (s_zb_hop_timer == NULL)
        {
            s_zb_hop_timer = xTimerCreate("poom_pcap_zb_hop", pdMS_TO_TICKS(hop_ms), pdTRUE, NULL, poom_pcap_zb_hop_timer_cb_);
        }
        if (s_zb_hop_timer != NULL)
        {
            (void)xTimerStart(s_zb_hop_timer, 0);
        }
    }

    return ESP_OK;
#else
    (void)channel;
    (void)enable_hopping;
    (void)hop_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t poom_pcap_manager_sniffer_stop(void)
{
    if (s_mode == POOM_PCAP_SNIFFER_MODE_NONE)
    {
        return ESP_OK;
    }

    if (s_mode == POOM_PCAP_SNIFFER_MODE_WIFI)
    {
        (void)poom_pcap_manager_wifi_stop_monitor_mode();
        (void)poom_wifi_ctrl_deinit();
        s_wifi_capture = POOM_PCAP_WIFI_CAPTURE_RAW;
    }
    else if (s_mode == POOM_PCAP_SNIFFER_MODE_BLE)
    {
        (void)poom_ble_scan_stop();
        poom_ble_scan_register_cb(NULL);
    }
    else if (s_mode == POOM_PCAP_SNIFFER_MODE_ZIGBEE)
    {
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        s_zb_running = false;
        s_zb_hop_enabled = false;

        poom_scanner_core_ieee802154_unregister_isr_consumer(poom_pcap_zb_isr_consumer_);

        if (s_zb_hop_timer != NULL)
        {
            (void)xTimerStop(s_zb_hop_timer, portMAX_DELAY);
            (void)xTimerDelete(s_zb_hop_timer, portMAX_DELAY);
            s_zb_hop_timer = NULL;
        }

        (void)esp_ieee802154_set_rx_when_idle(false);
        (void)esp_ieee802154_set_promiscuous(false);
        (void)esp_ieee802154_disable();

        if (s_zb_task != NULL)
        {
            TaskHandle_t task = s_zb_task;
            s_zb_task = NULL;
            vTaskDelete(task);
        }

        if (s_zb_q != NULL)
        {
            vQueueDelete(s_zb_q);
            s_zb_q = NULL;
        }
#endif
    }

    s_mode = POOM_PCAP_SNIFFER_MODE_NONE;

    (void)poom_pcap_manager_close();
    (void)poom_pcap_manager_deinit();
    return ESP_OK;
}

bool poom_pcap_manager_sniffer_is_active(void)
{
    return s_mode != POOM_PCAP_SNIFFER_MODE_NONE;
}

esp_err_t poom_pcap_manager_sniffer_zigbee_set_channel(uint8_t channel)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if ((s_mode != POOM_PCAP_SNIFFER_MODE_ZIGBEE) || !s_zb_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_zb_hop_enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if ((channel < POOM_PCAP_IEEE802154_CHANNEL_MIN) || (channel > POOM_PCAP_IEEE802154_CHANNEL_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_zb_channel = channel;
    (void)esp_ieee802154_set_channel(s_zb_channel);
    (void)esp_ieee802154_receive();
    return ESP_OK;
#else
    (void)channel;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint8_t poom_pcap_manager_sniffer_zigbee_get_channel(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if ((s_mode != POOM_PCAP_SNIFFER_MODE_ZIGBEE) || !s_zb_running)
    {
        return 0U;
    }
    return s_zb_channel;
#else
    return 0U;
#endif
}

int8_t poom_pcap_manager_sniffer_zigbee_get_rssi(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (s_mode != POOM_PCAP_SNIFFER_MODE_ZIGBEE)
    {
        return -127;
    }
    return esp_ieee802154_get_recent_rssi();
#else
    return -127;
#endif
}
