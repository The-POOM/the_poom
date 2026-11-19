// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
/* C5 L-LTF: signed 12-bit scalar in a little-endian 16-bit storage word. */
static inline int16_t poom_csi_decode12(const int8_t *bytes) {
    uint16_t word = ((uint16_t)(uint8_t)bytes[0] | ((uint16_t)(uint8_t)bytes[1] << 8U)) & 0x0FFFU;
    return word & 0x0800U ? (int16_t)((int32_t)word - 4096) : (int16_t)word;
}
