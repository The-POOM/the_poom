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
#include <stdio.h>
#include <stdlib.h>
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

static bool poom_pcap_wifi_get_frame_control_(const uint8_t *frame, size_t len, uint16_t *out_fc);
static bool poom_pcap_wifi_get_llc_ethertype_(const uint8_t *frame,
                                              size_t len,
                                              uint16_t *out_ethertype,
                                              size_t *out_payload_off);

#define POOM_PCAP_HANDSHAKE_DIR "/pcaps/handshakes"
#define POOM_PCAP_HANDSHAKE_BASE "handshake"
#define POOM_PCAP_HANDSHAKE_AP_MAX (8U)
#define POOM_PCAP_HANDSHAKE_SESSION_MAX (4U)
#define POOM_PCAP_HANDSHAKE_PMKID_MAX (4U)
#define POOM_PCAP_HANDSHAKE_EAPOL_MAX (256U)
#define POOM_PCAP_ETHERTYPE_EAPOL (0x888EU)
#define POOM_PCAP_EAPOL_TYPE_KEY (0x03U)
#define POOM_PCAP_KEY_INFO_KEY_ACK (0x0080U)
#define POOM_PCAP_KEY_INFO_INSTALL (0x0040U)
#define POOM_PCAP_KEY_INFO_KEY_MIC (0x0100U)
#define POOM_PCAP_KEY_INFO_ENCRYPTED_KEY_DATA (0x1000U)

typedef struct
{
    bool used;
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
} poom_pcap_handshake_ap_t;

typedef struct
{
    bool used;
    uint8_t keymic[16];
    uint16_t eapol_len;
    uint8_t eapol[POOM_PCAP_HANDSHAKE_EAPOL_MAX];
} poom_pcap_handshake_eapol_t;

typedef struct
{
    bool used;
    uint8_t mac_ap[6];
    uint8_t mac_sta[6];
    uint8_t essid_len;
    uint8_t essid[32];
    uint8_t message_mask;
    uint8_t message_pair;
    uint8_t keyver;
    uint8_t nonce_ap[32];
    uint8_t nonce_sta[32];
    poom_pcap_handshake_eapol_t msg2;
    poom_pcap_handshake_eapol_t msg4;
    uint64_t replay_counter[4];
} poom_pcap_handshake_session_t;

typedef struct
{
    bool used;
    uint8_t pmkid[16];
    uint8_t mac_ap[6];
    uint8_t mac_sta[6];
    uint8_t essid_len;
    uint8_t essid[32];
} poom_pcap_handshake_pmkid_t;

typedef struct
{
    poom_pcap_handshake_ap_t aps[POOM_PCAP_HANDSHAKE_AP_MAX];
    poom_pcap_handshake_session_t sessions[POOM_PCAP_HANDSHAKE_SESSION_MAX];
    poom_pcap_handshake_pmkid_t pmkids[POOM_PCAP_HANDSHAKE_PMKID_MAX];
    uint8_t pmkid_count;
    uint8_t valid_pair_count;
    bool has_beacon;
    bool hash22000_saved;
} poom_pcap_handshake_state_t;

static poom_pcap_handshake_state_t *s_hs = NULL;
static poom_pcap_wifi_handshake_status_t s_hs_status = {0};

static bool poom_pcap_mem_nonzero_(const uint8_t *data, size_t len)
{
    if (data == NULL)
    {
        return false;
    }
    for (size_t i = 0U; i < len; i++)
    {
        if (data[i] != 0U)
        {
            return true;
        }
    }
    return false;
}

static uint64_t poom_pcap_read_be64_(const uint8_t *data)
{
    uint64_t value = 0U;
    if (data == NULL)
    {
        return 0U;
    }
    for (size_t i = 0U; i < 8U; i++)
    {
        value = (value << 8U) | (uint64_t)data[i];
    }
    return value;
}

static const poom_pcap_handshake_eapol_t *poom_pcap_handshake_export_eapol_(const poom_pcap_handshake_session_t *session)
{
    if (session == NULL)
    {
        return NULL;
    }

    if ((session->message_pair == 0U) || (session->message_pair == 2U))
    {
        return &session->msg2;
    }
    if ((session->message_pair == 1U) || (session->message_pair == 5U))
    {
        return &session->msg4;
    }

    return NULL;
}

static bool poom_pcap_handshake_eapol_valid_(const poom_pcap_handshake_eapol_t *saved)
{
    return (saved != NULL) && saved->used && (saved->eapol_len > 0U) &&
           (saved->eapol_len <= POOM_PCAP_HANDSHAKE_EAPOL_MAX) &&
           poom_pcap_mem_nonzero_(saved->keymic, 16U);
}

static bool poom_pcap_handshake_session_valid_(const poom_pcap_handshake_session_t *session)
{
    if ((session == NULL) || !session->used || (session->message_pair == 255U) || (session->essid_len == 0U))
    {
        return false;
    }

    return poom_pcap_mem_nonzero_(session->mac_ap, 6U) &&
           poom_pcap_mem_nonzero_(session->mac_sta, 6U) &&
           poom_pcap_mem_nonzero_(session->nonce_ap, 32U) &&
           poom_pcap_mem_nonzero_(session->nonce_sta, 32U) &&
           poom_pcap_handshake_eapol_valid_(poom_pcap_handshake_export_eapol_(session));
}

static void poom_pcap_handshake_status_update_(void)
{
    memset(&s_hs_status, 0, sizeof(s_hs_status));
    if (s_hs == NULL)
    {
        return;
    }

    s_hs_status.pmkid_count = s_hs->pmkid_count;
    s_hs_status.has_beacon = s_hs->has_beacon;
    s_hs_status.hash22000_saved = s_hs->hash22000_saved;

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_SESSION_MAX; i++)
    {
        const poom_pcap_handshake_session_t *session = &s_hs->sessions[i];
        if (!session->used)
        {
            continue;
        }
        s_hs_status.eapol_message_mask |= session->message_mask;
        if (poom_pcap_handshake_session_valid_(session))
        {
            s_hs_status.valid_pair_count++;
        }
    }

    s_hs_status.complete = (s_hs_status.valid_pair_count > 0U) || (s_hs_status.pmkid_count > 0U);
}

static void poom_pcap_handshake_free_(void)
{
    if (s_hs != NULL)
    {
        free(s_hs);
        s_hs = NULL;
    }
}

static esp_err_t poom_pcap_handshake_reset_(void)
{
    poom_pcap_handshake_free_();
    memset(&s_hs_status, 0, sizeof(s_hs_status));

    s_hs = calloc(1U, sizeof(*s_hs));
    if (s_hs == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_SESSION_MAX; i++)
    {
        s_hs->sessions[i].message_pair = 255U;
        s_hs->sessions[i].keyver = 2U;
    }
    poom_pcap_handshake_status_update_();
    return ESP_OK;
}

static bool poom_pcap_wifi_get_header_len_(const uint8_t *frame, size_t len, uint16_t fc, size_t *out_hdr_len)
{
    if ((frame == NULL) || (out_hdr_len == NULL) || (len < 24U))
    {
        return false;
    }

    const uint8_t type = (uint8_t)((fc >> 2) & 0x03U);
    const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0FU);
    const bool to_ds = ((fc >> 8) & 0x01U) != 0U;
    const bool from_ds = ((fc >> 9) & 0x01U) != 0U;
    const bool qos = (subtype & 0x08U) != 0U;

    if (type != 2U)
    {
        return false;
    }

    size_t hdr_len = (to_ds && from_ds) ? 30U : 24U;
    if (qos)
    {
        hdr_len += 2U;
    }
    if (len < hdr_len)
    {
        return false;
    }

    *out_hdr_len = hdr_len;
    return true;
}

static bool poom_pcap_handshake_lookup_ssid_(const uint8_t *bssid, uint8_t *out_ssid, uint8_t *out_len)
{
    if ((s_hs == NULL) || (bssid == NULL) || (out_ssid == NULL) || (out_len == NULL))
    {
        return false;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_AP_MAX; i++)
    {
        if (s_hs->aps[i].used && memcmp(s_hs->aps[i].bssid, bssid, 6U) == 0)
        {
            *out_len = s_hs->aps[i].ssid_len;
            if (*out_len > 0U)
            {
                memcpy(out_ssid, s_hs->aps[i].ssid, *out_len);
            }
            return *out_len > 0U;
        }
    }

    return false;
}

static void poom_pcap_handshake_attach_ssid_(const uint8_t *bssid, const uint8_t *ssid, uint8_t ssid_len)
{
    if ((s_hs == NULL) || (bssid == NULL) || (ssid == NULL) || (ssid_len == 0U) || (ssid_len > 32U))
    {
        return;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_SESSION_MAX; i++)
    {
        poom_pcap_handshake_session_t *session = &s_hs->sessions[i];
        if (session->used && (session->essid_len == 0U) && (memcmp(session->mac_ap, bssid, 6U) == 0))
        {
            memcpy(session->essid, ssid, ssid_len);
            session->essid_len = ssid_len;
        }
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_PMKID_MAX; i++)
    {
        poom_pcap_handshake_pmkid_t *pmkid = &s_hs->pmkids[i];
        if (pmkid->used && (pmkid->essid_len == 0U) && (memcmp(pmkid->mac_ap, bssid, 6U) == 0))
        {
            memcpy(pmkid->essid, ssid, ssid_len);
            pmkid->essid_len = ssid_len;
        }
    }
}

static void poom_pcap_handshake_store_ssid_(const uint8_t *bssid, const uint8_t *ssid, uint8_t ssid_len)
{
    if ((s_hs == NULL) || (bssid == NULL) || (ssid == NULL) || (ssid_len == 0U) || (ssid_len > 32U))
    {
        return;
    }

    poom_pcap_handshake_ap_t *slot = NULL;
    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_AP_MAX; i++)
    {
        if (s_hs->aps[i].used && memcmp(s_hs->aps[i].bssid, bssid, 6U) == 0)
        {
            slot = &s_hs->aps[i];
            break;
        }
        if ((slot == NULL) && !s_hs->aps[i].used)
        {
            slot = &s_hs->aps[i];
        }
    }

    if (slot == NULL)
    {
        return;
    }

    slot->used = true;
    memcpy(slot->bssid, bssid, 6U);
    memcpy(slot->ssid, ssid, ssid_len);
    slot->ssid_len = ssid_len;
    s_hs->has_beacon = true;
    poom_pcap_handshake_attach_ssid_(bssid, ssid, ssid_len);
    poom_pcap_handshake_status_update_();
}

static void poom_pcap_handshake_parse_beacon_(const uint8_t *frame, size_t len)
{
    uint16_t fc = 0U;
    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc))
    {
        return;
    }

    const uint8_t type = (uint8_t)((fc >> 2) & 0x03U);
    const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0FU);
    if ((type != 0U) || ((subtype != 8U) && (subtype != 5U)) || (len < 36U))
    {
        return;
    }

    const uint8_t *bssid = &frame[16];
    size_t pos = 36U;
    while (pos + 2U <= len)
    {
        const uint8_t tag = frame[pos];
        const uint8_t tag_len = frame[pos + 1U];
        pos += 2U;
        if (pos + tag_len > len)
        {
            return;
        }
        if ((tag == 0U) && (tag_len > 0U) && (tag_len <= 32U))
        {
            poom_pcap_handshake_store_ssid_(bssid, &frame[pos], tag_len);
            return;
        }
        pos += tag_len;
    }
}

static bool poom_pcap_handshake_get_addrs_(const uint8_t *frame,
                                            size_t len,
                                            uint8_t *out_ap,
                                            uint8_t *out_sta,
                                            bool *out_from_ap)
{
    uint16_t fc = 0U;
    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc) || (len < 24U))
    {
        return false;
    }

    const bool to_ds = ((fc >> 8) & 0x01U) != 0U;
    const bool from_ds = ((fc >> 9) & 0x01U) != 0U;
    const uint8_t *addr1 = &frame[4];
    const uint8_t *addr2 = &frame[10];
    const uint8_t *addr3 = &frame[16];

    if (to_ds && !from_ds)
    {
        memcpy(out_ap, addr1, 6U);
        memcpy(out_sta, addr2, 6U);
        *out_from_ap = false;
        return true;
    }
    if (!to_ds && from_ds)
    {
        memcpy(out_ap, addr2, 6U);
        memcpy(out_sta, addr1, 6U);
        *out_from_ap = true;
        return true;
    }
    if (!to_ds && !from_ds)
    {
        memcpy(out_ap, addr3, 6U);
        if (memcmp(addr2, addr3, 6U) == 0)
        {
            memcpy(out_sta, addr1, 6U);
            *out_from_ap = true;
        }
        else
        {
            memcpy(out_sta, addr2, 6U);
            *out_from_ap = false;
        }
        return true;
    }

    return false;
}

static uint8_t poom_pcap_handshake_eapol_message_(uint16_t key_info, const uint8_t *nonce)
{
    const bool ack = (key_info & POOM_PCAP_KEY_INFO_KEY_ACK) != 0U;
    const bool install = (key_info & POOM_PCAP_KEY_INFO_INSTALL) != 0U;
    const bool mic = (key_info & POOM_PCAP_KEY_INFO_KEY_MIC) != 0U;
    const bool nonce_present = poom_pcap_mem_nonzero_(nonce, 32U);

    if (ack && !install && !mic)
    {
        return 1U;
    }
    if (!ack && mic && nonce_present)
    {
        return 2U;
    }
    if (ack && install && mic)
    {
        return 3U;
    }
    if (!ack && mic && !nonce_present)
    {
        return 4U;
    }
    return 0U;
}

static bool poom_pcap_handshake_has_msg_(const poom_pcap_handshake_session_t *session, uint8_t msg)
{
    if ((session == NULL) || (msg == 0U) || (msg > 4U))
    {
        return false;
    }
    return (session->message_mask & (uint8_t)(1U << (msg - 1U))) != 0U;
}

static bool poom_pcap_handshake_replay_pair_(const poom_pcap_handshake_session_t *session, uint8_t first_msg, uint8_t second_msg)
{
    if (!poom_pcap_handshake_has_msg_(session, first_msg) || !poom_pcap_handshake_has_msg_(session, second_msg))
    {
        return false;
    }

    const uint64_t first = session->replay_counter[first_msg - 1U];
    const uint64_t second = session->replay_counter[second_msg - 1U];

    if (((first_msg == 1U) && (second_msg == 2U)) || ((first_msg == 3U) && (second_msg == 4U)))
    {
        return first == second;
    }
    if ((first_msg == 2U) && (second_msg == 3U))
    {
        return (first + 1U) == second;
    }
    if ((first_msg == 1U) && (second_msg == 4U))
    {
        return (first + 1U) == second;
    }

    return false;
}

static void poom_pcap_handshake_update_pair_(poom_pcap_handshake_session_t *session)
{
    if (session == NULL)
    {
        return;
    }

    session->message_pair = 255U;
    if (poom_pcap_handshake_replay_pair_(session, 1U, 2U))
    {
        session->message_pair = 0U;
    }
    else if (poom_pcap_handshake_replay_pair_(session, 2U, 3U))
    {
        session->message_pair = 2U;
    }
    else if (poom_pcap_handshake_replay_pair_(session, 3U, 4U))
    {
        session->message_pair = 5U;
    }
    else if (poom_pcap_handshake_replay_pair_(session, 1U, 4U))
    {
        session->message_pair = 1U;
    }
}

static poom_pcap_handshake_session_t *poom_pcap_handshake_session_get_(const uint8_t *ap, const uint8_t *sta)
{
    if ((s_hs == NULL) || (ap == NULL) || (sta == NULL))
    {
        return NULL;
    }

    poom_pcap_handshake_session_t *empty = NULL;
    poom_pcap_handshake_session_t *replace = NULL;
    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_SESSION_MAX; i++)
    {
        poom_pcap_handshake_session_t *session = &s_hs->sessions[i];
        if (session->used && (memcmp(session->mac_ap, ap, 6U) == 0) && (memcmp(session->mac_sta, sta, 6U) == 0))
        {
            return session;
        }
        if ((empty == NULL) && !session->used)
        {
            empty = session;
        }
        if ((replace == NULL) ||
            (poom_pcap_handshake_session_valid_(replace) && !poom_pcap_handshake_session_valid_(session)) ||
            (session->message_mask < replace->message_mask))
        {
            replace = session;
        }
    }

    poom_pcap_handshake_session_t *session = (empty != NULL) ? empty : replace;
    if (session == NULL)
    {
        return NULL;
    }

    memset(session, 0, sizeof(*session));
    session->used = true;
    session->message_pair = 255U;
    session->keyver = 2U;
    memcpy(session->mac_ap, ap, 6U);
    memcpy(session->mac_sta, sta, 6U);
    (void)poom_pcap_handshake_lookup_ssid_(ap, session->essid, &session->essid_len);
    return session;
}

static void poom_pcap_handshake_save_eapol_(poom_pcap_handshake_session_t *session,
                                            uint8_t msg,
                                            const uint8_t *eapol,
                                            size_t eapol_len,
                                            const uint8_t *mic)
{
    if ((session == NULL) || (eapol == NULL) || (mic == NULL) ||
        (eapol_len == 0U) || (eapol_len > POOM_PCAP_HANDSHAKE_EAPOL_MAX))
    {
        return;
    }

    poom_pcap_handshake_eapol_t *saved = NULL;
    if (msg == 2U)
    {
        saved = &session->msg2;
    }
    else if (msg == 4U)
    {
        saved = &session->msg4;
    }
    else
    {
        return;
    }

    saved->used = true;
    saved->eapol_len = (uint16_t)eapol_len;
    memcpy(saved->eapol, eapol, eapol_len);
    memcpy(saved->keymic, mic, 16U);
    if (eapol_len >= 97U)
    {
        memset(&saved->eapol[81], 0, 16U);
    }
}

static bool poom_pcap_handshake_pmkid_exists_(const uint8_t *pmkid, const uint8_t *ap, const uint8_t *sta)
{
    if ((s_hs == NULL) || (pmkid == NULL) || (ap == NULL) || (sta == NULL))
    {
        return true;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_PMKID_MAX; i++)
    {
        const poom_pcap_handshake_pmkid_t *entry = &s_hs->pmkids[i];
        if (entry->used && (memcmp(entry->pmkid, pmkid, 16U) == 0) &&
            (memcmp(entry->mac_ap, ap, 6U) == 0) && (memcmp(entry->mac_sta, sta, 6U) == 0))
        {
            return true;
        }
    }
    return false;
}

static void poom_pcap_handshake_store_pmkid_(const uint8_t *pmkid, const uint8_t *ap, const uint8_t *sta)
{
    if ((s_hs == NULL) || (pmkid == NULL) || (ap == NULL) || (sta == NULL) ||
        poom_pcap_handshake_pmkid_exists_(pmkid, ap, sta))
    {
        return;
    }

    poom_pcap_handshake_pmkid_t *slot = NULL;
    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_PMKID_MAX; i++)
    {
        if (!s_hs->pmkids[i].used)
        {
            slot = &s_hs->pmkids[i];
            break;
        }
    }
    if (slot == NULL)
    {
        return;
    }

    slot->used = true;
    memcpy(slot->pmkid, pmkid, 16U);
    memcpy(slot->mac_ap, ap, 6U);
    memcpy(slot->mac_sta, sta, 6U);
    (void)poom_pcap_handshake_lookup_ssid_(ap, slot->essid, &slot->essid_len);
    s_hs->pmkid_count++;
    poom_pcap_handshake_status_update_();
}

static void poom_pcap_handshake_parse_pmkid_(const uint8_t *key_data,
                                             size_t key_data_len,
                                             const uint8_t *ap,
                                             const uint8_t *sta)
{
    if ((key_data == NULL) || (ap == NULL) || (sta == NULL) || (key_data_len < 22U))
    {
        return;
    }

    size_t pos = 0U;
    while (pos + 2U <= key_data_len)
    {
        const uint8_t tag = key_data[pos];
        const uint8_t tag_len = key_data[pos + 1U];
        pos += 2U;
        if (pos + tag_len > key_data_len)
        {
            break;
        }

        if ((tag == 0xDDU) && (tag_len >= 20U) &&
            (key_data[pos] == 0x00U) && (key_data[pos + 1U] == 0x0FU) &&
            (key_data[pos + 2U] == 0xACU) && (key_data[pos + 3U] == 0x04U))
        {
            poom_pcap_handshake_store_pmkid_(&key_data[pos + 4U], ap, sta);
            return;
        }

        pos += tag_len;
    }
}

static void poom_pcap_handshake_parse_eapol_(const uint8_t *frame, size_t len)
{
    if (s_hs == NULL)
    {
        return;
    }

    uint16_t fc = 0U;
    uint16_t ethertype = 0U;
    size_t off = 0U;
    size_t hdr_len = 0U;
    uint8_t ap[6] = {0};
    uint8_t sta[6] = {0};
    bool from_ap = false;

    if (!poom_pcap_wifi_get_frame_control_(frame, len, &fc) ||
        !poom_pcap_wifi_get_header_len_(frame, len, fc, &hdr_len) ||
        !poom_pcap_wifi_get_llc_ethertype_(frame, len, &ethertype, &off) ||
        (ethertype != POOM_PCAP_ETHERTYPE_EAPOL) ||
        !poom_pcap_handshake_get_addrs_(frame, len, ap, sta, &from_ap))
    {
        return;
    }

    const uint8_t *eapol = &frame[off];
    const size_t eapol_avail = len - off;
    if (eapol_avail < 99U || eapol[1] != POOM_PCAP_EAPOL_TYPE_KEY)
    {
        return;
    }

    const size_t eapol_body_len = ((size_t)eapol[2] << 8U) | (size_t)eapol[3];
    size_t eapol_len = 4U + eapol_body_len;
    if (eapol_len > eapol_avail)
    {
        eapol_len = eapol_avail;
    }
    if (eapol_len > POOM_PCAP_HANDSHAKE_EAPOL_MAX)
    {
        eapol_len = POOM_PCAP_HANDSHAKE_EAPOL_MAX;
    }

    const uint8_t *key = &eapol[4];
    const uint16_t key_info = ((uint16_t)key[1] << 8U) | (uint16_t)key[2];
    const uint8_t *nonce = &key[13];
    const uint8_t *mic = &key[77];
    const uint64_t replay_counter = poom_pcap_read_be64_(&key[5]);
    const uint8_t msg = poom_pcap_handshake_eapol_message_(key_info, nonce);

    if ((msg == 0U) || (msg > 4U))
    {
        return;
    }

    poom_pcap_handshake_session_t *session = poom_pcap_handshake_session_get_(ap, sta);
    if (session == NULL)
    {
        return;
    }

    session->message_mask |= (uint8_t)(1U << (msg - 1U));
    session->replay_counter[msg - 1U] = replay_counter;
    session->keyver = ((key_info & 0x0007U) == 1U) ? 1U : 2U;

    if (session->essid_len == 0U)
    {
        (void)poom_pcap_handshake_lookup_ssid_(ap, session->essid, &session->essid_len);
    }

    if (from_ap && ((msg == 1U) || (msg == 3U)))
    {
        memcpy(session->nonce_ap, nonce, 32U);
    }
    else if (!from_ap && (msg == 2U))
    {
        memcpy(session->nonce_sta, nonce, 32U);
    }

    if (((key_info & POOM_PCAP_KEY_INFO_KEY_MIC) != 0U) && ((msg == 2U) || (msg == 4U)))
    {
        poom_pcap_handshake_save_eapol_(session, msg, eapol, eapol_len, mic);
    }

    if ((key_info & POOM_PCAP_KEY_INFO_ENCRYPTED_KEY_DATA) == 0U && eapol_len >= 99U)
    {
        const size_t key_data_len_off = 4U + 95U;
        if (key_data_len_off + 2U <= eapol_len)
        {
            const size_t key_data_len = ((size_t)eapol[key_data_len_off] << 8U) | (size_t)eapol[key_data_len_off + 1U];
            const size_t key_data_off = key_data_len_off + 2U;
            if (key_data_off + key_data_len <= eapol_len)
            {
                poom_pcap_handshake_parse_pmkid_(&eapol[key_data_off], key_data_len, ap, sta);
            }
        }
    }

    poom_pcap_handshake_update_pair_(session);
    poom_pcap_handshake_status_update_();
}

static bool poom_pcap_replace_ext_(const char *src, const char *ext, char *out, size_t out_len)
{
    if ((src == NULL) || (ext == NULL) || (out == NULL) || (out_len == 0U))
    {
        return false;
    }

    int written = snprintf(out, out_len, "%s", src);
    if ((written < 0) || ((size_t)written >= out_len))
    {
        return false;
    }

    char *dot = strrchr(out, '.');
    if (dot == NULL)
    {
        return false;
    }

    const size_t prefix_len = (size_t)(dot - out);
    written = snprintf(&out[prefix_len], out_len - prefix_len, "%s", ext);
    return (written >= 0) && ((size_t)written < (out_len - prefix_len));
}

static void poom_pcap_hex_write_(FILE *file, const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    if ((file == NULL) || (data == NULL))
    {
        return;
    }
    for (size_t i = 0U; i < len; i++)
    {
        (void)fputc(hex[data[i] >> 4], file);
        (void)fputc(hex[data[i] & 0x0FU], file);
    }
}

static bool poom_pcap_handshake_write_22000_(FILE *file)
{
    bool wrote = false;
    if ((file == NULL) || (s_hs == NULL))
    {
        return false;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_PMKID_MAX; i++)
    {
        const poom_pcap_handshake_pmkid_t *pmkid = &s_hs->pmkids[i];
        if (!pmkid->used || (pmkid->essid_len == 0U))
        {
            continue;
        }

        (void)fputs("WPA*01*", file);
        poom_pcap_hex_write_(file, pmkid->pmkid, 16U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, pmkid->mac_ap, 6U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, pmkid->mac_sta, 6U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, pmkid->essid, pmkid->essid_len);
        (void)fputs("***\n", file);
        wrote = true;
    }

    for (size_t i = 0U; i < POOM_PCAP_HANDSHAKE_SESSION_MAX; i++)
    {
        const poom_pcap_handshake_session_t *session = &s_hs->sessions[i];
        if (!poom_pcap_handshake_session_valid_(session))
        {
            continue;
        }

        const poom_pcap_handshake_eapol_t *saved = poom_pcap_handshake_export_eapol_(session);
        if (!poom_pcap_handshake_eapol_valid_(saved))
        {
            continue;
        }

        (void)fputs("WPA*02*", file);
        poom_pcap_hex_write_(file, saved->keymic, 16U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, session->mac_ap, 6U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, session->mac_sta, 6U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, session->essid, session->essid_len);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, session->nonce_ap, 32U);
        (void)fputc('*', file);
        poom_pcap_hex_write_(file, saved->eapol, saved->eapol_len);
        (void)fprintf(file, "*%02x\n", (unsigned)session->message_pair);
        wrote = true;
    }

    return wrote;
}

static void poom_pcap_handshake_finalize_(void)
{
    if (s_hs == NULL)
    {
        return;
    }

    const char *pcap_path = poom_pcap_manager_get_file_path();
    char out_path[128];

    (void)poom_pcap_manager_flush();
    if (pcap_path == NULL)
    {
        return;
    }

    if (poom_pcap_replace_ext_(pcap_path, ".22000", out_path, sizeof(out_path)))
    {
        FILE *file = fopen(out_path, "w");
        if (file != NULL)
        {
            s_hs->hash22000_saved = poom_pcap_handshake_write_22000_(file);
            (void)fclose(file);
        }
    }

    poom_pcap_handshake_status_update_();
}

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
        case POOM_PCAP_WIFI_CAPTURE_HANDSHAKE:
            return poom_pcap_wifi_is_mgmt_subtype_(frame, len, 8U) ||
                   poom_pcap_wifi_is_mgmt_subtype_(frame, len, 5U) ||
                   poom_pcap_wifi_is_eapol_any_(frame, len);
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

    if (s_wifi_capture == POOM_PCAP_WIFI_CAPTURE_HANDSHAKE)
    {
        poom_pcap_handshake_parse_beacon_(pkt->payload, len);
        poom_pcap_handshake_parse_eapol_(pkt->payload, len);
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

    if (capture_mode == POOM_PCAP_WIFI_CAPTURE_HANDSHAKE)
    {
        ret = poom_pcap_handshake_reset_();
        if (ret == ESP_OK)
        {
            ret = poom_pcap_manager_start_file(POOM_PCAP_HANDSHAKE_BASE,
                                               POOM_PCAP_HANDSHAKE_DIR,
                                               POOM_PCAP_CAPTURE_WIFI,
                                               true);
        }
    }
    else
    {
        ret = poom_pcap_manager_start_auto(POOM_PCAP_CAPTURE_WIFI);
    }
    if (ret != ESP_OK)
    {
        if (capture_mode == POOM_PCAP_WIFI_CAPTURE_HANDSHAKE)
        {
            poom_pcap_handshake_free_();
        }
        (void)poom_pcap_manager_deinit();
        return ret;
    }

    s_mode = POOM_PCAP_SNIFFER_MODE_WIFI;
    s_wifi_capture = capture_mode;

    ret = poom_pcap_manager_wifi_start_monitor_mode(poom_pcap_sniffer_wifi_promisc_cb_, filter_mask);
    if (ret != ESP_OK)
    {
        s_mode = POOM_PCAP_SNIFFER_MODE_NONE;
        if (s_wifi_capture == POOM_PCAP_WIFI_CAPTURE_HANDSHAKE)
        {
            poom_pcap_handshake_free_();
        }
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
        if (s_wifi_capture == POOM_PCAP_WIFI_CAPTURE_HANDSHAKE)
        {
            poom_pcap_handshake_finalize_();
            poom_pcap_handshake_free_();
        }
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


bool poom_pcap_manager_wifi_handshake_get_status(poom_pcap_wifi_handshake_status_t *out_status)
{
    if (out_status == NULL)
    {
        return false;
    }

    *out_status = s_hs_status;
    return true;
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
