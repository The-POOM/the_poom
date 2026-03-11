// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef WS2812_H
#define WS2812_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/rmt_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ws2812.h
 * @brief RMT-based WS2812/SK6812 LED strip driver API.
 */

/**
 * @brief WS2812/SK6812 strip runtime descriptor.
 */
typedef struct {
    rmt_channel_handle_t chan; /**< RMT TX channel handle. */
    rmt_encoder_handle_t enc;  /**< RMT encoder handle. */
    int gpio_num;              /**< Data output GPIO. */
    int led_count;             /**< Number of LEDs in the strip. */
    bool is_rgbw;              /**< true for SK6812 (GRBW), false for WS2812 (GRB). */
    int resolution_hz;         /**< RMT resolution, for example 10 MHz. */
    uint8_t brightness;        /**< Global brightness 0..255 (applied on transmit). */
    uint8_t *buf;              /**< Pixel buffer in GRB/GRBW order. */
    uint8_t *txbuf;            /**< Scratch TX buffer used for brightness scaling. */
} ws2812_strip_t;

/**
 * @brief Initializes a WS2812/SK6812 strip.
 *
 * @param s Strip descriptor to initialize.
 * @param gpio_num Data output GPIO.
 * @param led_count Number of LEDs in the chain.
 * @param is_rgbw true for SK6812 (GRBW), false for WS2812 (GRB).
 * @param resolution_hz RMT resolution in Hz (for example `10 * 1000 * 1000`).
 * @return `ESP_OK` on success, error code otherwise.
 */
esp_err_t ws2812_init(ws2812_strip_t *s, int gpio_num, int led_count, bool is_rgbw, int resolution_hz);

/**
 * @brief Deinitializes the strip and releases allocated resources.
 *
 * @param s Strip descriptor.
 */
void ws2812_deinit(ws2812_strip_t *s);

/**
 * @brief Sets one pixel color.
 *
 * @param s Strip descriptor.
 * @param idx Pixel index.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel.
 * @param w White channel (used only when `is_rgbw == true`).
 */
void ws2812_set_pixel(ws2812_strip_t *s, int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t w);

/**
 * @brief Fill all pixels with the same color.
 *
 * @param s Strip descriptor.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel.
 * @param w White channel (used only when `is_rgbw == true`).
 */
void ws2812_fill(ws2812_strip_t *s, uint8_t r, uint8_t g, uint8_t b, uint8_t w);

/**
 * @brief Clear all pixels (off).
 *
 * @param s Strip descriptor.
 */
void ws2812_clear(ws2812_strip_t *s);

/**
 * @brief Sets global brightness.
 *
 * Scaling is applied on transmit; internal pixel buffer values are not modified.
 *
 * @param s Strip descriptor.
 * @param brightness Brightness value 0..255 (`255` = full brightness).
 */
void ws2812_set_brightness(ws2812_strip_t *s, uint8_t brightness);

/**
 * @brief Transmits the current pixel buffer to the LED strip.
 *
 * This is a blocking call and includes reset/latch timing automatically.
 *
 * @param s Strip descriptor.
 * @return `ESP_OK` on success, error code otherwise.
 */
esp_err_t ws2812_show(ws2812_strip_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PERIPH_WS2812_MIN_H */
