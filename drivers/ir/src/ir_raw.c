// SPDX-License-Identifier: MIT
#include "ir_raw.h"

#include <stdlib.h>

size_t ir_raw_signal_size(const ir_raw_signal_t* signal)
{
    return sizeof(*signal) + signal->count * sizeof(signal->timings_us[0]);
}

bool ir_raw_signal_valid(const ir_raw_signal_t* signal, size_t size)
{
    if(signal == NULL || size < sizeof(*signal) || signal->magic != IR_RAW_MAGIC ||
       signal->version != 1U || signal->count < 7U || signal->count > IR_RAW_MAX_TIMINGS ||
       (signal->count % 2U) == 0U || size != ir_raw_signal_size(signal) ||
       signal->carrier_hz < 10000U || signal->carrier_hz > 60000U ||
       signal->duty_permille == 0U || signal->duty_permille >= 1000U)
    {
        return false;
    }
    uint64_t total = 0U;
    for(size_t i = 0; i < signal->count; ++i)
    {
        if(signal->timings_us[i] == 0U) return false;
        total += signal->timings_us[i];
    }
    return total >= 3000U && total <= IR_RAW_MAX_TOTAL_US;
}

esp_err_t ir_raw_signal_from_capture(const rmt_symbol_word_t* symbols,
                                     size_t count, size_t capacity, uint32_t clk_hz,
                                     ir_raw_signal_t** out_signal)
{
    if(out_signal == NULL) return ESP_ERR_INVALID_ARG;
    *out_signal = NULL;
    if(symbols == NULL || clk_hz == 0U || count == 0U || count >= capacity)
        return ESP_ERR_INVALID_ARG;

    ir_raw_signal_t* raw = calloc(1, sizeof(*raw) + IR_RAW_MAX_TIMINGS * sizeof(uint32_t));
    if(raw == NULL) return ESP_ERR_NO_MEM;
    raw->magic = IR_RAW_MAGIC;
    raw->version = 1U;
    /* These are replay defaults, not measurements of the original carrier. */
    raw->carrier_hz = 38000U;
    raw->duty_permille = 330U;
    bool ended = false;
    for(size_t i = 0; i < count * 2U; ++i)
    {
        const rmt_symbol_word_t* symbol = &symbols[i / 2U];
        const uint32_t ticks = (i % 2U) ? symbol->duration1 : symbol->duration0;
        const bool space = (i % 2U) ? symbol->level1 : symbol->level0;
        if(ticks == 0U) { ended = true; continue; }
        if(ended) goto invalid;
        if(raw->count == 0U && space) continue;
        const uint64_t us = ((uint64_t)ticks * 1000000U) / clk_hz;
        if(us == 0U || us > IR_RAW_MAX_TOTAL_US) goto invalid;
        /* Merge adjacent halves of the same level, if any. */
        if(raw->count && space == ((raw->count % 2U) == 0U))
        {
            uint64_t merged = raw->timings_us[raw->count - 1U] + us;
            if(merged > IR_RAW_MAX_TOTAL_US) goto invalid;
            raw->timings_us[raw->count - 1U] = (uint32_t)merged;
        }
        else
        {
            if(raw->count == IR_RAW_MAX_TIMINGS) goto invalid;
            raw->timings_us[raw->count++] = (uint32_t)us;
        }
    }
    if(raw->count && (raw->count % 2U) == 0U) --raw->count;
    if(!ir_raw_signal_valid(raw, ir_raw_signal_size(raw))) goto invalid;
    *out_signal = raw;
    return ESP_OK;
invalid:
    free(raw);
    return ESP_ERR_INVALID_ARG;
}
