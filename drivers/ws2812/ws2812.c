// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "ws2812.h"

/**
 * @file ws2812.c
 * @brief RMT-based implementation for WS2812/SK6812 LED strips.
 */

static const char *TAG = "ws2812";

#define WS2812_BITS_PER_BYTE                  (8U)
#define WS2812_DEFAULT_RESOLUTION_HZ          (10 * 1000 * 1000)
#define WS2812_MAX_BRIGHTNESS                 (255U)
#define WS2812_BRIGHTNESS_ROUNDING_OFFSET     (127U)
#define WS2812_RESET_SYMBOL_PARTS             (2U)

#define WS2812_RMT_MEM_BLOCK_SYMBOLS          (64U)
#define WS2812_RMT_TRANS_QUEUE_DEPTH          (4U)
#define WS2812_RMT_LOOP_COUNT                 (0U)
#define WS2812_ENCODER_MIN_FREE_SYMBOLS       (8U)

#define WS2812_T0H_US                         (0.35f)
#define WS2812_T0L_US                         (0.90f)
#define WS2812_T1H_US                         (0.90f)
#define WS2812_T1L_US                         (0.35f)
#define WS2812_RESET_US                       (80.0f)
#define WS2812_US_PER_SECOND                  (1000000.0f)

/**
 * @brief Timing values in RMT ticks.
 */
typedef struct {
    uint32_t t0h_ticks;
    uint32_t t0l_ticks;
    uint32_t t1h_ticks;
    uint32_t t1l_ticks;
    uint32_t treset_ticks;
} ws2812_timing_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} ws2812_tx_view_t;

typedef struct {
    rmt_symbol_word_t zero;
    rmt_symbol_word_t one;
    rmt_symbol_word_t reset;
} ws2812_symbols_t;

typedef struct {
    ws2812_symbols_t *syms;
} ws2812_cb_ctx_t;

/**
 * @brief Encoder callback that converts bytes into RMT symbols.
 *
 * @param data Transmission view.
 * @param data_size Size of transmission view object.
 * @param symbols_written Number of symbols already emitted.
 * @param symbols_free Number of free output symbols in this chunk.
 * @param symbols Output symbol buffer.
 * @param done Set to true when encoding is finished.
 * @param arg User context with prebuilt symbols.
 * @return Number of symbols written in this callback call.
 */
static size_t ws2812_encoder_cb(const void *data, size_t data_size,
                                size_t symbols_written, size_t symbols_free,
                                rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)data_size;

    ws2812_cb_ctx_t *ctx = (ws2812_cb_ctx_t *)arg;
    if (symbols_free < WS2812_ENCODER_MIN_FREE_SYMBOLS)
    {
        return 0;
    }

    const ws2812_tx_view_t *view = (const ws2812_tx_view_t *)data;
    size_t data_pos = symbols_written / WS2812_BITS_PER_BYTE;
    if (data_pos < view->size)
    {
        uint8_t byte = view->data[data_pos];
        for (int bit = (int)(WS2812_BITS_PER_BYTE - 1U); bit >= 0; --bit)
        {
            symbols[(WS2812_BITS_PER_BYTE - 1U) - (size_t)bit] =
                (byte & (1U << bit)) ? ctx->syms->one : ctx->syms->zero;
        }
        return WS2812_BITS_PER_BYTE;
    }
    else
    {
        symbols[0] = ctx->syms->reset;
        *done = true;
        return 1;
    }
}

/**
 * @brief Builds RMT symbol templates from timing values.
 *
 * @param t Timing values in ticks.
 * @param s Output symbols.
 */
static inline void build_symbols(const ws2812_timing_t *t, ws2812_symbols_t *s)
{
    s->zero.level0 = 1; s->zero.duration0 = t->t0h_ticks;
    s->zero.level1 = 0; s->zero.duration1 = t->t0l_ticks;
    s->one.level0  = 1; s->one.duration0  = t->t1h_ticks;
    s->one.level1  = 0; s->one.duration1  = t->t1l_ticks;
    uint32_t half = t->treset_ticks / WS2812_RESET_SYMBOL_PARTS;
    s->reset.level0 = 0; s->reset.duration0 = half;
    s->reset.level1 = 0; s->reset.duration1 = t->treset_ticks - half;
}

/**
 * @brief Converts microseconds to RMT ticks.
 *
 * @param us Time in microseconds.
 * @param resolution_hz RMT resolution in Hz.
 * @return Tick count.
 */
static inline uint32_t ws2812_us_to_ticks_(float us, int resolution_hz)
{
    return (uint32_t)(us * (float)resolution_hz / WS2812_US_PER_SECOND);
}

/**
 * @brief Frees strip buffers.
 *
 * @param s Strip descriptor.
 */
static void ws2812_free_buffers_(ws2812_strip_t *s)
{
    free(s->buf);
    s->buf = NULL;

    free(s->txbuf);
    s->txbuf = NULL;
}

/**
 * @brief Sets one pixel in internal GRB/GRBW buffer.
 *
 * @param s Strip descriptor.
 * @param idx Pixel index.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel.
 * @param w White channel (RGBW strips only).
 */
void ws2812_set_pixel(ws2812_strip_t *s,
                      int idx,
                      uint8_t r,
                      uint8_t g,
                      uint8_t b,
                      uint8_t w)
{
    int stride;
    uint8_t *p;

    if ((s == NULL) || (s->buf == NULL) || (idx < 0) || (idx >= s->led_count))
    {
        return;
    }

    stride = s->is_rgbw ? 4 : 3;
    p = &s->buf[idx * stride];
    p[0] = g;
    p[1] = r;
    p[2] = b;
    if (s->is_rgbw)
    {
        p[3] = w;
    }
}

/**
 * @brief Fills all pixels with one color.
 *
 * @param s Strip descriptor.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel.
 * @param w White channel (RGBW strips only).
 */
void ws2812_fill(ws2812_strip_t *s, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    if (s == NULL)
    {
        return;
    }

    for (int i = 0; i < s->led_count; ++i)
    {
        ws2812_set_pixel(s, i, r, g, b, w);
    }
}

/**
 * @brief Clears internal pixel buffer.
 *
 * @param s Strip descriptor.
 */
void ws2812_clear(ws2812_strip_t *s)
{
    int stride;
    size_t nbytes;

    if ((s == NULL) || (s->buf == NULL))
    {
        return;
    }

    stride = s->is_rgbw ? 4 : 3;
    nbytes = (size_t)s->led_count * (size_t)stride;
    memset(s->buf, 0, nbytes);
}

/**
 * @brief Sets global strip brightness.
 *
 * @param s Strip descriptor.
 * @param brightness Brightness value 0..255.
 */
void ws2812_set_brightness(ws2812_strip_t *s, uint8_t brightness)
{
    if (s == NULL)
    {
        return;
    }

    s->brightness = brightness;
}

/**
 * @brief Initializes strip and RMT TX pipeline.
 *
 * @param s Strip descriptor.
 * @param gpio_num Data output GPIO.
 * @param led_count Number of LEDs.
 * @param is_rgbw true for GRBW strips.
 * @param resolution_hz RMT resolution in Hz.
 * @return `ESP_OK` on success, error code otherwise.
 */
esp_err_t ws2812_init(ws2812_strip_t *s, int gpio_num, int led_count, bool is_rgbw, int resolution_hz)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s && led_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    memset(s, 0, sizeof(*s));
    s->gpio_num = gpio_num;
    s->led_count = led_count;
    s->is_rgbw = is_rgbw;
    s->resolution_hz = (resolution_hz > 0) ? resolution_hz : WS2812_DEFAULT_RESOLUTION_HZ;
    s->brightness = WS2812_MAX_BRIGHTNESS;

    int stride = is_rgbw ? 4 : 3;
    s->buf = (uint8_t *)calloc(led_count, stride);
    ESP_RETURN_ON_FALSE(s->buf, ESP_ERR_NO_MEM, TAG, "no mem buf");
    s->txbuf = (uint8_t *)malloc(led_count * stride);
    if (s->txbuf == NULL)
    {
        ws2812_free_buffers_(s);
        return ESP_ERR_NO_MEM;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = WS2812_RMT_MEM_BLOCK_SYMBOLS,
        .resolution_hz = s->resolution_hz,
        .trans_queue_depth = WS2812_RMT_TRANS_QUEUE_DEPTH,
    };
    err = rmt_new_tx_channel(&tx_cfg, &s->chan);
    if (err != ESP_OK)
    {
        ws2812_free_buffers_(s);
        return err;
    }

    ws2812_timing_t timing = {0};
    timing.t0h_ticks = ws2812_us_to_ticks_(WS2812_T0H_US, s->resolution_hz);
    timing.t0l_ticks = ws2812_us_to_ticks_(WS2812_T0L_US, s->resolution_hz);
    timing.t1h_ticks = ws2812_us_to_ticks_(WS2812_T1H_US, s->resolution_hz);
    timing.t1l_ticks = ws2812_us_to_ticks_(WS2812_T1L_US, s->resolution_hz);
    timing.treset_ticks = ws2812_us_to_ticks_(WS2812_RESET_US, s->resolution_hz);

    static ws2812_symbols_t symbols;
    build_symbols(&timing, &symbols);

    static ws2812_cb_ctx_t cb_ctx;
    cb_ctx.syms = &symbols;
    rmt_simple_encoder_config_t enc_cfg = {
        .callback = ws2812_encoder_cb,
        .arg = &cb_ctx,
    };
    err = rmt_new_simple_encoder(&enc_cfg, &s->enc);
    if (err != ESP_OK)
    {
        (void)rmt_del_channel(s->chan);
        s->chan = NULL;
        ws2812_free_buffers_(s);
        return err;
    }

    err = rmt_enable(s->chan);
    if (err != ESP_OK)
    {
        (void)rmt_del_encoder(s->enc);
        s->enc = NULL;
        (void)rmt_del_channel(s->chan);
        s->chan = NULL;
        ws2812_free_buffers_(s);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Deinitializes strip resources.
 *
 * @param s Strip descriptor.
 */
void ws2812_deinit(ws2812_strip_t *s)
{
    if (!s)
    {
        return;
    }

    if (s->enc)
    {
        (void)rmt_del_encoder(s->enc);
        s->enc = NULL;
    }

    if (s->chan)
    {
        (void)rmt_disable(s->chan);
        (void)rmt_del_channel(s->chan);
        s->chan = NULL;
    }

    ws2812_free_buffers_(s);
    memset(s, 0, sizeof(*s));
}

/**
 * @brief Scales one byte by brightness factor.
 *
 * @param v Input value.
 * @param br Brightness 0..255.
 * @return Scaled value.
 */
static inline uint8_t scale_byte(uint8_t v, uint8_t br)
{
    if (br >= WS2812_MAX_BRIGHTNESS)
    {
        return v;
    }

    return (uint8_t)(((uint16_t)v * (uint16_t)br + WS2812_BRIGHTNESS_ROUNDING_OFFSET) / WS2812_MAX_BRIGHTNESS);
}

/**
 * @brief Sends current frame to strip (blocking).
 *
 * @param s Strip descriptor.
 * @return `ESP_OK` on success, error code otherwise.
 */
esp_err_t ws2812_show(ws2812_strip_t *s)
{
    ESP_RETURN_ON_FALSE(s && s->enc && s->chan && s->buf, ESP_ERR_INVALID_STATE, TAG, "not inited");
    const int stride = s->is_rgbw ? 4 : 3;
    const size_t nbytes = s->led_count * stride;

    const uint8_t *tx_data = s->buf;
    ws2812_tx_view_t view = {0};

    if (s->brightness < WS2812_MAX_BRIGHTNESS)
    {
        for (size_t i = 0; i < nbytes; ++i)
        {
            s->txbuf[i] = scale_byte(s->buf[i], s->brightness);
        }
        tx_data = s->txbuf;
    }
    view.data = tx_data;
    view.size = nbytes;

    rmt_transmit_config_t tcfg = { .loop_count = WS2812_RMT_LOOP_COUNT };
    ESP_RETURN_ON_ERROR(rmt_transmit(s->chan, s->enc, &view, sizeof(view), &tcfg), TAG, "transmit");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s->chan, portMAX_DELAY), TAG, "wait");
    return ESP_OK;
}
