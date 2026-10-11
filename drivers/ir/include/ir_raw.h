// SPDX-License-Identifier: MIT
#pragma once

#include "ir_dec.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_RAW_MAX_TIMINGS 1024U
#define IR_RAW_MAX_TOTAL_US 2000000U
#define IR_RAW_MAGIC 0x31574152U /* RAW1 */

/* Versioned, pointer-free NVS representation. Timings alternate mark/space,
 * starting and ending with a mark. The receiver's idle tail is not stored. */
typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t carrier_hz;
    uint32_t duty_permille;
    uint32_t timings_us[];
} ir_raw_signal_t;

size_t ir_raw_signal_size(const ir_raw_signal_t* signal);
bool ir_raw_signal_valid(const ir_raw_signal_t* signal, size_t size);

/* Rejects full buffers (potential truncation), short noise and repeat-only
 * captures. Caller owns *out_signal and releases it with free(). */
esp_err_t ir_raw_signal_from_capture(const rmt_symbol_word_t* symbols,
                                     size_t count, size_t capacity, uint32_t clk_hz,
                                     ir_raw_signal_t** out_signal);

#ifdef __cplusplus
}
#endif
