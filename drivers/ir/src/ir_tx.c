// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_tx.c
 * @brief Implementation of `ir_tx` (RMT TX IR helper).
 */

#include "ir_tx.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "soc/soc_caps.h"

#define IR_TX_DEFAULT_TAG "ir_tx"
#define IR_TX_DEFAULT_CLK_HZ (1000000U)
#define IR_TX_DEFAULT_CARRIER_HZ (38000U)
#define IR_TX_DEFAULT_DUTY_CYCLE (0.33f)
#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
#define IR_TX_DEFAULT_MEM_SYMBOLS ((uint32_t)SOC_RMT_MEM_WORDS_PER_CHANNEL)
#else
#define IR_TX_DEFAULT_MEM_SYMBOLS (64U)
#endif
#define IR_TX_DEFAULT_QUEUE_DEPTH (4U)
#define IR_TX_MAX_SYMBOL_CAPACITY (128U)

#define IR_TX_US_PER_SECOND (1000000ULL)
#define IR_TX_RMT_LEVEL_HIGH (1U)
#define IR_TX_RMT_LEVEL_LOW (0U)
#define IR_TX_NEC_INV_MASK (0xFFU)

#define IR_TX_NEC_HDR_MARK_US (9000U)
#define IR_TX_NEC_HDR_SPACE_US (4500U)
#define IR_TX_NEC_BIT_MARK_US (560U)
#define IR_TX_NEC_ONE_SPACE_US (1690U)
#define IR_TX_NEC_ZERO_SPACE_US (560U)
#define IR_TX_NEC_STOP_MARK_US (560U)

#define IR_TX_SAMSUNG32_HDR_MARK_US (4500U)
#define IR_TX_SAMSUNG32_HDR_SPACE_US (4500U)
#define IR_TX_SAMSUNG32_BIT_MARK_US (550U)
#define IR_TX_SAMSUNG32_ONE_SPACE_US (1650U)
#define IR_TX_SAMSUNG32_ZERO_SPACE_US (550U)
#define IR_TX_SAMSUNG32_STOP_MARK_US (550U)

#define IR_TX_SIRC_HDR_MARK_US (2400U)
#define IR_TX_SIRC_HDR_SPACE_US (600U)
#define IR_TX_SIRC_ONE_MARK_US (1200U)
#define IR_TX_SIRC_ZERO_MARK_US (600U)
#define IR_TX_SIRC_SPACE_US (600U)

#define IR_TX_RCA_HDR_MARK_US (4000U)
#define IR_TX_RCA_HDR_SPACE_US (4000U)
#define IR_TX_RCA_BIT_MARK_US (500U)
#define IR_TX_RCA_ONE_SPACE_US (2000U)
#define IR_TX_RCA_ZERO_SPACE_US (1000U)

#define IR_TX_PIONEER_HDR_MARK_US (8500U)
#define IR_TX_PIONEER_HDR_SPACE_US (4225U)
#define IR_TX_PIONEER_BIT_MARK_US (500U)
#define IR_TX_PIONEER_ONE_SPACE_US (1500U)
#define IR_TX_PIONEER_ZERO_SPACE_US (500U)

#define IR_TX_KASEIKYO_HDR_MARK_US (3360U)
#define IR_TX_KASEIKYO_HDR_SPACE_US (1665U)
#define IR_TX_KASEIKYO_BIT_MARK_US (420U)
#define IR_TX_KASEIKYO_ONE_SPACE_US (1274U)
#define IR_TX_KASEIKYO_ZERO_SPACE_US (420U)

#define IR_TX_RC5_BIT_US (889U)
#define IR_TX_RC6_HDR_MARK_US (2666U)
#define IR_TX_RC6_HDR_SPACE_US (889U)
#define IR_TX_RC6_BIT_US (444U)

#ifndef IR_TX_ENABLE_LOG
#define IR_TX_ENABLE_LOG (1)
#endif

#if IR_TX_ENABLE_LOG
#define IR_TX_PRINTF_E(tag, fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", (tag), __func__, __LINE__, ##__VA_ARGS__)
#define IR_TX_PRINTF_I(tag, fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", (tag), __func__, __LINE__, ##__VA_ARGS__)
#else
#define IR_TX_PRINTF_E(...)
#define IR_TX_PRINTF_I(...)
#endif

typedef struct
{
    rmt_symbol_word_t* symbols;
    size_t count;
    size_t capacity;
} ir_tx_waveform_builder_t;

/**
 * @brief Internal helper for `ir_tx_log_tag`.
 *
 * @param[in] tx Parameter passed to the function.
 * @return const char*
 */
static const char* ir_tx_log_tag_(const ir_tx_handle_t* tx)
{
    if ((tx != NULL) && (tx->tag != NULL))
    {
        return tx->tag;
    }

    return IR_TX_DEFAULT_TAG;
}

/**
 * @brief Internal helper for `ir_tx_us_to_rmt_ticks`.
 *
 * @param[in] us Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @return inline uint32_t
 */
static inline uint32_t ir_tx_us_to_rmt_ticks_(uint32_t us, uint32_t clk_hz)
{
    return (uint32_t)(((uint64_t)us * (uint64_t)clk_hz) / IR_TX_US_PER_SECOND);
}

/**
 * @brief Internal helper for `ir_tx_reverse8`.
 *
 * @param[in] value Parameter passed to the function.
 * @return inline uint8_t
 */
static inline uint8_t ir_tx_reverse8_(uint8_t value)
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
 * @brief Internal helper for `ir_tx_samsung32_normalize_address`.
 *
 * @param[in] address Parameter passed to the function.
 * @return inline uint16_t
 */
static inline uint16_t ir_tx_samsung32_normalize_address_(uint32_t address)
{
    const uint16_t addr16 = (uint16_t)(address & 0xFFFFU);
    const uint8_t low = (uint8_t)(addr16 & 0xFFU);
    const uint8_t high = (uint8_t)(addr16 >> 8U);

    if (high == 0U)
    {
        return (uint16_t)((uint16_t)low | ((uint16_t)low << 8U));
    }

    return addr16;
}

/**
 * @brief Internal helper for `ir_tx_waveform_reset`.
 *
 * @param[in] builder Parameter passed to the function.
 * @return void
 */
static void ir_tx_waveform_reset_(ir_tx_waveform_builder_t* builder)
{
    if (builder == NULL)
    {
        return;
    }

    builder->count = 0U;
    if (builder->symbols != NULL)
    {
        memset(builder->symbols, 0, builder->capacity * sizeof(builder->symbols[0]));
    }
}

/**
 * @brief Internal helper for `ir_tx_waveform_append`.
 *
 * @param[in] builder Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @param[in] level Parameter passed to the function.
 * @param[in] duration_us Parameter passed to the function.
 * @return bool
 */
static bool ir_tx_waveform_append_(ir_tx_waveform_builder_t* builder,
                                   uint32_t clk_hz,
                                   bool level,
                                   uint32_t duration_us)
{
    rmt_symbol_word_t* current = NULL;
    const uint32_t ticks = ir_tx_us_to_rmt_ticks_(duration_us, clk_hz);

    if ((builder == NULL) || (builder->symbols == NULL) || (duration_us == 0U))
    {
        return duration_us == 0U;
    }

    if (builder->count > 0U)
    {
        current = &builder->symbols[builder->count - 1U];

        if ((current->duration1 == 0U) && (current->duration0 > 0U) && ((bool)current->level0 == level))
        {
            current->duration0 = (uint16_t)(current->duration0 + ticks);
            return true;
        }

        if ((current->duration1 > 0U) && ((bool)current->level1 == level))
        {
            current->duration1 = (uint16_t)(current->duration1 + ticks);
            return true;
        }

        if (current->duration0 > 0U && current->duration1 == 0U)
        {
            current->level1 = level ? IR_TX_RMT_LEVEL_HIGH : IR_TX_RMT_LEVEL_LOW;
            current->duration1 = (uint16_t)ticks;
            return true;
        }
    }

    if (builder->count >= builder->capacity)
    {
        return false;
    }

    current = &builder->symbols[builder->count++];
    memset(current, 0, sizeof(*current));
    current->level0 = level ? IR_TX_RMT_LEVEL_HIGH : IR_TX_RMT_LEVEL_LOW;
    current->duration0 = (uint16_t)ticks;
    return true;
}

/**
 * @brief Internal helper for `ir_tx_apply_carrier`.
 *
 * @param[in] transmitter Parameter passed to the function.
 * @param[in] carrier_hz Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ir_tx_apply_carrier_(ir_tx_handle_t* transmitter, uint32_t carrier_hz)
{
    esp_err_t err;
    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = carrier_hz,
        .duty_cycle = transmitter->duty_cycle,
    };

    if ((transmitter == NULL) || (transmitter->channel == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (transmitter->current_carrier_hz == carrier_hz)
    {
        return ESP_OK;
    }

    err = rmt_apply_carrier(transmitter->channel, &carrier_cfg);
    if (err == ESP_OK)
    {
        transmitter->current_carrier_hz = carrier_hz;
    }

    return err;
}

/**
 * @brief Internal helper for `ir_tx_protocol_carrier_hz`.
 *
 * @param[in] protocol Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t ir_tx_protocol_carrier_hz_(ir_protocol_t protocol)
{
    switch (protocol)
    {
        case IR_PROTOCOL_SIRC:
        case IR_PROTOCOL_SIRC15:
        case IR_PROTOCOL_SIRC20:
            return 40000U;
        case IR_PROTOCOL_RC5:
        case IR_PROTOCOL_RC5X:
        case IR_PROTOCOL_RC6:
            return 36000U;
        default:
            return 38000U;
    }
}

/**
 * @brief Internal helper for `ir_tx_transmit_builder`.
 *
 * @param[in] transmitter Parameter passed to the function.
 * @param[in] builder Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ir_tx_transmit_builder_(ir_tx_handle_t* transmitter,
                                         const ir_tx_waveform_builder_t* builder)
{
    esp_err_t err;
    rmt_transmit_config_t transmit_cfg = {.loop_count = 0};

    if ((transmitter == NULL) || (builder == NULL) || (builder->count == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)rmt_disable(transmitter->channel);
    err = rmt_enable(transmitter->channel);
    if (err != ESP_OK)
    {
        return err;
    }

    return rmt_transmit(transmitter->channel,
                        transmitter->copy_encoder,
                        builder->symbols,
                        builder->count * sizeof(builder->symbols[0]),
                        &transmit_cfg);
}

/**
 * @brief Internal helper for `ir_tx_emit_pdwm_space_frame`.
 *
 * @param[in] transmitter Parameter passed to the function.
 * @param[in] bits_lsb Parameter passed to the function.
 * @param[in] bit_count Parameter passed to the function.
 * @param[in] header_mark_us Parameter passed to the function.
 * @param[in] header_space_us Parameter passed to the function.
 * @param[in] bit_mark_us Parameter passed to the function.
 * @param[in] one_space_us Parameter passed to the function.
 * @param[in] zero_space_us Parameter passed to the function.
 * @param[in] stop_mark_us Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ir_tx_emit_pdwm_space_frame_(ir_tx_handle_t* transmitter,
                                              uint64_t bits_lsb,
                                              size_t bit_count,
                                              uint32_t header_mark_us,
                                              uint32_t header_space_us,
                                              uint32_t bit_mark_us,
                                              uint32_t one_space_us,
                                              uint32_t zero_space_us,
                                              uint32_t stop_mark_us)
{
    ir_tx_waveform_builder_t builder = {
        .symbols = transmitter->symbols,
        .capacity = transmitter->symbol_capacity,
    };

    ir_tx_waveform_reset_(&builder);

    if (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, true, header_mark_us) ||
        !ir_tx_waveform_append_(&builder, transmitter->clk_hz, false, header_space_us))
    {
        return ESP_ERR_NO_MEM;
    }

    for (size_t bit_index = 0U; bit_index < bit_count; bit_index++)
    {
        const bool bit_value = ((bits_lsb >> bit_index) & 0x1ULL) != 0ULL;

        if (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, true, bit_mark_us) ||
            !ir_tx_waveform_append_(&builder,
                                    transmitter->clk_hz,
                                    false,
                                    bit_value ? one_space_us : zero_space_us))
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (stop_mark_us > 0U)
    {
        if (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, true, stop_mark_us))
        {
            return ESP_ERR_NO_MEM;
        }
    }

    return ir_tx_transmit_builder_(transmitter, &builder);
}

/**
 * @brief Internal helper for `ir_tx_emit_pdwm_mark_frame`.
 *
 * @param[in] transmitter Parameter passed to the function.
 * @param[in] bits_lsb Parameter passed to the function.
 * @param[in] bit_count Parameter passed to the function.
 * @param[in] header_mark_us Parameter passed to the function.
 * @param[in] header_space_us Parameter passed to the function.
 * @param[in] one_mark_us Parameter passed to the function.
 * @param[in] zero_mark_us Parameter passed to the function.
 * @param[in] bit_space_us Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ir_tx_emit_pdwm_mark_frame_(ir_tx_handle_t* transmitter,
                                             uint64_t bits_lsb,
                                             size_t bit_count,
                                             uint32_t header_mark_us,
                                             uint32_t header_space_us,
                                             uint32_t one_mark_us,
                                             uint32_t zero_mark_us,
                                             uint32_t bit_space_us)
{
    ir_tx_waveform_builder_t builder = {
        .symbols = transmitter->symbols,
        .capacity = transmitter->symbol_capacity,
    };

    ir_tx_waveform_reset_(&builder);

    if (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, true, header_mark_us) ||
        !ir_tx_waveform_append_(&builder, transmitter->clk_hz, false, header_space_us))
    {
        return ESP_ERR_NO_MEM;
    }

    for (size_t bit_index = 0U; bit_index < bit_count; bit_index++)
    {
        const bool bit_value = ((bits_lsb >> bit_index) & 0x1ULL) != 0ULL;

        if (!ir_tx_waveform_append_(&builder,
                                    transmitter->clk_hz,
                                    true,
                                    bit_value ? one_mark_us : zero_mark_us) ||
            !ir_tx_waveform_append_(&builder, transmitter->clk_hz, false, bit_space_us))
        {
            return ESP_ERR_NO_MEM;
        }
    }

    return ir_tx_transmit_builder_(transmitter, &builder);
}

/**
 * @brief Internal helper for `ir_tx_emit_manchester_bits`.
 *
 * @param[in] transmitter Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] bit_count Parameter passed to the function.
 * @param[in] half_bit_us Parameter passed to the function.
 * @param[in] header_mark_us Parameter passed to the function.
 * @param[in] header_space_us Parameter passed to the function.
 * @param[in] special_double_bit_index Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ir_tx_emit_manchester_bits_(ir_tx_handle_t* transmitter,
                                             const uint8_t* data,
                                             size_t bit_count,
                                             uint32_t half_bit_us,
                                             uint32_t header_mark_us,
                                             uint32_t header_space_us,
                                             int special_double_bit_index)
{
    ir_tx_waveform_builder_t builder = {
        .symbols = transmitter->symbols,
        .capacity = transmitter->symbol_capacity,
    };

    ir_tx_waveform_reset_(&builder);

    if ((header_mark_us > 0U) &&
        (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, true, header_mark_us) ||
         !ir_tx_waveform_append_(&builder, transmitter->clk_hz, false, header_space_us)))
    {
        return ESP_ERR_NO_MEM;
    }

    for (size_t bit_index = 0U; bit_index < bit_count; bit_index++)
    {
        const size_t byte_index = bit_index / 8U;
        const uint8_t shift = (uint8_t)(bit_index % 8U);
        const bool value = (data[byte_index] & (1U << shift)) != 0U;
        uint32_t first_half_us = half_bit_us;
        uint32_t second_half_us = half_bit_us;

        if (((int)bit_index) == special_double_bit_index)
        {
            first_half_us *= 2U;
            second_half_us *= 2U;
        }

        if (!ir_tx_waveform_append_(&builder, transmitter->clk_hz, !value, first_half_us) ||
            !ir_tx_waveform_append_(&builder, transmitter->clk_hz, value, second_half_us))
        {
            return ESP_ERR_NO_MEM;
        }
    }

    return ir_tx_transmit_builder_(transmitter, &builder);
}

ir_tx_config_t ir_tx_default_config(void)
{
    return (ir_tx_config_t){
        .gpio = -1,
        .clk_hz = IR_TX_DEFAULT_CLK_HZ,
        .carrier_hz = IR_TX_DEFAULT_CARRIER_HZ,
        .duty_cycle = IR_TX_DEFAULT_DUTY_CYCLE,
        .mem_symbols = IR_TX_DEFAULT_MEM_SYMBOLS,
        .queue_depth = IR_TX_DEFAULT_QUEUE_DEPTH,
    };
}

esp_err_t ir_tx_init(ir_tx_handle_t* transmitter, const ir_tx_config_t* config, const char* tag)
{
    esp_err_t err;
    uint32_t mem_block_symbols;

    if ((transmitter == NULL) || (config == NULL) || (config->gpio < 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(transmitter, 0, sizeof(*transmitter));
    transmitter->tag = (tag != NULL) ? tag : IR_TX_DEFAULT_TAG;
    transmitter->clk_hz = config->clk_hz;
    transmitter->duty_cycle = config->duty_cycle;
    transmitter->current_carrier_hz = 0U;
    transmitter->symbol_capacity = IR_TX_MAX_SYMBOL_CAPACITY;
    transmitter->symbols = (rmt_symbol_word_t*)heap_caps_calloc(transmitter->symbol_capacity,
                                                                sizeof(transmitter->symbols[0]),
                                                                MALLOC_CAP_DEFAULT);
    if (transmitter->symbols == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    mem_block_symbols = config->mem_symbols;
    if (mem_block_symbols < 64U)
    {
        mem_block_symbols = 64U;
    }

#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
    if (mem_block_symbols > (uint32_t)SOC_RMT_MEM_WORDS_PER_CHANNEL)
    {
        mem_block_symbols = (uint32_t)SOC_RMT_MEM_WORDS_PER_CHANNEL;
    }
#endif

    {
        rmt_tx_channel_config_t channel_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = config->clk_hz,
            .mem_block_symbols = mem_block_symbols,
            .trans_queue_depth = config->queue_depth,
            .gpio_num = config->gpio,
        };

        err = rmt_new_tx_channel(&channel_cfg, &transmitter->channel);
        if (err != ESP_OK)
        {
            IR_TX_PRINTF_E(ir_tx_log_tag_(transmitter), "rmt_new_tx_channel: %s", esp_err_to_name(err));
            ir_tx_deinit(transmitter);
            return err;
        }
    }

    {
        rmt_copy_encoder_config_t copy_cfg = {};

        err = rmt_new_copy_encoder(&copy_cfg, &transmitter->copy_encoder);
        if (err != ESP_OK)
        {
            IR_TX_PRINTF_E(ir_tx_log_tag_(transmitter), "rmt_new_copy_encoder: %s", esp_err_to_name(err));
            ir_tx_deinit(transmitter);
            return err;
        }
    }

    err = ir_tx_apply_carrier_(transmitter, config->carrier_hz);
    if (err != ESP_OK)
    {
        IR_TX_PRINTF_E(ir_tx_log_tag_(transmitter), "rmt_apply_carrier: %s", esp_err_to_name(err));
        ir_tx_deinit(transmitter);
        return err;
    }

    err = rmt_enable(transmitter->channel);
    if (err != ESP_OK)
    {
        IR_TX_PRINTF_E(ir_tx_log_tag_(transmitter), "rmt_enable: %s", esp_err_to_name(err));
        ir_tx_deinit(transmitter);
        return err;
    }

    IR_TX_PRINTF_I(ir_tx_log_tag_(transmitter),
                   "TX init OK (gpio=%d, res=%luHz, carrier=%luHz)",
                   config->gpio,
                   (unsigned long)config->clk_hz,
                   (unsigned long)config->carrier_hz);
    return ESP_OK;
}

esp_err_t ir_tx_deinit(ir_tx_handle_t* transmitter)
{
    if (transmitter == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (transmitter->copy_encoder != NULL)
    {
        (void)rmt_del_encoder(transmitter->copy_encoder);
        transmitter->copy_encoder = NULL;
    }

    if (transmitter->channel != NULL)
    {
        (void)rmt_disable(transmitter->channel);
        (void)rmt_del_channel(transmitter->channel);
        transmitter->channel = NULL;
    }

    if (transmitter->symbols != NULL)
    {
        heap_caps_free(transmitter->symbols);
        transmitter->symbols = NULL;
        transmitter->symbol_capacity = 0U;
    }

    return ESP_OK;
}

esp_err_t ir_tx_nec_send(ir_tx_handle_t* transmitter, uint8_t address, uint8_t command)
{
    uint64_t frame_lsb = 0U;

    if ((transmitter == NULL) || (transmitter->channel == NULL) || (transmitter->copy_encoder == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    frame_lsb |= (uint64_t)address;
    frame_lsb |= ((uint64_t)(address ^ IR_TX_NEC_INV_MASK) << 8U);
    frame_lsb |= ((uint64_t)command << 16U);
    frame_lsb |= ((uint64_t)(command ^ IR_TX_NEC_INV_MASK) << 24U);

    return ir_tx_emit_pdwm_space_frame_(transmitter,
                                        frame_lsb,
                                        32U,
                                        IR_TX_NEC_HDR_MARK_US,
                                        IR_TX_NEC_HDR_SPACE_US,
                                        IR_TX_NEC_BIT_MARK_US,
                                        IR_TX_NEC_ONE_SPACE_US,
                                        IR_TX_NEC_ZERO_SPACE_US,
                                        IR_TX_NEC_STOP_MARK_US);
}

esp_err_t ir_tx_nec_ext_send(ir_tx_handle_t* transmitter, uint16_t address, uint8_t command)
{
    uint64_t frame_lsb = 0U;

    if ((transmitter == NULL) || (transmitter->channel == NULL) || (transmitter->copy_encoder == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    frame_lsb |= (uint64_t)(address & 0xFFU);
    frame_lsb |= ((uint64_t)((address >> 8U) & 0xFFU) << 8U);
    frame_lsb |= ((uint64_t)command << 16U);
    frame_lsb |= ((uint64_t)(command ^ IR_TX_NEC_INV_MASK) << 24U);

    return ir_tx_emit_pdwm_space_frame_(transmitter,
                                        frame_lsb,
                                        32U,
                                        IR_TX_NEC_HDR_MARK_US,
                                        IR_TX_NEC_HDR_SPACE_US,
                                        IR_TX_NEC_BIT_MARK_US,
                                        IR_TX_NEC_ONE_SPACE_US,
                                        IR_TX_NEC_ZERO_SPACE_US,
                                        IR_TX_NEC_STOP_MARK_US);
}

esp_err_t ir_tx_samsung32_send(ir_tx_handle_t* transmitter, uint16_t address, uint8_t command)
{
    uint64_t frame_lsb = 0U;
    const uint16_t normalized_address = ir_tx_samsung32_normalize_address_(address);

    if ((transmitter == NULL) || (transmitter->channel == NULL) || (transmitter->copy_encoder == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    frame_lsb |= (uint64_t)(normalized_address & 0xFFU);
    frame_lsb |= ((uint64_t)((normalized_address >> 8U) & 0xFFU) << 8U);
    frame_lsb |= ((uint64_t)command << 16U);
    frame_lsb |= ((uint64_t)(command ^ IR_TX_NEC_INV_MASK) << 24U);

    return ir_tx_emit_pdwm_space_frame_(transmitter,
                                        frame_lsb,
                                        32U,
                                        IR_TX_SAMSUNG32_HDR_MARK_US,
                                        IR_TX_SAMSUNG32_HDR_SPACE_US,
                                        IR_TX_SAMSUNG32_BIT_MARK_US,
                                        IR_TX_SAMSUNG32_ONE_SPACE_US,
                                        IR_TX_SAMSUNG32_ZERO_SPACE_US,
                                        IR_TX_SAMSUNG32_STOP_MARK_US);
}

esp_err_t ir_tx_send(ir_tx_handle_t* transmitter, ir_protocol_t protocol, uint32_t address, uint32_t command)
{
    esp_err_t err;
    uint32_t log_address = address;

    if ((transmitter == NULL) || (transmitter->channel == NULL) || (transmitter->copy_encoder == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = ir_tx_apply_carrier_(transmitter, ir_tx_protocol_carrier_hz_(protocol));
    if (err != ESP_OK)
    {
        return err;
    }

    switch (protocol)
    {
        case IR_PROTOCOL_NEC:
            err = ir_tx_nec_send(transmitter, (uint8_t)(address & 0xFFU), (uint8_t)(command & 0xFFU));
            break;

        case IR_PROTOCOL_NEC_EXT:
            err = ir_tx_nec_ext_send(transmitter, (uint16_t)(address & 0xFFFFU), (uint8_t)(command & 0xFFU));
            break;

        case IR_PROTOCOL_SAMSUNG32:
            log_address = ir_tx_samsung32_normalize_address_(address);
            err = ir_tx_samsung32_send(transmitter, (uint16_t)(address & 0xFFFFU), (uint8_t)(command & 0xFFU));
            break;

        case IR_PROTOCOL_NEC42:
        {
            const uint32_t addr13 = address & 0x1FFFU;
            const uint8_t cmd8 = (uint8_t)(command & 0xFFU);
            const uint64_t bits = (uint64_t)addr13 |
                                  ((uint64_t)(~addr13 & 0x1FFFU) << 13U) |
                                  ((uint64_t)cmd8 << 26U) |
                                  ((uint64_t)((uint8_t)~cmd8) << 34U);
            err = ir_tx_emit_pdwm_space_frame_(transmitter,
                                               bits,
                                               42U,
                                               IR_TX_NEC_HDR_MARK_US,
                                               IR_TX_NEC_HDR_SPACE_US,
                                               IR_TX_NEC_BIT_MARK_US,
                                               IR_TX_NEC_ONE_SPACE_US,
                                               IR_TX_NEC_ZERO_SPACE_US,
                                               IR_TX_NEC_STOP_MARK_US);
            break;
        }

        case IR_PROTOCOL_NEC42_EXT:
        {
            const uint64_t bits = ((uint64_t)address & 0x3FFFFFFULL) |
                                  (((uint64_t)command & 0xFFFFULL) << 26U);
            err = ir_tx_emit_pdwm_space_frame_(transmitter,
                                               bits,
                                               42U,
                                               IR_TX_NEC_HDR_MARK_US,
                                               IR_TX_NEC_HDR_SPACE_US,
                                               IR_TX_NEC_BIT_MARK_US,
                                               IR_TX_NEC_ONE_SPACE_US,
                                               IR_TX_NEC_ZERO_SPACE_US,
                                               IR_TX_NEC_STOP_MARK_US);
            break;
        }

        case IR_PROTOCOL_SIRC:
        case IR_PROTOCOL_SIRC15:
        case IR_PROTOCOL_SIRC20:
        {
            uint64_t bits = 0U;
            size_t bit_count = 12U;
            uint32_t addr_mask = 0x1FU;

            if (protocol == IR_PROTOCOL_SIRC15)
            {
                bit_count = 15U;
                addr_mask = 0xFFU;
            }
            else if (protocol == IR_PROTOCOL_SIRC20)
            {
                bit_count = 20U;
                addr_mask = 0x1FFFU;
            }

            bits = ((uint64_t)(command & 0x7FU)) | (((uint64_t)address & addr_mask) << 7U);
            err = ir_tx_emit_pdwm_mark_frame_(transmitter,
                                              bits,
                                              bit_count,
                                              IR_TX_SIRC_HDR_MARK_US,
                                              IR_TX_SIRC_HDR_SPACE_US,
                                              IR_TX_SIRC_ONE_MARK_US,
                                              IR_TX_SIRC_ZERO_MARK_US,
                                              IR_TX_SIRC_SPACE_US);
            break;
        }

        case IR_PROTOCOL_RCA:
        {
            const uint64_t bits = ((uint64_t)address & 0x0FULL) |
                                  (((uint64_t)command & 0xFFULL) << 4U) |
                                  (((uint64_t)(~address) & 0x0FULL) << 12U) |
                                  (((uint64_t)(~command) & 0xFFULL) << 16U);
            err = ir_tx_emit_pdwm_space_frame_(transmitter,
                                               bits,
                                               24U,
                                               IR_TX_RCA_HDR_MARK_US,
                                               IR_TX_RCA_HDR_SPACE_US,
                                               IR_TX_RCA_BIT_MARK_US,
                                               IR_TX_RCA_ONE_SPACE_US,
                                               IR_TX_RCA_ZERO_SPACE_US,
                                               0U);
            break;
        }

        case IR_PROTOCOL_PIONEER:
        {
            const uint8_t addr8 = (uint8_t)(address & 0xFFU);
            const uint8_t cmd8 = (uint8_t)(command & 0xFFU);
            const uint64_t bits = (uint64_t)addr8 |
                                  ((uint64_t)(addr8 ^ 0xFFU) << 8U) |
                                  ((uint64_t)cmd8 << 16U) |
                                  ((uint64_t)(cmd8 ^ 0xFFU) << 24U);
            err = ir_tx_emit_pdwm_space_frame_(transmitter,
                                               bits,
                                               32U,
                                               IR_TX_PIONEER_HDR_MARK_US,
                                               IR_TX_PIONEER_HDR_SPACE_US,
                                               IR_TX_PIONEER_BIT_MARK_US,
                                               IR_TX_PIONEER_ONE_SPACE_US,
                                               IR_TX_PIONEER_ZERO_SPACE_US,
                                               0U);
            break;
        }

        case IR_PROTOCOL_KASEIKYO:
        {
            uint8_t data[6] = {0};
            uint8_t vendor_parity = 0U;
            const uint16_t vendor_id = (uint16_t)((address >> 8U) & 0xFFFFU);
            const uint8_t genre1 = (uint8_t)((address >> 4U) & 0x0FU);
            const uint8_t genre2 = (uint8_t)(address & 0x0FU);
            const uint8_t id = (uint8_t)((address >> 24U) & 0x03U);
            const uint16_t data_cmd = (uint16_t)(command & 0x03FFU);
            uint64_t bits = 0U;

            data[0] = (uint8_t)(vendor_id & 0xFFU);
            data[1] = (uint8_t)(vendor_id >> 8U);
            vendor_parity = (uint8_t)(data[0] ^ data[1]);
            vendor_parity = (uint8_t)((vendor_parity & 0x0FU) ^ (vendor_parity >> 4U));
            data[2] = (uint8_t)((genre1 << 4U) | (vendor_parity & 0x0FU));
            data[3] = (uint8_t)(genre2 | ((data_cmd & 0x0FU) << 4U));
            data[4] = (uint8_t)((id << 6U) | ((data_cmd >> 4U) & 0x3FU));
            data[5] = (uint8_t)(data[2] ^ data[3] ^ data[4]);

            for (size_t index = 0U; index < sizeof(data); index++)
            {
                bits |= ((uint64_t)data[index] << (index * 8U));
            }

            err = ir_tx_emit_pdwm_space_frame_(transmitter,
                                               bits,
                                               48U,
                                               IR_TX_KASEIKYO_HDR_MARK_US,
                                               IR_TX_KASEIKYO_HDR_SPACE_US,
                                               IR_TX_KASEIKYO_BIT_MARK_US,
                                               IR_TX_KASEIKYO_ONE_SPACE_US,
                                               IR_TX_KASEIKYO_ZERO_SPACE_US,
                                               0U);
            break;
        }

        case IR_PROTOCOL_RC5:
        case IR_PROTOCOL_RC5X:
        {
            uint8_t data[2] = {0};
            uint16_t raw = 0U;

            raw |= 0x01U;
            if (protocol == IR_PROTOCOL_RC5)
            {
                raw |= 0x02U;
            }
            raw |= (uint16_t)((ir_tx_reverse8_((uint8_t)(address & 0x1FU)) >> 3U) << 3U);
            raw |= (uint16_t)((ir_tx_reverse8_((uint8_t)(command & 0x3FU)) >> 2U) << 8U);
            data[0] = (uint8_t)~(raw & 0xFFU);
            data[1] = (uint8_t)~((raw >> 8U) & 0xFFU);

            err = ir_tx_emit_manchester_bits_(transmitter, data, 14U, IR_TX_RC5_BIT_US, 0U, 0U, -1);
            break;
        }

        case IR_PROTOCOL_RC6:
        {
            uint8_t data[3] = {0};
            uint32_t raw = 0U;

            raw |= 0x01U;
            raw |= ((uint32_t)ir_tx_reverse8_((uint8_t)address) << 5U);
            raw |= ((uint32_t)ir_tx_reverse8_((uint8_t)command) << 13U);
            data[0] = (uint8_t)(raw & 0xFFU);
            data[1] = (uint8_t)((raw >> 8U) & 0xFFU);
            data[2] = (uint8_t)((raw >> 16U) & 0xFFU);

            err = ir_tx_emit_manchester_bits_(transmitter,
                                              data,
                                              21U,
                                              IR_TX_RC6_BIT_US,
                                              IR_TX_RC6_HDR_MARK_US,
                                              IR_TX_RC6_HDR_SPACE_US,
                                              4);
            break;
        }

        default:
            err = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    if (err == ESP_OK)
    {
        IR_TX_PRINTF_I(ir_tx_log_tag_(transmitter),
                       "IR sent proto=%s addr=0x%08lX cmd=0x%08lX",
                       ir_protocol_name(protocol),
                       (unsigned long)log_address,
                       (unsigned long)command);
    }
    else
    {
        IR_TX_PRINTF_E(ir_tx_log_tag_(transmitter),
                       "IR send failed proto=%s: %s",
                       ir_protocol_name(protocol),
                       esp_err_to_name(err));
    }

    return err;
}
