// SPDX-License-Identifier: MIT
// Frame formats adapted from ESP32 Marauder, Copyright (c) 2020 Just Call Me Koko.
// See MARAUDER_LICENSE.txt for provenance and license.
#ifndef POOM_MARAUDER_FRAMES_H
#define POOM_MARAUDER_FRAMES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POOM_MARAUDER_FRAME_CAPACITY 256U
typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32];
    size_t ssid_len;
    uint8_t channel;
    bool privacy;
    bool wpa3;
} poom_marauder_frame_target_t;

/* All builders return zero on invalid arguments/capacity. No radio or RNG I/O. */
size_t poom_marauder_frame_bad_msg(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t client[6],
    uint16_t sequence, uint64_t replay, const uint8_t nonce[32]);
size_t poom_marauder_frame_assoc_sleep(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t client[6], uint16_t sequence);
size_t poom_marauder_frame_beacon(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, uint16_t sequence,
    uint64_t timestamp_us, bool quiet, uint8_t destination_channel);
size_t poom_marauder_frame_sae(uint8_t *out, size_t capacity,
    const poom_marauder_frame_target_t *ap, const uint8_t source[6],
    uint16_t sequence, const uint8_t scalar[32], const uint8_t point[64]);
#endif
