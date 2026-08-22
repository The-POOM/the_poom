// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_dec.c
 * @brief Implementation of IR protocol decoders.
 */

#include "ir_dec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IR_DEC_US_PER_SECOND (1000000ULL)

#define IR_DEC_NEC_HDR_MARK_US (9000U)
#define IR_DEC_NEC_HDR_SPACE_US (4500U)
#define IR_DEC_NEC_REPEAT_SPACE_US (2250U)
#define IR_DEC_NEC_BIT_MARK_US (560U)
#define IR_DEC_NEC_ONE_SPACE_US (1690U)
#define IR_DEC_NEC_ZERO_SPACE_US (560U)
#define IR_DEC_NEC_STOP_MARK_US (560U)

#define IR_DEC_SAMSUNG32_HDR_MARK_US (4500U)
#define IR_DEC_SAMSUNG32_HDR_SPACE_US (4500U)
#define IR_DEC_SAMSUNG32_REPEAT_MARK_US (4500U)
#define IR_DEC_SAMSUNG32_REPEAT_SPACE_US (2250U)
#define IR_DEC_SAMSUNG32_BIT_MARK_US (550U)
#define IR_DEC_SAMSUNG32_ONE_SPACE_US (1650U)
#define IR_DEC_SAMSUNG32_ZERO_SPACE_US (550U)
#define IR_DEC_SAMSUNG32_STOP_MARK_US (550U)

#define IR_DEC_SIRC_HDR_MARK_US (2400U)
#define IR_DEC_SIRC_HDR_SPACE_US (600U)
#define IR_DEC_SIRC_ONE_MARK_US (1200U)
#define IR_DEC_SIRC_ZERO_MARK_US (600U)
#define IR_DEC_SIRC_BIT_SPACE_US (600U)
#define IR_DEC_SIRC_MIN_SPLIT_US (10000U)

#define IR_DEC_RCA_HDR_MARK_US (4000U)
#define IR_DEC_RCA_HDR_SPACE_US (4000U)
#define IR_DEC_RCA_BIT_MARK_US (500U)
#define IR_DEC_RCA_ONE_SPACE_US (2000U)
#define IR_DEC_RCA_ZERO_SPACE_US (1000U)

#define IR_DEC_PIONEER_HDR_MARK_US (8500U)
#define IR_DEC_PIONEER_HDR_SPACE_US (4225U)
#define IR_DEC_PIONEER_BIT_MARK_US (500U)
#define IR_DEC_PIONEER_ONE_SPACE_US (1500U)
#define IR_DEC_PIONEER_ZERO_SPACE_US (500U)

#define IR_DEC_KASEIKYO_HDR_MARK_US (3360U)
#define IR_DEC_KASEIKYO_HDR_SPACE_US (1665U)
#define IR_DEC_KASEIKYO_BIT_MARK_US (420U)
#define IR_DEC_KASEIKYO_ONE_SPACE_US (1274U)
#define IR_DEC_KASEIKYO_ZERO_SPACE_US (420U)

#define IR_DEC_RC5_BIT_US (889U)
#define IR_DEC_RC6_HDR_MARK_US (2666U)
#define IR_DEC_RC6_HDR_SPACE_US (889U)
#define IR_DEC_RC6_BIT_US (444U)

#define IR_DEC_HDR_TOL_US (350U)
#define IR_DEC_BIT_TOL_US (220U)
#define IR_DEC_RC_TOL_US (180U)

#define IR_DEC_NEC_BITS (32U)
#define IR_DEC_NEC42_BITS (42U)
#define IR_DEC_SAMSUNG32_BITS (32U)
#define IR_DEC_SIRC12_BITS (12U)
#define IR_DEC_SIRC15_BITS (15U)
#define IR_DEC_SIRC20_BITS (20U)
#define IR_DEC_RCA_BITS (24U)
#define IR_DEC_PIONEER_BITS (32U)
#define IR_DEC_PIONEER33_BITS (33U)
#define IR_DEC_KASEIKYO_BITS (48U)
#define IR_DEC_RC5_BITS (14U)
#define IR_DEC_RC6_BITS (21U)

typedef struct
{
    uint32_t* timings_us;
    size_t count;
} ir_flat_timings_t;

typedef enum
{
    IR_DEC_MANCHESTER_ERROR = 0,
    IR_DEC_MANCHESTER_OK,
    IR_DEC_MANCHESTER_READY,
} ir_dec_manchester_status_t;

/**
 * @brief Internal helper for `ir_dec_ticks_to_us`.
 *
 * @param[in] ticks Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @return inline uint32_t
 */
static inline uint32_t ir_dec_ticks_to_us_(uint32_t ticks, uint32_t clk_hz)
{
    if (clk_hz == 0U)
    {
        return 0U;
    }

    return (uint32_t)(((uint64_t)ticks * IR_DEC_US_PER_SECOND) / (uint64_t)clk_hz);
}

/**
 * @brief Internal helper for `ir_dec_match`.
 *
 * @param[in] actual_us Parameter passed to the function.
 * @param[in] expected_us Parameter passed to the function.
 * @param[in] tolerance_us Parameter passed to the function.
 * @return inline bool
 */
static inline bool ir_dec_match_(uint32_t actual_us, uint32_t expected_us, uint32_t tolerance_us)
{
    if (actual_us >= expected_us)
    {
        return (actual_us - expected_us) <= tolerance_us;
    }

    return (expected_us - actual_us) <= tolerance_us;
}

/**
 * @brief Internal helper for `ir_dec_repeat_tail_has_only_silence`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] start_index Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_repeat_tail_has_only_silence_(const rmt_symbol_word_t* symbols,
                                                 size_t symbol_count,
                                                 size_t start_index)
{
    if (symbols == NULL)
    {
        return false;
    }

    for (size_t symbol_index = start_index; symbol_index < symbol_count; symbol_index++)
    {
        if ((symbols[symbol_index].duration0 > 0U) || (symbols[symbol_index].duration1 > 0U))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Internal helper for `ir_dec_reverse8`.
 *
 * @param[in] value Parameter passed to the function.
 * @return inline uint8_t
 */
static inline uint8_t ir_dec_reverse8_(uint8_t value)
{
    uint8_t out = 0U;

    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
        out = (uint8_t)((out << 1U) | (value & 0x01U));
        value >>= 1U;
    }

    return out;
}

/**
 * @brief Internal helper for `ir_dec_store_bit`.
 *
 * @param[in] data_bytes Parameter passed to the function.
 * @param[in] data_len Parameter passed to the function.
 * @param[in] io_bit_count Parameter passed to the function.
 * @param[in] bit_value Parameter passed to the function.
 * @param[in] bit_limit Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_store_bit_(uint8_t* data_bytes,
                              size_t data_len,
                              size_t* io_bit_count,
                              bool bit_value,
                              size_t bit_limit)
{
    const size_t byte_index = *io_bit_count / 8U;
    const uint8_t bit_index = (uint8_t)(*io_bit_count % 8U);

    if ((data_bytes == NULL) || (io_bit_count == NULL) || (byte_index >= data_len) || (*io_bit_count >= bit_limit))
    {
        return false;
    }

    if (bit_value)
    {
        data_bytes[byte_index] |= (uint8_t)(1U << bit_index);
    }

    (*io_bit_count)++;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_manchester_step`.
 *
 * @param[in] data_bytes Parameter passed to the function.
 * @param[in] data_len Parameter passed to the function.
 * @param[in] io_bit_count Parameter passed to the function.
 * @param[in] io_switch_detect Parameter passed to the function.
 * @param[in] level Parameter passed to the function.
 * @param[in] timing_us Parameter passed to the function.
 * @param[in] bit_time_us Parameter passed to the function.
 * @param[in] tolerance_us Parameter passed to the function.
 * @param[in] start_from_space Parameter passed to the function.
 * @param[in] bit_limit Parameter passed to the function.
 * @return ir_dec_manchester_status_t
 */
static ir_dec_manchester_status_t ir_dec_manchester_step_(uint8_t* data_bytes,
                                                          size_t data_len,
                                                          size_t* io_bit_count,
                                                          bool* io_switch_detect,
                                                          bool level,
                                                          uint32_t timing_us,
                                                          uint32_t bit_time_us,
                                                          uint32_t tolerance_us,
                                                          bool start_from_space,
                                                          size_t bit_limit)
{
    const bool single_timing = ir_dec_match_(timing_us, bit_time_us, tolerance_us);
    const bool double_timing = ir_dec_match_(timing_us, bit_time_us * 2U, tolerance_us);
    bool bit_value = false;

    if ((data_bytes == NULL) || (io_bit_count == NULL) || (io_switch_detect == NULL))
    {
        return IR_DEC_MANCHESTER_ERROR;
    }

    if (!single_timing && !double_timing)
    {
        return IR_DEC_MANCHESTER_ERROR;
    }

    if (single_timing)
    {
        if (!(*io_switch_detect))
        {
            *io_switch_detect = true;
            return IR_DEC_MANCHESTER_OK;
        }

        bit_value = start_from_space ? !level : level;
        if (!ir_dec_store_bit_(data_bytes, data_len, io_bit_count, bit_value, bit_limit))
        {
            return IR_DEC_MANCHESTER_ERROR;
        }

        *io_switch_detect = false;
        return (*io_bit_count >= bit_limit) ? IR_DEC_MANCHESTER_READY : IR_DEC_MANCHESTER_OK;
    }

    bit_value = start_from_space ? level : !level;
    if (!ir_dec_store_bit_(data_bytes, data_len, io_bit_count, bit_value, bit_limit))
    {
        return IR_DEC_MANCHESTER_ERROR;
    }

    *io_switch_detect = false;
    return (*io_bit_count >= bit_limit) ? IR_DEC_MANCHESTER_READY : IR_DEC_MANCHESTER_OK;
}

/**
 * @brief Internal helper for `ir_dec_find_header`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] header_mark_us Parameter passed to the function.
 * @param[in] header_space_us Parameter passed to the function.
 * @param[in] tolerance_us Parameter passed to the function.
 * @param[in] out_payload_start_index Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_find_header_(const rmt_symbol_word_t* symbols,
                                size_t symbol_count,
                                uint32_t clk_hz,
                                uint32_t header_mark_us,
                                uint32_t header_space_us,
                                uint32_t tolerance_us,
                                size_t* out_payload_start_index)
{
    if ((symbols == NULL) || (out_payload_start_index == NULL))
    {
        return false;
    }

    for (size_t symbol_index = 0U; symbol_index < symbol_count; symbol_index++)
    {
        const uint32_t mark_us = ir_dec_ticks_to_us_(symbols[symbol_index].duration0, clk_hz);
        const uint32_t space_us = ir_dec_ticks_to_us_(symbols[symbol_index].duration1, clk_hz);

        if (ir_dec_match_(mark_us, header_mark_us, tolerance_us) &&
            ir_dec_match_(space_us, header_space_us, tolerance_us))
        {
            *out_payload_start_index = symbol_index + 1U;
            return true;
        }
    }

    return false;
}

/**
 * @brief Internal helper for `ir_dec_decode_space_bits_lsb`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] payload_start_index Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] bit_mark_us Parameter passed to the function.
 * @param[in] zero_space_us Parameter passed to the function.
 * @param[in] one_space_us Parameter passed to the function.
 * @param[in] tolerance_us Parameter passed to the function.
 * @param[in] bit_count Parameter passed to the function.
 * @param[in] out_bits_lsb Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_space_bits_lsb_(const rmt_symbol_word_t* symbols,
                                          size_t symbol_count,
                                          size_t payload_start_index,
                                          uint32_t clk_hz,
                                          uint32_t bit_mark_us,
                                          uint32_t zero_space_us,
                                          uint32_t one_space_us,
                                          uint32_t tolerance_us,
                                          size_t bit_count,
                                          uint64_t* out_bits_lsb)
{
    uint64_t bits = 0U;

    if ((symbols == NULL) || (out_bits_lsb == NULL))
    {
        return false;
    }

    if ((payload_start_index + bit_count) > symbol_count)
    {
        return false;
    }

    for (size_t bit_index = 0U; bit_index < bit_count; bit_index++)
    {
        const rmt_symbol_word_t* symbol = &symbols[payload_start_index + bit_index];
        const uint32_t mark_us = ir_dec_ticks_to_us_(symbol->duration0, clk_hz);
        const uint32_t space_us = ir_dec_ticks_to_us_(symbol->duration1, clk_hz);

        if (!ir_dec_match_(mark_us, bit_mark_us, tolerance_us))
        {
            return false;
        }

        if (ir_dec_match_(space_us, zero_space_us, tolerance_us))
        {
            continue;
        }

        if (ir_dec_match_(space_us, one_space_us, tolerance_us))
        {
            bits |= ((uint64_t)1U << bit_index);
            continue;
        }

        return false;
    }

    *out_bits_lsb = bits;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_decode_mark_bits_lsb`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] payload_start_index Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] zero_mark_us Parameter passed to the function.
 * @param[in] one_mark_us Parameter passed to the function.
 * @param[in] bit_space_us Parameter passed to the function.
 * @param[in] tolerance_us Parameter passed to the function.
 * @param[in] split_min_us Parameter passed to the function.
 * @param[in] out_bits_lsb Parameter passed to the function.
 * @param[in] out_bit_count Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_mark_bits_lsb_(const rmt_symbol_word_t* symbols,
                                         size_t symbol_count,
                                         size_t payload_start_index,
                                         uint32_t clk_hz,
                                         uint32_t zero_mark_us,
                                         uint32_t one_mark_us,
                                         uint32_t bit_space_us,
                                         uint32_t tolerance_us,
                                         uint32_t split_min_us,
                                         uint64_t* out_bits_lsb,
                                         size_t* out_bit_count)
{
    uint64_t bits = 0U;
    size_t bit_count = 0U;

    if ((symbols == NULL) || (out_bits_lsb == NULL) || (out_bit_count == NULL))
    {
        return false;
    }

    for (size_t symbol_index = payload_start_index; symbol_index < symbol_count; symbol_index++)
    {
        const rmt_symbol_word_t* symbol = &symbols[symbol_index];
        const uint32_t mark_us = ir_dec_ticks_to_us_(symbol->duration0, clk_hz);
        const uint32_t space_us = ir_dec_ticks_to_us_(symbol->duration1, clk_hz);
        bool bit_value = false;

        if (ir_dec_match_(mark_us, zero_mark_us, tolerance_us))
        {
            bit_value = false;
        }
        else if (ir_dec_match_(mark_us, one_mark_us, tolerance_us))
        {
            bit_value = true;
        }
        else
        {
            return false;
        }

        if (bit_value)
        {
            bits |= ((uint64_t)1U << bit_count);
        }
        bit_count++;

        if (space_us == 0U)
        {
            break;
        }

        if (split_min_us > 0U)
        {
            if (space_us > split_min_us)
            {
                break;
            }
        }
        else if (!ir_dec_match_(space_us, bit_space_us, tolerance_us))
        {
            return false;
        }

        if ((split_min_us > 0U) && (!ir_dec_match_(space_us, bit_space_us, tolerance_us)))
        {
            return false;
        }
    }

    *out_bits_lsb = bits;
    *out_bit_count = bit_count;
    return bit_count > 0U;
}

/**
 * @brief Internal helper for `ir_dec_flatten_timings`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_flat Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_flatten_timings_(const rmt_symbol_word_t* symbols,
                                    size_t symbol_count,
                                    uint32_t clk_hz,
                                    ir_flat_timings_t* out_flat)
{
    uint32_t* timings_us = NULL;
    size_t timing_count = 0U;

    if ((symbols == NULL) || (out_flat == NULL))
    {
        return false;
    }

    memset(out_flat, 0, sizeof(*out_flat));

    timings_us = (uint32_t*)calloc(symbol_count * 2U, sizeof(*timings_us));
    if (timings_us == NULL)
    {
        return false;
    }

    for (size_t symbol_index = 0U; symbol_index < symbol_count; symbol_index++)
    {
        if (symbols[symbol_index].duration0 > 0U)
        {
            timings_us[timing_count++] = ir_dec_ticks_to_us_(symbols[symbol_index].duration0, clk_hz);
        }
        if (symbols[symbol_index].duration1 > 0U)
        {
            timings_us[timing_count++] = ir_dec_ticks_to_us_(symbols[symbol_index].duration1, clk_hz);
        }
    }

    if (timing_count == 0U)
    {
        free(timings_us);
        return false;
    }

    out_flat->timings_us = timings_us;
    out_flat->count = timing_count;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_flatten_free`.
 *
 * @param[in] flat Parameter passed to the function.
 * @return void
 */
static void ir_dec_flatten_free_(ir_flat_timings_t* flat)
{
    if (flat == NULL)
    {
        return;
    }

    free(flat->timings_us);
    flat->timings_us = NULL;
    flat->count = 0U;
}

/**
 * @brief Internal helper for `ir_dec_is_repeat_candidate`.
 *
 * @param[in] context Parameter passed to the function.
 * @param[in] protocol Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_is_repeat_candidate_(const ir_decoder_context_t* context, ir_protocol_t protocol)
{
    if ((context == NULL) || !context->has_last_frame)
    {
        return false;
    }

    if (protocol == IR_PROTOCOL_NONE)
    {
        return true;
    }

    switch (context->last_frame.protocol)
    {
        case IR_PROTOCOL_NEC:
        case IR_PROTOCOL_NEC_EXT:
        case IR_PROTOCOL_NEC42:
        case IR_PROTOCOL_NEC42_EXT:
            return protocol == IR_PROTOCOL_NEC;
        case IR_PROTOCOL_SAMSUNG32:
            return protocol == IR_PROTOCOL_SAMSUNG32;
        default:
            return context->last_frame.protocol == protocol;
    }
}

/**
 * @brief Internal helper for `ir_dec_decode_nec_repeat`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] context Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_nec_repeat_(const rmt_symbol_word_t* symbols,
                                      size_t symbol_count,
                                      uint32_t clk_hz,
                                      ir_decoder_context_t* context,
                                      ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    size_t header_symbol_index = 0U;
    const rmt_symbol_word_t* stop_symbol = NULL;

    if ((context == NULL) || (!context->has_last_frame) || (out_frame == NULL) || (symbols == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_NEC_HDR_MARK_US,
                             IR_DEC_NEC_REPEAT_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if ((payload_start_index == 0U) || (payload_start_index >= symbol_count))
    {
        return false;
    }

    if (!ir_dec_is_repeat_candidate_(context, IR_PROTOCOL_NEC))
    {
        return false;
    }

    header_symbol_index = payload_start_index - 1U;
    stop_symbol = &symbols[payload_start_index];

    if (!ir_dec_match_(ir_dec_ticks_to_us_(symbols[header_symbol_index].duration0, clk_hz),
                       IR_DEC_NEC_HDR_MARK_US,
                       IR_DEC_HDR_TOL_US) ||
        !ir_dec_match_(ir_dec_ticks_to_us_(symbols[header_symbol_index].duration1, clk_hz),
                       IR_DEC_NEC_REPEAT_SPACE_US,
                       IR_DEC_HDR_TOL_US))
    {
        return false;
    }

    if (!ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration0, clk_hz),
                       IR_DEC_NEC_STOP_MARK_US,
                       IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if ((stop_symbol->duration1 > 0U) &&
        ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration1, clk_hz),
                      IR_DEC_NEC_ZERO_SPACE_US,
                      IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if ((stop_symbol->duration1 > 0U) &&
        ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration1, clk_hz),
                      IR_DEC_NEC_ONE_SPACE_US,
                      IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if (!ir_dec_repeat_tail_has_only_silence_(symbols, symbol_count, payload_start_index + 1U))
    {
        return false;
    }

    *out_frame = context->last_frame;
    out_frame->repeat = true;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_decode_samsung32_repeat`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] context Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_samsung32_repeat_(const rmt_symbol_word_t* symbols,
                                            size_t symbol_count,
                                            uint32_t clk_hz,
                                            ir_decoder_context_t* context,
                                            ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    size_t header_symbol_index = 0U;
    const rmt_symbol_word_t* repeat_symbol = NULL;
    const rmt_symbol_word_t* stop_symbol = NULL;

    if ((context == NULL) || (!context->has_last_frame) || (out_frame == NULL) || (symbols == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_SAMSUNG32_REPEAT_MARK_US,
                             IR_DEC_SAMSUNG32_REPEAT_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if ((payload_start_index == 0U) || ((payload_start_index + 1U) >= symbol_count))
    {
        return false;
    }

    if (!ir_dec_is_repeat_candidate_(context, IR_PROTOCOL_SAMSUNG32))
    {
        return false;
    }

    header_symbol_index = payload_start_index - 1U;
    repeat_symbol = &symbols[payload_start_index];
    stop_symbol = &symbols[payload_start_index + 1U];

    if (!ir_dec_match_(ir_dec_ticks_to_us_(symbols[header_symbol_index].duration0, clk_hz),
                       IR_DEC_SAMSUNG32_REPEAT_MARK_US,
                       IR_DEC_HDR_TOL_US) ||
        !ir_dec_match_(ir_dec_ticks_to_us_(symbols[header_symbol_index].duration1, clk_hz),
                       IR_DEC_SAMSUNG32_REPEAT_SPACE_US,
                       IR_DEC_HDR_TOL_US))
    {
        return false;
    }

    if (!ir_dec_match_(ir_dec_ticks_to_us_(repeat_symbol->duration0, clk_hz),
                       IR_DEC_SAMSUNG32_BIT_MARK_US,
                       IR_DEC_BIT_TOL_US) ||
        !ir_dec_match_(ir_dec_ticks_to_us_(repeat_symbol->duration1, clk_hz),
                       IR_DEC_SAMSUNG32_ONE_SPACE_US,
                       IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if (!ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration0, clk_hz),
                       IR_DEC_SAMSUNG32_STOP_MARK_US,
                       IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if ((stop_symbol->duration1 > 0U) &&
        ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration1, clk_hz),
                      IR_DEC_SAMSUNG32_ZERO_SPACE_US,
                      IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if ((stop_symbol->duration1 > 0U) &&
        ir_dec_match_(ir_dec_ticks_to_us_(stop_symbol->duration1, clk_hz),
                      IR_DEC_SAMSUNG32_ONE_SPACE_US,
                      IR_DEC_BIT_TOL_US))
    {
        return false;
    }

    if (!ir_dec_repeat_tail_has_only_silence_(symbols, symbol_count, payload_start_index + 2U))
    {
        return false;
    }

    *out_frame = context->last_frame;
    out_frame->repeat = true;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_decode_nec_family`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_nec_family_(const rmt_symbol_word_t* symbols,
                                      size_t symbol_count,
                                      uint32_t clk_hz,
                                      ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    uint8_t command = 0U;
    uint8_t command_inv = 0U;

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_NEC_HDR_MARK_US,
                             IR_DEC_NEC_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if (ir_dec_decode_space_bits_lsb_(symbols,
                                      symbol_count,
                                      payload_start_index,
                                      clk_hz,
                                      IR_DEC_NEC_BIT_MARK_US,
                                      IR_DEC_NEC_ZERO_SPACE_US,
                                      IR_DEC_NEC_ONE_SPACE_US,
                                      IR_DEC_BIT_TOL_US,
                                      IR_DEC_NEC42_BITS,
                                      &bits))
    {
        const uint32_t data0 = (uint32_t)(bits & 0xFFFFFFFFULL);
        const uint16_t data1 = (uint16_t)((bits >> 32U) & 0xFFFFU);
        const uint32_t address = data0 & 0x1FFFU;
        const uint32_t address_inv = (data0 >> 13U) & 0x1FFFU;
        const uint32_t cmd = ((data0 >> 26U) & 0x3FU) | (((uint32_t)data1 & 0x03U) << 6U);
        const uint32_t cmd_inv = ((uint32_t)data1 >> 2U) & 0xFFU;

        if ((cmd ^ cmd_inv) == 0xFFU)
        {
            out_frame->protocol = ((address ^ address_inv) == 0x1FFFU) ? IR_PROTOCOL_NEC42 : IR_PROTOCOL_NEC42_EXT;
            out_frame->address = ((address ^ address_inv) == 0x1FFFU)
                ? address
                : (address | (address_inv << 13U));
            out_frame->command = ((address ^ address_inv) == 0x1FFFU) ? cmd : (cmd | (cmd_inv << 8U));
            out_frame->repeat = false;
            return true;
        }
    }

    if (!ir_dec_decode_space_bits_lsb_(symbols,
                                       symbol_count,
                                       payload_start_index,
                                       clk_hz,
                                       IR_DEC_NEC_BIT_MARK_US,
                                       IR_DEC_NEC_ZERO_SPACE_US,
                                       IR_DEC_NEC_ONE_SPACE_US,
                                       IR_DEC_BIT_TOL_US,
                                       IR_DEC_NEC_BITS,
                                       &bits))
    {
        return false;
    }

    command = (uint8_t)((bits >> 16U) & 0xFFU);
    command_inv = (uint8_t)((bits >> 24U) & 0xFFU);
    if ((uint8_t)(command ^ command_inv) != 0xFFU)
    {
        return false;
    }

    {
        const uint8_t address = (uint8_t)(bits & 0xFFU);
        const uint8_t address_second = (uint8_t)((bits >> 8U) & 0xFFU);

        out_frame->protocol = ((uint8_t)(address ^ address_second) == 0xFFU) ? IR_PROTOCOL_NEC : IR_PROTOCOL_NEC_EXT;
        out_frame->address = (out_frame->protocol == IR_PROTOCOL_NEC)
            ? (uint32_t)address
            : ((uint32_t)address | ((uint32_t)address_second << 8U));
        out_frame->command = command;
        out_frame->repeat = false;
        return true;
    }
}

/**
 * @brief Internal helper for `ir_dec_decode_samsung32`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_samsung32_(const rmt_symbol_word_t* symbols,
                                     size_t symbol_count,
                                     uint32_t clk_hz,
                                     ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    const uint8_t command_mask = 0xFFU;

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_SAMSUNG32_HDR_MARK_US,
                             IR_DEC_SAMSUNG32_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if (!ir_dec_decode_space_bits_lsb_(symbols,
                                       symbol_count,
                                       payload_start_index,
                                       clk_hz,
                                       IR_DEC_SAMSUNG32_BIT_MARK_US,
                                       IR_DEC_SAMSUNG32_ZERO_SPACE_US,
                                       IR_DEC_SAMSUNG32_ONE_SPACE_US,
                                       IR_DEC_BIT_TOL_US,
                                       IR_DEC_SAMSUNG32_BITS,
                                       &bits))
    {
        return false;
    }

    if (((uint8_t)((bits >> 16U) & command_mask) ^ (uint8_t)((bits >> 24U) & command_mask)) != command_mask)
    {
        return false;
    }

    if ((uint8_t)(bits & 0xFFU) != (uint8_t)((bits >> 8U) & 0xFFU))
    {
        return false;
    }

    out_frame->protocol = IR_PROTOCOL_SAMSUNG32;
    out_frame->address = (uint32_t)(bits & 0xFFU);
    out_frame->command = (uint32_t)((bits >> 16U) & command_mask);
    out_frame->repeat = false;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_decode_sirc`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_sirc_(const rmt_symbol_word_t* symbols,
                                size_t symbol_count,
                                uint32_t clk_hz,
                                ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    size_t bit_count = 0U;

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_SIRC_HDR_MARK_US,
                             IR_DEC_SIRC_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if (!ir_dec_decode_mark_bits_lsb_(symbols,
                                      symbol_count,
                                      payload_start_index,
                                      clk_hz,
                                      IR_DEC_SIRC_ZERO_MARK_US,
                                      IR_DEC_SIRC_ONE_MARK_US,
                                      IR_DEC_SIRC_BIT_SPACE_US,
                                      IR_DEC_BIT_TOL_US,
                                      IR_DEC_SIRC_MIN_SPLIT_US,
                                      &bits,
                                      &bit_count))
    {
        return false;
    }

    switch (bit_count)
    {
        case IR_DEC_SIRC12_BITS:
            out_frame->protocol = IR_PROTOCOL_SIRC;
            out_frame->address = (uint32_t)((bits >> 7U) & 0x1FU);
            out_frame->command = (uint32_t)(bits & 0x7FU);
            break;
        case IR_DEC_SIRC15_BITS:
            out_frame->protocol = IR_PROTOCOL_SIRC15;
            out_frame->address = (uint32_t)((bits >> 7U) & 0xFFU);
            out_frame->command = (uint32_t)(bits & 0x7FU);
            break;
        case IR_DEC_SIRC20_BITS:
            out_frame->protocol = IR_PROTOCOL_SIRC20;
            out_frame->address = (uint32_t)((bits >> 7U) & 0x1FFFU);
            out_frame->command = (uint32_t)(bits & 0x7FU);
            break;
        default:
            return false;
    }

    out_frame->repeat = false;
    return true;
}

/**
 * @brief Internal helper for `ir_dec_decode_rca`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_rca_(const rmt_symbol_word_t* symbols,
                               size_t symbol_count,
                               uint32_t clk_hz,
                               ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    const uint8_t address = 0U;
    const uint8_t command = 0U;

    (void)address;
    (void)command;

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_RCA_HDR_MARK_US,
                             IR_DEC_RCA_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if (!ir_dec_decode_space_bits_lsb_(symbols,
                                       symbol_count,
                                       payload_start_index,
                                       clk_hz,
                                       IR_DEC_RCA_BIT_MARK_US,
                                       IR_DEC_RCA_ZERO_SPACE_US,
                                       IR_DEC_RCA_ONE_SPACE_US,
                                       IR_DEC_BIT_TOL_US,
                                       IR_DEC_RCA_BITS,
                                       &bits))
    {
        return false;
    }

    {
        const uint8_t real_address = (uint8_t)(bits & 0x0FU);
        const uint8_t real_command = (uint8_t)((bits >> 4U) & 0xFFU);
        const uint8_t address_inv = (uint8_t)((bits >> 12U) & 0x0FU);
        const uint8_t command_inv = (uint8_t)((bits >> 16U) & 0xFFU);

        if (((uint8_t)(real_address ^ address_inv) != 0x0FU) ||
            ((uint8_t)(real_command ^ command_inv) != 0xFFU))
        {
            return false;
        }

        out_frame->protocol = IR_PROTOCOL_RCA;
        out_frame->address = real_address;
        out_frame->command = real_command;
        out_frame->repeat = false;
        return true;
    }
}

/**
 * @brief Internal helper for `ir_dec_decode_pioneer`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_pioneer_(const rmt_symbol_word_t* symbols,
                                   size_t symbol_count,
                                   uint32_t clk_hz,
                                   ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    const size_t bit_lengths[] = {IR_DEC_PIONEER33_BITS, IR_DEC_PIONEER_BITS};

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_PIONEER_HDR_MARK_US,
                             IR_DEC_PIONEER_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    for (size_t length_index = 0U; length_index < (sizeof(bit_lengths) / sizeof(bit_lengths[0])); length_index++)
    {
        if (!ir_dec_decode_space_bits_lsb_(symbols,
                                           symbol_count,
                                           payload_start_index,
                                           clk_hz,
                                           IR_DEC_PIONEER_BIT_MARK_US,
                                           IR_DEC_PIONEER_ZERO_SPACE_US,
                                           IR_DEC_PIONEER_ONE_SPACE_US,
                                           IR_DEC_BIT_TOL_US,
                                           bit_lengths[length_index],
                                           &bits))
        {
            continue;
        }

        {
            const uint32_t frame32 = (uint32_t)(bits & 0xFFFFFFFFU);
            const uint8_t address = (uint8_t)(frame32 & 0xFFU);
            const uint8_t address_inv = (uint8_t)((frame32 >> 8U) & 0xFFU);
            const uint8_t command = (uint8_t)((frame32 >> 16U) & 0xFFU);
            const uint8_t command_inv = (uint8_t)((frame32 >> 24U) & 0xFFU);

            if (((uint8_t)(address ^ address_inv) != 0xFFU) ||
                ((uint8_t)(command ^ command_inv) != 0xFFU))
            {
                continue;
            }

            out_frame->protocol = IR_PROTOCOL_PIONEER;
            out_frame->address = address;
            out_frame->command = command;
            out_frame->repeat = false;
            return true;
        }
    }

    return false;
}

/**
 * @brief Internal helper for `ir_dec_decode_kaseikyo`.
 *
 * @param[in] symbols Parameter passed to the function.
 * @param[in] symbol_count Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_kaseikyo_(const rmt_symbol_word_t* symbols,
                                    size_t symbol_count,
                                    uint32_t clk_hz,
                                    ir_decoded_frame_t* out_frame)
{
    size_t payload_start_index = 0U;
    uint64_t bits = 0U;
    uint8_t data[6] = {0};

    if ((symbols == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (!ir_dec_find_header_(symbols,
                             symbol_count,
                             clk_hz,
                             IR_DEC_KASEIKYO_HDR_MARK_US,
                             IR_DEC_KASEIKYO_HDR_SPACE_US,
                             IR_DEC_HDR_TOL_US,
                             &payload_start_index))
    {
        return false;
    }

    if (!ir_dec_decode_space_bits_lsb_(symbols,
                                       symbol_count,
                                       payload_start_index,
                                       clk_hz,
                                       IR_DEC_KASEIKYO_BIT_MARK_US,
                                       IR_DEC_KASEIKYO_ZERO_SPACE_US,
                                       IR_DEC_KASEIKYO_ONE_SPACE_US,
                                       IR_DEC_BIT_TOL_US,
                                       IR_DEC_KASEIKYO_BITS,
                                       &bits))
    {
        return false;
    }

    for (size_t index = 0U; index < sizeof(data); index++)
    {
        data[index] = (uint8_t)((bits >> (index * 8U)) & 0xFFU);
    }

    {
        const uint16_t vendor_id = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
        const uint8_t vendor_parity = data[2] & 0x0FU;
        const uint8_t genre1 = data[2] >> 4U;
        const uint8_t genre2 = data[3] & 0x0FU;
        const uint16_t command = (uint16_t)(data[3] >> 4U) | ((uint16_t)(data[4] & 0x3FU) << 4U);
        const uint8_t id = data[4] >> 6U;
        const uint8_t parity = data[5];
        uint8_t vendor_parity_check = (uint8_t)(data[0] ^ data[1]);
        const uint8_t parity_check = (uint8_t)(data[2] ^ data[3] ^ data[4]);

        vendor_parity_check = (uint8_t)((vendor_parity_check & 0x0FU) ^ (vendor_parity_check >> 4U));
        if ((vendor_parity != vendor_parity_check) || (parity != parity_check))
        {
            return false;
        }

        out_frame->protocol = IR_PROTOCOL_KASEIKYO;
        out_frame->address = ((uint32_t)id << 24U) |
                             ((uint32_t)vendor_id << 8U) |
                             ((uint32_t)genre1 << 4U) |
                             (uint32_t)genre2;
        out_frame->command = command;
        out_frame->repeat = false;
        return true;
    }
}

/**
 * @brief Internal helper for `ir_dec_decode_rc5`.
 *
 * @param[in] flat Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @param[in] out_toggle_valid Parameter passed to the function.
 * @param[in] out_toggle Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_rc5_(const ir_flat_timings_t* flat,
                               ir_decoded_frame_t* out_frame,
                               bool* out_toggle_valid,
                               bool* out_toggle)
{
    uint8_t data_bytes[2] = {0};
    size_t bit_count = 0U;
    bool switch_detect = false;
    bool level = true;

    if ((flat == NULL) || (flat->timings_us == NULL) || (out_frame == NULL))
    {
        return false;
    }

    if (out_toggle_valid != NULL)
    {
        *out_toggle_valid = false;
    }

    for (size_t timing_index = 0U; timing_index < flat->count; timing_index++)
    {
        const ir_dec_manchester_status_t status =
            ir_dec_manchester_step_(data_bytes,
                                    sizeof(data_bytes),
                                    &bit_count,
                                    &switch_detect,
                                    level,
                                    flat->timings_us[timing_index],
                                    IR_DEC_RC5_BIT_US,
                                    IR_DEC_RC_TOL_US,
                                    true,
                                    IR_DEC_RC5_BITS);

        if (status == IR_DEC_MANCHESTER_ERROR)
        {
            return false;
        }

        if (status == IR_DEC_MANCHESTER_READY)
        {
            break;
        }

        level = !level;
    }

    if (bit_count != IR_DEC_RC5_BITS)
    {
        return false;
    }

    data_bytes[0] = (uint8_t)~data_bytes[0];
    data_bytes[1] = (uint8_t)~data_bytes[1];

    {
        const uint16_t raw = (uint16_t)data_bytes[0] | ((uint16_t)data_bytes[1] << 8U);
        const bool start_bit1 = (raw & 0x01U) != 0U;
        const bool start_bit2 = (raw & 0x02U) != 0U;
        const bool toggle = (raw & 0x04U) != 0U;

        if (!start_bit1)
        {
            return false;
        }

        out_frame->protocol = start_bit2 ? IR_PROTOCOL_RC5 : IR_PROTOCOL_RC5X;
        out_frame->address = (uint32_t)(ir_dec_reverse8_(data_bytes[0]) & 0x1FU);
        out_frame->command = (uint32_t)((ir_dec_reverse8_(data_bytes[1]) >> 2U) & 0x3FU);
        out_frame->repeat = false;

        if (out_toggle_valid != NULL)
        {
            *out_toggle_valid = true;
        }
        if (out_toggle != NULL)
        {
            *out_toggle = toggle;
        }

        return true;
    }
}

/**
 * @brief Internal helper for `ir_dec_decode_rc6`.
 *
 * @param[in] flat Parameter passed to the function.
 * @param[in] out_frame Parameter passed to the function.
 * @param[in] out_toggle_valid Parameter passed to the function.
 * @param[in] out_toggle Parameter passed to the function.
 * @return bool
 */
static bool ir_dec_decode_rc6_(const ir_flat_timings_t* flat,
                               ir_decoded_frame_t* out_frame,
                               bool* out_toggle_valid,
                               bool* out_toggle)
{
    uint8_t data_bytes[4] = {0};
    size_t bit_count = 0U;
    bool switch_detect = false;
    bool level = true;
    size_t start_index = 0U;

    if ((flat == NULL) || (flat->timings_us == NULL) || (out_frame == NULL) || (flat->count < 3U))
    {
        return false;
    }

    if (out_toggle_valid != NULL)
    {
        *out_toggle_valid = false;
    }

    if (!ir_dec_match_(flat->timings_us[0], IR_DEC_RC6_HDR_MARK_US, IR_DEC_HDR_TOL_US) ||
        !ir_dec_match_(flat->timings_us[1], IR_DEC_RC6_HDR_SPACE_US, IR_DEC_HDR_TOL_US))
    {
        return false;
    }

    start_index = 2U;
    level = true;

    for (size_t timing_index = start_index; timing_index < flat->count; timing_index++)
    {
        uint32_t timing_us = flat->timings_us[timing_index];
        const bool single_timing = ir_dec_match_(timing_us, IR_DEC_RC6_BIT_US, IR_DEC_RC_TOL_US);
        const bool triple_timing = ir_dec_match_(timing_us, IR_DEC_RC6_BIT_US * 3U, IR_DEC_RC_TOL_US);
        ir_dec_manchester_status_t status = IR_DEC_MANCHESTER_ERROR;

        if ((bit_count == 4U) && (single_timing ^ triple_timing))
        {
            if (!ir_dec_store_bit_(data_bytes,
                                   sizeof(data_bytes),
                                   &bit_count,
                                   single_timing ? !level : level,
                                   IR_DEC_RC6_BITS))
            {
                return false;
            }

            status = (bit_count >= IR_DEC_RC6_BITS) ? IR_DEC_MANCHESTER_READY : IR_DEC_MANCHESTER_OK;
        }
        else if (bit_count == 5U)
        {
            if (single_timing || triple_timing)
            {
                if (triple_timing)
                {
                    timing_us = IR_DEC_RC6_BIT_US;
                }
                switch_detect = false;
                status = ir_dec_manchester_step_(data_bytes,
                                                 sizeof(data_bytes),
                                                 &bit_count,
                                                 &switch_detect,
                                                 level,
                                                 timing_us,
                                                 IR_DEC_RC6_BIT_US,
                                                 IR_DEC_RC_TOL_US,
                                                 false,
                                                 IR_DEC_RC6_BITS);
            }
            else if (ir_dec_match_(timing_us, IR_DEC_RC6_BIT_US * 2U, IR_DEC_RC_TOL_US))
            {
                status = IR_DEC_MANCHESTER_OK;
            }
            else
            {
                return false;
            }
        }
        else
        {
            status = ir_dec_manchester_step_(data_bytes,
                                             sizeof(data_bytes),
                                             &bit_count,
                                             &switch_detect,
                                             level,
                                             timing_us,
                                             IR_DEC_RC6_BIT_US,
                                             IR_DEC_RC_TOL_US,
                                             false,
                                             IR_DEC_RC6_BITS);
        }

        if (status == IR_DEC_MANCHESTER_ERROR)
        {
            return false;
        }

        if ((bit_count >= IR_DEC_RC6_BITS) || (status == IR_DEC_MANCHESTER_READY))
        {
            break;
        }

        level = !level;
    }

    if (bit_count != IR_DEC_RC6_BITS)
    {
        return false;
    }

    {
        const uint32_t raw = (uint32_t)data_bytes[0] |
                             ((uint32_t)data_bytes[1] << 8U) |
                             ((uint32_t)data_bytes[2] << 16U) |
                             ((uint32_t)data_bytes[3] << 24U);
        const bool start_bit = (raw & 0x01U) != 0U;
        const uint8_t mode = (uint8_t)((raw >> 1U) & 0x07U);
        const bool toggle = (raw & 0x10U) != 0U;

        if (!start_bit || (mode != 0U))
        {
            return false;
        }

        out_frame->protocol = IR_PROTOCOL_RC6;
        out_frame->address = ir_dec_reverse8_((uint8_t)(raw >> 5U));
        out_frame->command = ir_dec_reverse8_((uint8_t)(raw >> 13U));
        out_frame->repeat = false;

        if (out_toggle_valid != NULL)
        {
            *out_toggle_valid = true;
        }
        if (out_toggle != NULL)
        {
            *out_toggle = toggle;
        }

        return true;
    }
}

/**
 * @brief Internal helper for `ir_dec_context_store`.
 *
 * @param[in] context Parameter passed to the function.
 * @param[in] frame Parameter passed to the function.
 * @return void
 */
static void ir_dec_context_store_(ir_decoder_context_t* context, const ir_decoded_frame_t* frame)
{
    if ((context == NULL) || (frame == NULL))
    {
        return;
    }

    context->last_frame = *frame;
    context->last_frame.repeat = false;
    context->has_last_frame = true;
}

/**
 * @brief Toggles the current runtime state.
 *
 * @param[in] context Parameter passed to the function.
 * @param[in] protocol Parameter passed to the function.
 * @param[in] toggle_valid Parameter passed to the function.
 * @param[in] toggle Parameter passed to the function.
 * @return void
 */
static void ir_dec_context_store_toggle_(ir_decoder_context_t* context,
                                         ir_protocol_t protocol,
                                         bool toggle_valid,
                                         bool toggle)
{
    if ((context == NULL) || !toggle_valid)
    {
        return;
    }

    if ((protocol == IR_PROTOCOL_RC5) || (protocol == IR_PROTOCOL_RC5X))
    {
        context->rc5_toggle_valid = true;
        context->rc5_last_toggle = toggle;
    }
    else if (protocol == IR_PROTOCOL_RC6)
    {
        context->rc6_toggle_valid = true;
        context->rc6_last_toggle = toggle;
    }
}

/**
 * @brief Toggles the current runtime state.
 *
 * @param[in] context Parameter passed to the function.
 * @param[in] frame Parameter passed to the function.
 * @param[in] toggle_valid Parameter passed to the function.
 * @param[in] toggle Parameter passed to the function.
 * @return void
 */
static void ir_dec_mark_repeat_from_toggle_(ir_decoder_context_t* context,
                                            ir_decoded_frame_t* frame,
                                            bool toggle_valid,
                                            bool toggle)
{
    if ((context == NULL) || (frame == NULL) || !toggle_valid || !context->has_last_frame)
    {
        return;
    }

    if ((context->last_frame.protocol != frame->protocol) ||
        (context->last_frame.address != frame->address) ||
        (context->last_frame.command != frame->command))
    {
        frame->repeat = false;
        return;
    }

    if ((frame->protocol == IR_PROTOCOL_RC5) || (frame->protocol == IR_PROTOCOL_RC5X))
    {
        if (context->rc5_toggle_valid)
        {
            frame->repeat = toggle == context->rc5_last_toggle;
        }
    }
    else if ((frame->protocol == IR_PROTOCOL_RC6) && context->rc6_toggle_valid)
    {
        frame->repeat = toggle == context->rc6_last_toggle;
    }
}

const char* ir_protocol_name(ir_protocol_t protocol)
{
    switch (protocol)
    {
        case IR_PROTOCOL_NEC: return "NEC";
        case IR_PROTOCOL_NEC_EXT: return "NECext";
        case IR_PROTOCOL_SAMSUNG32: return "Samsung32";
        case IR_PROTOCOL_SIRC: return "SIRC";
        case IR_PROTOCOL_SIRC15: return "SIRC15";
        case IR_PROTOCOL_SIRC20: return "SIRC20";
        case IR_PROTOCOL_RC5: return "RC5";
        case IR_PROTOCOL_RC5X: return "RC5X";
        case IR_PROTOCOL_RC6: return "RC6";
        case IR_PROTOCOL_RCA: return "RCA";
        case IR_PROTOCOL_PIONEER: return "Pioneer";
        case IR_PROTOCOL_KASEIKYO: return "Kaseikyo";
        case IR_PROTOCOL_NEC42: return "NEC42";
        case IR_PROTOCOL_NEC42_EXT: return "NEC42ext";
        default: return "Unknown";
    }
}

bool ir_protocol_parse_name(const char* text, ir_protocol_t* out_protocol)
{
    ir_protocol_t protocol = IR_PROTOCOL_NONE;

    if ((text == NULL) || (out_protocol == NULL))
    {
        return false;
    }

    if (strcasecmp(text, "NEC") == 0)
    {
        protocol = IR_PROTOCOL_NEC;
    }
    else if ((strcasecmp(text, "NECext") == 0) || (strcasecmp(text, "NEC_EXT") == 0))
    {
        protocol = IR_PROTOCOL_NEC_EXT;
    }
    else if (strcasecmp(text, "Samsung32") == 0)
    {
        protocol = IR_PROTOCOL_SAMSUNG32;
    }
    else if (strcasecmp(text, "SIRC") == 0)
    {
        protocol = IR_PROTOCOL_SIRC;
    }
    else if (strcasecmp(text, "SIRC15") == 0)
    {
        protocol = IR_PROTOCOL_SIRC15;
    }
    else if (strcasecmp(text, "SIRC20") == 0)
    {
        protocol = IR_PROTOCOL_SIRC20;
    }
    else if (strcasecmp(text, "RC5") == 0)
    {
        protocol = IR_PROTOCOL_RC5;
    }
    else if (strcasecmp(text, "RC5X") == 0)
    {
        protocol = IR_PROTOCOL_RC5X;
    }
    else if (strcasecmp(text, "RC6") == 0)
    {
        protocol = IR_PROTOCOL_RC6;
    }
    else if (strcasecmp(text, "RCA") == 0)
    {
        protocol = IR_PROTOCOL_RCA;
    }
    else if (strcasecmp(text, "Pioneer") == 0)
    {
        protocol = IR_PROTOCOL_PIONEER;
    }
    else if (strcasecmp(text, "Kaseikyo") == 0)
    {
        protocol = IR_PROTOCOL_KASEIKYO;
    }
    else if (strcasecmp(text, "NEC42") == 0)
    {
        protocol = IR_PROTOCOL_NEC42;
    }
    else if ((strcasecmp(text, "NEC42ext") == 0) || (strcasecmp(text, "NEC42_EXT") == 0))
    {
        protocol = IR_PROTOCOL_NEC42_EXT;
    }

    *out_protocol = protocol;
    return protocol != IR_PROTOCOL_NONE;
}

bool ir_protocol_is_supported(ir_protocol_t protocol)
{
    return protocol != IR_PROTOCOL_NONE;
}

void ir_decoder_context_reset(ir_decoder_context_t* context)
{
    if (context != NULL)
    {
        memset(context, 0, sizeof(*context));
    }
}

bool ir_decode_any_ex(const rmt_symbol_word_t* symbols,
                      size_t symbol_count,
                      uint32_t clk_hz,
                      ir_decoder_context_t* context,
                      ir_decoded_frame_t* out_frame)
{
    ir_flat_timings_t flat = {0};
    ir_decoded_frame_t frame = {0};
    bool decoded = false;
    bool frame_toggle_valid = false;
    bool frame_toggle = false;

    if ((symbols == NULL) || (out_frame == NULL) || (symbol_count == 0U))
    {
        return false;
    }

    if (ir_dec_decode_nec_repeat_(symbols, symbol_count, clk_hz, context, &frame) ||
        ir_dec_decode_samsung32_repeat_(symbols, symbol_count, clk_hz, context, &frame))
    {
        *out_frame = frame;
        return true;
    }

    decoded = ir_dec_decode_nec_family_(symbols, symbol_count, clk_hz, &frame) ||
              ir_dec_decode_samsung32_(symbols, symbol_count, clk_hz, &frame) ||
              ir_dec_decode_sirc_(symbols, symbol_count, clk_hz, &frame) ||
              ir_dec_decode_rca_(symbols, symbol_count, clk_hz, &frame) ||
              ir_dec_decode_pioneer_(symbols, symbol_count, clk_hz, &frame) ||
              ir_dec_decode_kaseikyo_(symbols, symbol_count, clk_hz, &frame);

    if (!decoded)
    {
        if (ir_dec_flatten_timings_(symbols, symbol_count, clk_hz, &flat))
        {
            decoded = ir_dec_decode_rc5_(&flat, &frame, &frame_toggle_valid, &frame_toggle) ||
                      ir_dec_decode_rc6_(&flat, &frame, &frame_toggle_valid, &frame_toggle);
            ir_dec_flatten_free_(&flat);
        }
    }

    if (!decoded)
    {
        return false;
    }

    ir_dec_mark_repeat_from_toggle_(context, &frame, frame_toggle_valid, frame_toggle);
    ir_dec_context_store_(context, &frame);
    ir_dec_context_store_toggle_(context, frame.protocol, frame_toggle_valid, frame_toggle);
    *out_frame = frame;
    return true;
}

bool ir_decode_any(const rmt_symbol_word_t* symbols,
                   size_t symbol_count,
                   uint32_t clk_hz,
                   ir_decoded_frame_t* out_frame)
{
    return ir_decode_any_ex(symbols, symbol_count, clk_hz, NULL, out_frame);
}

bool nec_decode_ex(const rmt_symbol_word_t* symbols,
                   size_t symbol_count,
                   uint16_t* out_address,
                   uint16_t* out_command,
                   bool* out_is_extended)
{
    ir_decoded_frame_t frame = {0};

    if ((out_address == NULL) || (out_command == NULL))
    {
        return false;
    }

    if (!ir_decode_any(symbols, symbol_count, 1000000U, &frame))
    {
        return false;
    }

    if ((frame.protocol != IR_PROTOCOL_NEC) &&
        (frame.protocol != IR_PROTOCOL_NEC_EXT))
    {
        return false;
    }

    *out_address = (uint16_t)frame.address;
    *out_command = (uint16_t)(frame.command & 0xFFFFU);
    if (out_is_extended != NULL)
    {
        *out_is_extended = frame.protocol == IR_PROTOCOL_NEC_EXT;
    }

    return true;
}

bool nec_decode(const rmt_symbol_word_t* symbols,
                size_t symbol_count,
                uint16_t* out_address,
                uint16_t* out_command)
{
    return nec_decode_ex(symbols, symbol_count, out_address, out_command, NULL);
}

bool samsung32_decode(const rmt_symbol_word_t* symbols,
                      size_t symbol_count,
                      uint16_t* out_address,
                      uint16_t* out_command)
{
    ir_decoded_frame_t frame = {0};

    if ((out_address == NULL) || (out_command == NULL))
    {
        return false;
    }

    if (!ir_decode_any(symbols, symbol_count, 1000000U, &frame))
    {
        return false;
    }

    if (frame.protocol != IR_PROTOCOL_SAMSUNG32)
    {
        return false;
    }

    *out_address = (uint16_t)frame.address;
    *out_command = (uint16_t)(frame.command & 0xFFFFU);
    return true;
}
