// SPDX-License-Identifier: GPL-3.0-or-later
/* Native bounded AD decoder. Identifier/name rules adapted from JustCallMeKoko
 * ESP32 Marauder ef9a4bbb; see MARAUDER_PASSIVE_BLE.md for provenance. */
#ifndef POOM_BLE_RECON_PARSER_H
#define POOM_BLE_RECON_PARSER_H

#include "poom_ble_detector.h"
#include <string.h>

typedef struct {
    bool meta;
    bool blocked;
    bool xuntong;
    bool name_present;
    bool complete_name;
    size_t name_length;
    char name[POOM_BLE_DETECTOR_NAME_LEN];
} poom_ble_recon_parse_state_t;

static uint16_t poom_ble_recon_le16_(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static void poom_ble_recon_identifier_(poom_ble_recon_parse_state_t *s,
                                       poom_ble_detector_record_t *r, uint16_t id)
{
    switch(id) {
    case 0xFD5F: case 0xFEB7: case 0xFEB8: case 0x01AB: case 0x058E: case 0x0D53:
        s->meta = true;
        if(!(r->signatures & POOM_BLE_SIGNATURE_FLIPPER)) r->identifier = id;
        break;
    case 0xFD5A: case 0xFD69: case 0x004C: case 0x0006: case 0xFEF3:
        s->blocked = true; break;
    default: break;
    }
}

static void poom_ble_recon_uuid16_(poom_ble_recon_parse_state_t *s,
                                   poom_ble_detector_record_t *r, uint16_t uuid)
{
    poom_ble_recon_identifier_(s, r, uuid);
    if(uuid == 0xFD44U) r->signatures |= POOM_BLE_SIGNATURE_FMNA;
    if(uuid == 0xFCB2U) r->signatures |= POOM_BLE_SIGNATURE_LEGACY_FINDMY;
    if(uuid >= 0x3081U && uuid <= 0x3083U) {
        r->signatures |= POOM_BLE_SIGNATURE_FLIPPER;
        r->identifier = uuid;
    }
}

static void poom_ble_recon_uuid128_(poom_ble_recon_parse_state_t *s,
                                    poom_ble_detector_record_t *r, const uint8_t *uuid)
{
    /* BLE AD encodes UUIDs least-significant byte first. */
    static const uint8_t dult[16] = {
        0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,0x26,0xc2,0xf4,0x12,0x01,0x00,0x19,0x15
    };
    static const uint8_t base[12] = {
        0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00
    };
    if(memcmp(uuid, dult, sizeof(dult)) == 0) r->signatures |= POOM_BLE_SIGNATURE_DULT;
    if(memcmp(uuid, base, sizeof(base)) == 0 && uuid[14] == 0U && uuid[15] == 0U)
        poom_ble_recon_uuid16_(s, r, poom_ble_recon_le16_(uuid + 12U));
}

static void poom_ble_recon_serial_(poom_ble_detector_record_t *r,
                                   const uint8_t *data, size_t length)
{
    for(size_t i = 0U; i + 2U < length; ++i) {
        if(data[i] != 'T' || data[i + 1U] != 'N') continue;
        size_t at = 2U;
        r->flock_serial[0] = 'T'; r->flock_serial[1] = 'N';
        for(size_t j = i + 2U; j < length && at + 1U < sizeof(r->flock_serial); ++j) {
            if(data[j] >= '0' && data[j] <= '9') r->flock_serial[at++] = (char)data[j];
            else if(data[j] != ' ' && data[j] != '#' && data[j] != '-') break;
        }
        r->flock_serial[at] = '\0';
        if(at > 2U) return;
        r->flock_serial[0] = '\0';
    }
}

static void poom_ble_recon_ad_(poom_ble_recon_parse_state_t *s,
                              poom_ble_detector_record_t *r, const uint8_t *p, size_t length)
{
    if(!p) return;
    for(size_t at = 0U; at < length;) {
        const size_t field = p[at];
        if(field == 0U || field > length - at - 1U) break;
        const uint8_t type = p[at + 1U];
        const uint8_t *data = p + at + 2U;
        const size_t n = field - 1U;
        if((type == 0x08U || type == 0x09U) && (type == 0x09U || !s->complete_name)) {
            const size_t copy = n < sizeof(s->name) ? n : sizeof(s->name) - 1U;
            memcpy(s->name, data, copy); s->name[copy] = '\0';
            s->name_length = n; s->name_present = n > 0U;
            s->complete_name = type == 0x09U;
        } else if(type == 0xFFU && n >= 2U) {
            const uint16_t company = poom_ble_recon_le16_(data);
            poom_ble_recon_identifier_(s, r, company);
            if(company == 0x004CU && n >= 29U && data[2] == 0x12U && data[3] == 0x19U)
                r->signatures |= POOM_BLE_SIGNATURE_FINDMY;
            if(company == 0x004CU && n >= 25U && data[2] == 0x02U && data[3] == 0x15U) {
                r->signatures |= POOM_BLE_SIGNATURE_IBEACON;
                memcpy(r->ibeacon_uuid, data + 4U, 16U);
                r->ibeacon_major = ((uint16_t)data[20] << 8U) | data[21];
                r->ibeacon_minor = ((uint16_t)data[22] << 8U) | data[23];
                r->ibeacon_power = (int8_t)data[24];
            }
            if(company == 0x09C8U) { s->xuntong = true; poom_ble_recon_serial_(r, data + 2U, n - 2U); }
        } else if(type == 0x02U || type == 0x03U || type == 0x16U) {
            const size_t limit = type == 0x16U ? (n >= 2U ? 2U : 0U) : n;
            for(size_t i = 0U; i + 2U <= limit; i += 2U)
                poom_ble_recon_uuid16_(s, r, poom_ble_recon_le16_(data + i));
        } else if(type == 0x04U || type == 0x05U || type == 0x20U) {
            const size_t limit = type == 0x20U ? (n >= 4U ? 4U : 0U) : n;
            for(size_t i = 0U; i + 4U <= limit; i += 4U) {
                if(data[i + 2U] == 0U && data[i + 3U] == 0U)
                    poom_ble_recon_uuid16_(s, r, poom_ble_recon_le16_(data + i));
            }
        } else if(type == 0x06U || type == 0x07U || type == 0x21U) {
            const size_t limit = type == 0x21U ? (n >= 16U ? 16U : 0U) : n;
            for(size_t i = 0U; i + 16U <= limit; i += 16U)
                poom_ble_recon_uuid128_(s, r, data + i);
        }
        at += field + 1U;
    }
}

static bool poom_ble_recon_digits_(const char *name, size_t length)
{
    for(size_t i = 0U; i < length; ++i) if(name[i] < '0' || name[i] > '9') return false;
    return true;
}

static void poom_ble_recon_decode_parts_(const uint8_t *adv, size_t adv_len,
                                         const uint8_t *response, size_t response_len,
                                         poom_ble_detector_record_t *r)
{
    poom_ble_recon_parse_state_t s = {0};
    r->signatures = 0U; r->identifier = 0U;
    memset(r->ibeacon_uuid, 0, sizeof(r->ibeacon_uuid));
    r->ibeacon_major = r->ibeacon_minor = 0U; r->ibeacon_power = 0;
    r->flock_serial[0] = '\0';
    poom_ble_recon_ad_(&s, r, adv, adv_len);
    poom_ble_recon_ad_(&s, r, response, response_len);
    if(s.meta && !s.blocked) r->signatures |= POOM_BLE_SIGNATURE_META;
    const bool penguin = (s.name_length == 18U && memcmp(s.name, "Penguin-", 8U) == 0 &&
                          poom_ble_recon_digits_(s.name + 8U, 10U)) ||
                         (s.name_length == 14U && memcmp(s.name, "FS Ext Battery", 14U) == 0) ||
                         (s.name_length == 10U && poom_ble_recon_digits_(s.name, 10U));
    if(s.xuntong && (penguin || !s.name_present)) r->signatures |= POOM_BLE_SIGNATURE_FLOCK;
    else r->flock_serial[0] = '\0';
    if(strstr(s.name, "HC-03") || strstr(s.name, "HC-05") || strstr(s.name, "HC-06"))
        r->signatures |= POOM_BLE_SIGNATURE_HC_NAME;
    if(s.name_present) {
        memcpy(r->name, s.name, sizeof(r->name));
        for(size_t i = 0U; i < sizeof(r->name) && r->name[i]; ++i)
            if((unsigned char)r->name[i] < 32U || (unsigned char)r->name[i] > 126U) r->name[i] = '?';
    } else {
        const char *name = NULL;
        if(r->signatures & POOM_BLE_SIGNATURE_IBEACON) name = "iBeacon";
        else if(r->signatures & POOM_BLE_FINDMY_SIGNATURES) name = "FindMy candidate";
        else if(r->signatures & POOM_BLE_SIGNATURE_FLIPPER) name = "Flipper candidate";
        else if(r->signatures & POOM_BLE_SIGNATURE_FLOCK) name = "Flock candidate";
        else if(r->signatures & POOM_BLE_SIGNATURE_META) name = "Meta candidate";
        if(name) { strncpy(r->name, name, sizeof(r->name) - 1U); r->name[sizeof(r->name) - 1U] = '\0'; }
    }
}

#endif
