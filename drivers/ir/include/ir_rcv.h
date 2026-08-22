// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_rcv.h
 * @brief IR receive (RMT RX) helper for ESP-IDF.
 *
 * This component wraps the ESP-IDF RMT RX driver to capture raw IR pulse symbols
 * (`rmt_symbol_word_t`). It does not decode protocols (NEC/RC5/etc). Use
 * `ir_rcv_dump()` to print captured symbols for debugging or feed the symbols
 * into your own decoder.
 *
 * Typical flow:
 * - `ir_rcv_init()`
 * - loop:
 *   - `ir_rcv_start()`
 *   - `ir_rcv_wait()`
 *   - `ir_rcv_dump()` (optional)
 * - `ir_rcv_deinit()`
 */

#ifndef IR_RCV_H
#define IR_RCV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief IR receiver configuration.
 */
typedef struct
{
    int gpio; /* GPIO connected to the IR receiver output */
    uint32_t clk_hz; /* RMT resolution in Hz (e.g. 1,000,000 for 1 MHz) */
    uint32_t mem_symbols; /* RMT memory block size in symbols (e.g. 64) */
    uint32_t pulse_min_ns; /* Reject pulses shorter than this (ns) */
    uint32_t pulse_max_ns; /* Reject pulses longer than this (ns) */
    size_t buffer_symbols; /* RX buffer size in symbols */
    uint32_t queue_depth; /* Event queue depth (usually 1 is enough) */
} ir_rcv_config_t;

/**
 * @brief IR receiver runtime handle.
 *
 * Owns the RMT channel, the FreeRTOS queue and the symbol buffer.
 */
typedef struct
{
    rmt_channel_handle_t channel; /* RMT RX channel handle */
    QueueHandle_t queue; /* Queue of rmt_rx_done_event_data_t events */

    rmt_symbol_word_t* buffer; /* Symbol buffer owned by this handle */
    size_t buffer_symbols; /* Buffer size in symbols */

    uint32_t clk_hz; /* Copy of cfg->clk_hz (used for time conversions) */
    const char* tag; /* Optional log tag (e.g. "IR") */
} ir_rcv_handle_t;

/**
 * @brief Get a reasonable default configuration.
 *
 * You MUST set cfg.gpio before calling ir_rcv_init().
 */
ir_rcv_config_t ir_rcv_default_config(void);

/**
 * @brief Initialize the IR receiver.
 *
 * Creates:
 *  - RMT RX channel
 *  - internal symbol buffer
 *  - event queue
 * Registers the RX-done callback and enables the channel.
 *
 * @param[out] rcv     Handle to initialize.
 * @param[in]  cfg     Configuration.
 * @param[in]  tag     Log tag (can be NULL).
 * @return ESP_OK on success.
 */
esp_err_t ir_rcv_init(ir_rcv_handle_t* receiver, const ir_rcv_config_t* config, const char* tag);

/**
 * @brief Deinitialize the IR receiver and free all resources.
 *
 * @param[in,out] rcv Handle to deinitialize.
 * @return ESP_OK on success.
 */
esp_err_t ir_rcv_deinit(ir_rcv_handle_t* receiver);

/**
 * @brief Start one receive operation (non-blocking).
 *
 * The RMT driver will write captured symbols into rcv->buffer.
 * When reception completes, the RX-done callback pushes an event to the queue.
 *
 * This function is written to be robust across timeouts: it forces a known
 * state by calling rmt_disable() then rmt_enable() before rmt_receive().
 *
 * @param[in,out] rcv Handle.
 * @param[in]     cfg Configuration (pulse_min/max are used here).
 * @return ESP_OK on success.
 */
esp_err_t ir_rcv_start(ir_rcv_handle_t* receiver, const ir_rcv_config_t* config);

/**
 * @brief Wait for a receive-done event.
 *
 * @param[in]  rcv        Handle.
 * @param[out] out        Output event data.
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return true if an event was received, false on timeout/error.
 */
bool ir_rcv_wait(ir_rcv_handle_t* receiver, rmt_rx_done_event_data_t* out, uint32_t timeout_ms);

/**
 * @brief Print captured symbols (debug helper).
 *
 * Prints pairs (level0/duration0, level1/duration1) and converts ticks to microseconds
 * using the configured clk_hz.
 *
 * @param[in] rcv         Handle (used for clk_hz and tag).
 * @param[in] rx          RX done event.
 * @param[in] max_symbols 0 = print all, otherwise print first N symbols.
 */
void ir_rcv_dump(const ir_rcv_handle_t* receiver, const rmt_rx_done_event_data_t* rx, size_t max_symbols);

#ifdef __cplusplus
}
#endif

#endif /* IR_RCV_H */
