// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_rcv.c
 * @brief Implementation of `ir_rcv` (RMT RX IR capture helper).
 */

#include "ir_rcv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "soc/soc_caps.h"

#define IR_RCV_DEFAULT_TAG "ir_rcv"
#define IR_RCV_DEFAULT_CLK_HZ (1000000U)
#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
#define IR_RCV_DEFAULT_MEM_SYMBOLS ((uint32_t)SOC_RMT_MEM_WORDS_PER_CHANNEL)
#else
#define IR_RCV_DEFAULT_MEM_SYMBOLS (64U)
#endif
#define IR_RCV_DEFAULT_PULSE_MIN_NS (1250U)
#define IR_RCV_DEFAULT_PULSE_MAX_NS (12000000U)
#define IR_RCV_DEFAULT_BUFFER_SYMBOLS (64U)
#define IR_RCV_DEFAULT_QUEUE_DEPTH (1U)

#define IR_RCV_US_PER_SECOND (1000000ULL)

#ifndef IR_RCV_ENABLE_LOG
#define IR_RCV_ENABLE_LOG (1)
#endif

#if IR_RCV_ENABLE_LOG
#define IR_RCV_PRINTF_E(tag, fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", (tag), __func__, __LINE__, ##__VA_ARGS__)
#define IR_RCV_PRINTF_W(tag, fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", (tag), __func__, __LINE__, ##__VA_ARGS__)
#define IR_RCV_PRINTF_I(tag, fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", (tag), __func__, __LINE__, ##__VA_ARGS__)
#else
#define IR_RCV_PRINTF_E(...)
#define IR_RCV_PRINTF_W(...)
#define IR_RCV_PRINTF_I(...)
#endif

/**
 * @brief Internal helper for `ir_rcv_log_tag`.
 *
 * @param[in] receiver Parameter passed to the function.
 * @return const char*
 */
static const char* ir_rcv_log_tag_(const ir_rcv_handle_t* receiver)
{
    if ((receiver != NULL) && (receiver->tag != NULL))
    {
        return receiver->tag;
    }

    return IR_RCV_DEFAULT_TAG;
}

/**
 * @brief Internal helper for `ir_rcv_ticks_to_microseconds`.
 *
 * @param[in] ticks Parameter passed to the function.
 * @param[in] clk_hz Parameter passed to the function.
 * @return inline uint32_t
 */
static inline uint32_t ir_rcv_ticks_to_microseconds_(uint32_t ticks, uint32_t clk_hz)
{
    if (clk_hz == 0U)
    {
        return 0U;
    }

    return (uint32_t)(((uint64_t)ticks * IR_RCV_US_PER_SECOND) / (uint64_t)clk_hz);
}

/**
 * @brief Internal helper for `ir_rcv_rx_done_isr`.
 *
 * @param[in] channel Parameter passed to the function.
 * @param[in] event_data Parameter passed to the function.
 * @param[in] user_data Parameter passed to the function.
 * @return bool
 */
static bool ir_rcv_rx_done_isr_(rmt_channel_handle_t channel,
                                 const rmt_rx_done_event_data_t* event_data,
                                 void* user_data)
{
    (void)channel;

    QueueHandle_t event_queue = (QueueHandle_t)user_data;
    BaseType_t higher_prio_task_woken = pdFALSE;

    if ((event_queue != NULL) && (event_data != NULL))
    {
        rmt_rx_done_event_data_t event_copy = *event_data;
        (void)xQueueSendFromISR(event_queue, &event_copy, &higher_prio_task_woken);
    }

    return (higher_prio_task_woken == pdTRUE);
}

ir_rcv_config_t ir_rcv_default_config(void)
{
    return (ir_rcv_config_t){
        .gpio = -1,
        .clk_hz = IR_RCV_DEFAULT_CLK_HZ,
        .mem_symbols = IR_RCV_DEFAULT_MEM_SYMBOLS,
        .pulse_min_ns = IR_RCV_DEFAULT_PULSE_MIN_NS,
        .pulse_max_ns = IR_RCV_DEFAULT_PULSE_MAX_NS,
        .buffer_symbols = IR_RCV_DEFAULT_BUFFER_SYMBOLS,
        .queue_depth = IR_RCV_DEFAULT_QUEUE_DEPTH,
    };
}

esp_err_t ir_rcv_init(ir_rcv_handle_t* receiver, const ir_rcv_config_t* config, const char* tag)
{
    esp_err_t err;

    if ((receiver == NULL) || (config == NULL) || (config->gpio < 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(receiver, 0, sizeof(*receiver));
    receiver->tag = (tag != NULL) ? tag : IR_RCV_DEFAULT_TAG;
    receiver->clk_hz = config->clk_hz;

    receiver->buffer_symbols = config->buffer_symbols;
    receiver->buffer = (rmt_symbol_word_t*)heap_caps_calloc(receiver->buffer_symbols,
                                                        sizeof(rmt_symbol_word_t),
                                                        MALLOC_CAP_DEFAULT);
    if (receiver->buffer == NULL)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver),
                         "No mem for RX buffer (%u symbols)",
                         (unsigned)receiver->buffer_symbols);
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_channel_config_t rmt_rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = config->clk_hz,
        .mem_block_symbols = config->mem_symbols,
        .gpio_num = config->gpio,
    };

    IR_RCV_PRINTF_I(ir_rcv_log_tag_(receiver), "Creating RMT RX channel (GPIO=%d, res=%lu Hz)",
                     config->gpio, (unsigned long)config->clk_hz);

    err = rmt_new_rx_channel(&rmt_rx_cfg, &receiver->channel);
    if (err != ESP_OK)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        ir_rcv_deinit(receiver);
        return err;
    }

    receiver->queue = xQueueCreate(config->queue_depth, sizeof(rmt_rx_done_event_data_t));
    if (receiver->queue == NULL)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "Failed to create event queue");
        ir_rcv_deinit(receiver);
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = ir_rcv_rx_done_isr_,
    };

    err = rmt_rx_register_event_callbacks(receiver->channel, &callbacks, receiver->queue);
    if (err != ESP_OK)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "register callbacks failed: %s", esp_err_to_name(err));
        ir_rcv_deinit(receiver);
        return err;
    }

    err = rmt_enable(receiver->channel);
    if (err != ESP_OK)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "rmt_enable failed: %s", esp_err_to_name(err));
        ir_rcv_deinit(receiver);
        return err;
    }

    IR_RCV_PRINTF_I(ir_rcv_log_tag_(receiver), "IR receiver init OK (enabled)");
    return ESP_OK;
}

esp_err_t ir_rcv_deinit(ir_rcv_handle_t* receiver)
{
    if (receiver == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (receiver->channel != NULL)
    {
        (void)rmt_disable(receiver->channel);
        (void)rmt_del_channel(receiver->channel);
        receiver->channel = NULL;
    }

    if (receiver->queue != NULL)
    {
        vQueueDelete(receiver->queue);
        receiver->queue = NULL;
    }

    if (receiver->buffer != NULL)
    {
        heap_caps_free(receiver->buffer);
        receiver->buffer = NULL;
        receiver->buffer_symbols = 0;
    }

    return ESP_OK;
}

esp_err_t ir_rcv_start(ir_rcv_handle_t* receiver, const ir_rcv_config_t* config)
{
    esp_err_t err;

    if ((receiver == NULL) || (config == NULL) || (receiver->channel == NULL) || (receiver->buffer == NULL) || (receiver->queue == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)rmt_disable(receiver->channel);

    (void)xQueueReset(receiver->queue);

    err = rmt_enable(receiver->channel);
    if (err != ESP_OK)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    memset(receiver->buffer, 0, receiver->buffer_symbols * sizeof(rmt_symbol_word_t));

    rmt_receive_config_t receive_cfg = {
        .signal_range_min_ns = config->pulse_min_ns,
        .signal_range_max_ns = config->pulse_max_ns,
    };

    err = rmt_receive(receiver->channel,
                      receiver->buffer,
                      receiver->buffer_symbols * sizeof(rmt_symbol_word_t),
                      &receive_cfg);
    if (err != ESP_OK)
    {
        IR_RCV_PRINTF_E(ir_rcv_log_tag_(receiver), "rmt_receive failed: %s", esp_err_to_name(err));
    }

    return err;
}

bool ir_rcv_wait(ir_rcv_handle_t* receiver, rmt_rx_done_event_data_t* out, uint32_t timeout_ms)
{
    if ((receiver == NULL) || (receiver->queue == NULL) || (out == NULL))
    {
        return false;
    }

    if (xQueueReceive(receiver->queue, out, pdMS_TO_TICKS(timeout_ms)) == pdPASS)
    {
        return true;
    }

    return false;
}

void ir_rcv_dump(const ir_rcv_handle_t* receiver,
                 const rmt_rx_done_event_data_t* rx,
                 size_t max_symbols)
{
    if ((receiver == NULL) || (rx == NULL) || (rx->received_symbols == NULL))
    {
        return;
    }

    size_t symbol_count = rx->num_symbols;
    if ((max_symbols > 0U) && (symbol_count > max_symbols))
    {
        symbol_count = max_symbols;
    }

    IR_RCV_PRINTF_I(ir_rcv_log_tag_(receiver), "RX done: total_symbols=%u, printing=%u",
                     (unsigned)rx->num_symbols, (unsigned)symbol_count);

    const rmt_symbol_word_t* symbols = rx->received_symbols;

    for (size_t symbol_index = 0U; symbol_index < symbol_count; symbol_index++)
    {
        uint32_t duration0_ticks = symbols[symbol_index].duration0;
        uint32_t duration1_ticks = symbols[symbol_index].duration1;
        uint8_t level0 = (uint8_t)symbols[symbol_index].level0;
        uint8_t level1 = (uint8_t)symbols[symbol_index].level1;

        IR_RCV_PRINTF_I(ir_rcv_log_tag_(receiver),
                         "[%02u] L0=%u D0=%u ticks (%u us) | L1=%u D1=%u ticks (%u us)",
                         (unsigned)symbol_index,
                         (unsigned)level0,
                         (unsigned)duration0_ticks,
                         (unsigned)ir_rcv_ticks_to_microseconds_(duration0_ticks, receiver->clk_hz),
                         (unsigned)level1,
                         (unsigned)duration1_ticks,
                         (unsigned)ir_rcv_ticks_to_microseconds_(duration1_ticks, receiver->clk_hz));
    }
}
