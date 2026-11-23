// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum {
    POOM_BLE_SPAM_APPLE_DEVICES,
    POOM_BLE_SPAM_APPLE_JUICE,
    POOM_BLE_SPAM_SOUR_APPLE,
    POOM_BLE_SPAM_SWIFT_PAIR,
    POOM_BLE_SPAM_SAMSUNG,
    POOM_BLE_SPAM_FAST_PAIR,
    POOM_BLE_SPAM_FLIPPER,
    POOM_BLE_SPAM_ALL,
    POOM_BLE_SPAM_MODE_COUNT
} poom_ble_spam_mode_t;
const char *poom_ble_spam_mode_name(poom_ble_spam_mode_t mode);
/* Pure legacy-ADV builder, no heap or radio access. Output maximum 31 bytes. */
size_t poom_ble_payload_build(poom_ble_spam_mode_t mode, uint32_t sequence, uint32_t entropy,
                             uint8_t output[31], char label[24]);
