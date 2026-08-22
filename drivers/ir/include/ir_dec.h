// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_dec.h
 * @brief IR protocol decode helpers.
 */

#ifndef IR_DEC_H
#define IR_DEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_rx.h"

typedef enum
{
    IR_PROTOCOL_NONE = 0U,
    IR_PROTOCOL_NEC = 1U,
    IR_PROTOCOL_NEC_EXT = 2U,
    IR_PROTOCOL_SAMSUNG32 = 3U,
    IR_PROTOCOL_SIRC = 4U,
    IR_PROTOCOL_SIRC15 = 5U,
    IR_PROTOCOL_SIRC20 = 6U,
    IR_PROTOCOL_RC5 = 7U,
    IR_PROTOCOL_RC5X = 8U,
    IR_PROTOCOL_RC6 = 9U,
    IR_PROTOCOL_RCA = 10U,
    IR_PROTOCOL_PIONEER = 11U,
    IR_PROTOCOL_KASEIKYO = 12U,
    IR_PROTOCOL_NEC42 = 13U,
    IR_PROTOCOL_NEC42_EXT = 14U,
} ir_protocol_t;

typedef struct
{
    ir_protocol_t protocol;
    uint32_t address;
    uint32_t command;
    bool repeat;
} ir_decoded_frame_t;

typedef struct
{
    bool has_last_frame;
    ir_decoded_frame_t last_frame;
    bool rc5_toggle_valid;
    bool rc5_last_toggle;
    bool rc6_toggle_valid;
    bool rc6_last_toggle;
} ir_decoder_context_t;

const char* ir_protocol_name(ir_protocol_t protocol);
bool ir_protocol_parse_name(const char* text, ir_protocol_t* out_protocol);
bool ir_protocol_is_supported(ir_protocol_t protocol);
void ir_decoder_context_reset(ir_decoder_context_t* context);

bool ir_decode_any_ex(const rmt_symbol_word_t* symbols,
                      size_t symbol_count,
                      uint32_t clk_hz,
                      ir_decoder_context_t* context,
                      ir_decoded_frame_t* out_frame);

bool ir_decode_any(const rmt_symbol_word_t* symbols,
                   size_t symbol_count,
                   uint32_t clk_hz,
                   ir_decoded_frame_t* out_frame);

/**
 * @brief Decode a basic NEC frame from RMT RX symbols.
 *
 * Supports:
 * - NEC: 8-bit address + ~address + 8-bit command + ~command
 * - NECext: 16-bit address + 8-bit command + ~command
 *
 * @param[in] syms Received symbols.
 * @param[in] n Number of symbols.
 * @param[out] out_addr Decoded address (NEC: low byte used, NECext: full 16-bit).
 * @param[out] out_cmd Decoded 8-bit command (in low byte of uint16_t).
 * @return true when a valid NEC frame is decoded.
 */
bool nec_decode(const rmt_symbol_word_t* symbols,
                size_t symbol_count,
                uint16_t* out_address,
                uint16_t* out_command);

/**
 * @brief Decode a NEC/NECext frame and report variant.
 *
 * @param[in] symbols Received symbols.
 * @param[in] symbol_count Number of symbols.
 * @param[out] out_address Decoded address (8-bit or 16-bit).
 * @param[out] out_command Decoded 8-bit command (in low byte of uint16_t).
 * @param[out] out_is_extended True when frame matches NECext encoding.
 * @return true when a valid frame is decoded.
 */
bool nec_decode_ex(const rmt_symbol_word_t* symbols,
                   size_t symbol_count,
                   uint16_t* out_address,
                   uint16_t* out_command,
                   bool* out_is_extended);

/**
 * @brief Decode a Samsung32 frame from RMT RX symbols.
 *
 * Note: assumes 1 MHz RMT RX resolution (ticks == microseconds).
 *
 * @param[in] symbols Received symbols.
 * @param[in] symbol_count Number of symbols.
 * @param[out] out_address Decoded 16-bit address.
 * @param[out] out_command Decoded 8-bit command (in low byte of uint16_t).
 * @return true when a valid Samsung32 frame is decoded.
 */
bool samsung32_decode(const rmt_symbol_word_t* symbols,
                      size_t symbol_count,
                      uint16_t* out_address,
                      uint16_t* out_command);

#ifdef __cplusplus
}
#endif

#endif /* IR_DEC_H */
