// SPDX-License-Identifier: MIT
// See LICENSE and README.md for Dragon Detector / Just Call Me Koko attribution.
#include "poom_findmy_protocol.h"
#include <string.h>

static const poom_findmy_profile_t profiles[] = {
    { "AirTag", 0, {0x6c,0xd6,0xf8,0x28,0x97,0x8d,0xaa,0x86,0x51,0x49,0x1c,0x7d,0x00,0x90,0xfc,0x7d},
      {0x6c,0xd6,0xf8,0x28,0x97,0x8d,0xaa,0x86,0x51,0x49,0x1c,0x7d,0x01,0x90,0xfc,0x7d}, {0xaf,0,0},1,false },
    { "Find My accessory", 0xfd44, {0},
      {0x7a,0x42,0x04,0x03,0x73,0x2f,0xd4,0xbe,0xef,0x49,0x3b,0x94,0x03,0x00,0x86,0x4f}, {1,0,3},3,true },
    { "DULT", 0, {0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,0x26,0xc2,0xf4,0x12,0x01,0x00,0x19,0x15},
      {0x0e,0x68,0x21,0x74,0x37,0x48,0x61,0xbf,0x92,0xfb,0x68,0x1d,0x01,0x00,0x0c,0x8e}, {0,3,0},2,true },
};

const poom_findmy_profile_t *poom_findmy_profile(poom_findmy_protocol_t protocol)
{
    return (unsigned)protocol < POOM_FINDMY_PROTOCOL_COUNT ? &profiles[protocol] : NULL;
}

bool poom_findmy_parse_ad(const uint8_t *data, size_t length, uint8_t *candidates, char name[24])
{
    if(!candidates || !name || (!data && length)) return false;
    uint8_t flags = 0;
    char parsed_name[24] = {0};
    for(size_t offset = 0; offset < length;) {
        const size_t n = data[offset++];
        if(!n) break;
        if(n > length - offset) return false;
        const uint8_t type = data[offset];
        const uint8_t *p = data + offset + 1;
        const size_t bytes = n - 1;
        if(type == 0xff && bytes >= 29 && p[0] == 0x4c && p[1] == 0 && p[2] == 0x12 && p[3] == 0x19) {
            flags |= POOM_FINDMY_APPLE;
        } else if(type == 0x02 || type == 0x03 || type == 0x16) {
            if(bytes % 2 && type != 0x16) return false;
            const size_t stop = type == 0x16 ? (bytes >= 2 ? 2 : 0) : bytes;
            for(size_t i = 0; i < stop; i += 2) {
                const uint16_t uuid = (uint16_t)(p[i] | ((uint16_t)p[i + 1] << 8));
                if(uuid == 0xfd44) flags |= POOM_FINDMY_FMNA;
                if(uuid == 0xfcb2) flags |= POOM_FINDMY_DULT;
            }
        } else if(type == 0x06 || type == 0x07 || type == 0x21) {
            if(bytes % 16 && type != 0x21) return false;
            const size_t stop = type == 0x21 ? (bytes >= 16 ? 16 : 0) : bytes;
            for(size_t i = 0; i < stop; i += 16) {
                if(!memcmp(p + i, profiles[POOM_FINDMY_DULT_SOUND].service128, 16)) flags |= POOM_FINDMY_DULT;
                if(!memcmp(p + i, profiles[POOM_FINDMY_AIRTAG].service128, 16)) flags |= POOM_FINDMY_APPLE;
            }
        } else if((type == 0x09 || type == 0x08) && (type == 0x09 || !parsed_name[0])) {
            const size_t copy = bytes < sizeof(parsed_name) - 1 ? bytes : sizeof(parsed_name) - 1;
            for(size_t i = 0; i < copy; ++i) parsed_name[i] = p[i] >= 32 && p[i] <= 126 ? (char)p[i] : '?';
            parsed_name[copy] = 0;
        }
        offset += n;
    }
    *candidates |= flags;
    if(parsed_name[0]) memcpy(name, parsed_name, sizeof(parsed_name));
    return true;
}

bool poom_findmy_key_equal(const poom_findmy_key_t *a, const poom_findmy_key_t *b)
{
    return a && b && a->address_type == b->address_type && !memcmp(a->mac, b->mac, 6);
}
bool poom_findmy_eligible(const poom_findmy_device_t *device, uint32_t now_ms)
{
    return device && device->candidates && device->connectable && device->key.address_type <= 3 &&
           (uint32_t)(now_ms - device->last_seen_ms) < POOM_FINDMY_FRESH_MS;
}
size_t poom_findmy_freeze(const poom_findmy_device_t *devices, size_t count, uint32_t now_ms,
                         poom_findmy_key_t *keys, size_t capacity)
{
    if(!devices || !keys) return 0;
    size_t used = 0;
    for(size_t i = 0; i < count && used < capacity; ++i) {
        if(!poom_findmy_eligible(&devices[i], now_ms)) continue;
        bool duplicate = false;
        for(size_t j = 0; j < used; ++j) if(poom_findmy_key_equal(&keys[j], &devices[i].key)) duplicate = true;
        if(!duplicate) keys[used++] = devices[i].key;
    }
    return used;
}
poom_findmy_reply_t poom_findmy_response(poom_findmy_protocol_t protocol,
                                        const uint8_t *data, size_t length, uint16_t *status)
{
    if(!data || !length || (unsigned)protocol >= POOM_FINDMY_PROTOCOL_COUNT) return POOM_FINDMY_REPLY_NONE;
    if(protocol != POOM_FINDMY_DULT_SOUND) return POOM_FINDMY_REPLY_RECEIVED;
    if(length == 2 && data[0] == 3 && data[1] == 3) return POOM_FINDMY_REPLY_COMPLETED;
    if(length == 6 && data[0] == 2 && data[1] == 3 && data[2] == 0 && data[3] == 3) {
        const uint16_t result = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
        if(status) *status = result;
        return result ? POOM_FINDMY_REPLY_REJECTED : POOM_FINDMY_REPLY_ACCEPTED;
    }
    return POOM_FINDMY_REPLY_NONE;
}
