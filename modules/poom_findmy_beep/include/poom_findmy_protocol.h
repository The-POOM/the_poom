// SPDX-License-Identifier: MIT
// Protocol constants/response grammar adapted from Dragon Detector and Marauder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POOM_FINDMY_MAX_DEVICES 24U
#define POOM_FINDMY_FRESH_MS 30000U
enum { POOM_FINDMY_APPLE = 1U, POOM_FINDMY_FMNA = 2U, POOM_FINDMY_DULT = 4U };
typedef enum { POOM_FINDMY_AIRTAG, POOM_FINDMY_ACCESSORY, POOM_FINDMY_DULT_SOUND, POOM_FINDMY_PROTOCOL_COUNT } poom_findmy_protocol_t;
typedef enum { POOM_FINDMY_REPLY_NONE, POOM_FINDMY_REPLY_RECEIVED, POOM_FINDMY_REPLY_ACCEPTED,
               POOM_FINDMY_REPLY_REJECTED, POOM_FINDMY_REPLY_COMPLETED } poom_findmy_reply_t;
typedef struct { uint8_t mac[6]; uint8_t address_type; } poom_findmy_key_t;
typedef struct {
    poom_findmy_key_t key;
    uint32_t last_seen_ms;
    int8_t rssi;
    uint8_t candidates;
    bool connectable;
    char name[24];
} poom_findmy_device_t;
typedef struct {
    const char *name;
    uint16_t service16; /* Zero means the 128-bit UUID below. Bluetooth byte order. */
    uint8_t service128[16];
    uint8_t characteristic128[16];
    uint8_t command[3];
    uint8_t command_length;
    bool responses;
} poom_findmy_profile_t;

const poom_findmy_profile_t *poom_findmy_profile(poom_findmy_protocol_t protocol);
/* Parse each advertising/scan-response payload separately. Never span AD boundaries.
 * Returns false for malformed data; partial candidate output must then be discarded. */
bool poom_findmy_parse_ad(const uint8_t *data, size_t length, uint8_t *candidates, char name[24]);
bool poom_findmy_key_equal(const poom_findmy_key_t *a, const poom_findmy_key_t *b);
bool poom_findmy_eligible(const poom_findmy_device_t *device, uint32_t now_ms);
/* Freeze unique currently eligible identities, preserving the displayed order. */
size_t poom_findmy_freeze(const poom_findmy_device_t *devices, size_t count, uint32_t now_ms,
                         poom_findmy_key_t *keys, size_t capacity);
poom_findmy_reply_t poom_findmy_response(poom_findmy_protocol_t protocol,
                                        const uint8_t *data, size_t length, uint16_t *status);
