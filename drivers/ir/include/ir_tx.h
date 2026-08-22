// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file ir_tx.h
 * @brief IR transmit (RMT TX) helper for ESP-IDF.
 */

#ifndef IR_TX_H
#define IR_TX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "ir_dec.h"

/**
 * @brief IR transmitter configuration.
 */
typedef struct
{
    int gpio; /* IR LED GPIO */
    uint32_t clk_hz; /* RMT resolution (Hz), e.g. 1 MHz */
    uint32_t carrier_hz; /* Carrier frequency, e.g. 38 kHz */
    float duty_cycle; /* Duty cycle, e.g. 0.33 */
    uint32_t mem_symbols; /* RMT memory block size in symbols (e.g. 64) */
    uint32_t queue_depth; /* RMT TX queue depth (e.g. 4) */
} ir_tx_config_t;

/**
 * @brief IR transmitter runtime handle.
 */
typedef struct
{
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t copy_encoder; /* ESP-IDF copy encoder */
    uint32_t clk_hz;
    float duty_cycle;
    uint32_t current_carrier_hz;
    rmt_symbol_word_t* symbols;
    size_t symbol_capacity;
    const char* tag;
} ir_tx_handle_t;

ir_tx_config_t ir_tx_default_config(void);

esp_err_t ir_tx_init(ir_tx_handle_t* transmitter, const ir_tx_config_t* config, const char* tag);
esp_err_t ir_tx_deinit(ir_tx_handle_t* transmitter);

esp_err_t ir_tx_send(ir_tx_handle_t* transmitter, ir_protocol_t protocol, uint32_t address, uint32_t command);

/**
 * @brief Send NEC (8-bit addr + ~addr + 8-bit cmd + ~cmd) using raw symbols.
 * @param[in,out] tx TX handle.
 * @param[in] addr 8-bit address.
 * @param[in] cmd 8-bit command.
 * @return esp_err_t
 */
esp_err_t ir_tx_nec_send(ir_tx_handle_t* transmitter, uint8_t address, uint8_t command);

/**
 * @brief Send NECext (16-bit address + 8-bit command + ~command) using raw symbols.
 * @param[in,out] transmitter TX handle.
 * @param[in] address 16-bit address (LSB first on-air).
 * @param[in] command 8-bit command.
 * @return esp_err_t
 */
esp_err_t ir_tx_nec_ext_send(ir_tx_handle_t* transmitter, uint16_t address, uint8_t command);

/**
 * @brief Send Samsung32 (16-bit address + 8-bit command + ~command) using raw symbols.
 * @param[in,out] transmitter TX handle.
 * @param[in] address 16-bit address.
 * @param[in] command 8-bit command.
 * @return esp_err_t
 */
esp_err_t ir_tx_samsung32_send(ir_tx_handle_t* transmitter, uint16_t address, uint8_t command);

#ifdef __cplusplus
}
#endif

#endif /* IR_TX_H */
