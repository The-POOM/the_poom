// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send a DESFire native command (wrapped as ISO-DEP APDU 0x90 ..) and collect chained responses.
 *
 * This helper relies on an active NFC ISO-DEP connection (run nfc-core-start + nfc-card-connect first).
 *
 * @param ins         DESFire INS byte (e.g. 0x5A Select Application, 0xBD Read Data, 0x60 Get Version)
 * @param data        Optional command data (may be NULL when data_len==0)
 * @param data_len    Length of data (0..250)
 * @param out_buf     Output buffer for payload (may be NULL when out_buf_max==0)
 * @param out_buf_max Output buffer size
 * @param out_len     Total payload length copied to out_buf
 * @param out_status  DESFire status byte (SW2 from 0x91SW2)
 */
bool poom_clipper_desfire_cmd_collect(uint8_t ins,
                                     const uint8_t* data,
                                     size_t data_len,
                                     uint8_t* out_buf,
                                     size_t out_buf_max,
                                     size_t* out_len,
                                     uint8_t* out_status);

/**
 * @brief Read and print Clipper DESFire ride history using the active NFC reader.
 *
 * Returns 0 on success, 1 on failure (CLI-friendly).
 */
int poom_clipper_print_history(void);

#ifdef __cplusplus
}
#endif
