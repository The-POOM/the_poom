// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "Arduboy2.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c.h"
#include "oled_driver.h"
#include "oled_transport.h"

extern const uint8_t poom_arduboy_font5x7[];

static oled_driver_t s_dev;
static uint8_t s_buffer[(ARDUBOY_WIDTH * ARDUBOY_HEIGHT) / 8];
static int16_t s_cursor_x = 0;
static int16_t s_cursor_y = 0;
static uint8_t s_text_size = 1;
static int8_t s_x_shift = (int8_t)POOM_ARDUBOY_OLED_X_SHIFT;

static inline bool in_bounds_(int16_t x, int16_t y)
{
    return (x >= 0) && (y >= 0) && (x < ARDUBOY_WIDTH) && (y < ARDUBOY_HEIGHT);
}

static inline void put_pixel_(int16_t x, int16_t y, uint8_t color)
{
    if (!in_bounds_(x, y))
    {
        return;
    }

    const uint16_t index = ((uint16_t)y >> 3) * (uint16_t)ARDUBOY_WIDTH + (uint16_t)x;
    const uint8_t bit = (uint8_t)(1u << (y & 7));

    if (color == WHITE)
    {
        s_buffer[index] |= bit;
    }
    else if (color == BLACK)
    {
        s_buffer[index] &= (uint8_t)(~bit);
    }
    else
    {
        s_buffer[index] ^= bit;
    }
}

static void draw_char5x7_(int16_t x, int16_t y, char c)
{
    if (c < 0x20 || c > 0x7E)
    {
        c = '?';
    }

    const uint16_t base = (uint16_t)(c - 0x20) * 5u;
    const uint8_t size = (s_text_size == 0u) ? 1u : s_text_size;

    for (int16_t col = 0; col < 5; col++)
    {
        const uint8_t line = poom_arduboy_font5x7[base + (uint16_t)col];
        for (int16_t row = 0; row < 7; row++)
        {
            if ((line & (1u << row)) == 0)
            {
                continue;
            }

            if (size == 1u)
            {
                put_pixel_((int16_t)(x + col), (int16_t)(y + row), WHITE);
            }
            else
            {
                const int16_t sx = (int16_t)(x + col * (int16_t)size);
                const int16_t sy = (int16_t)(y + row * (int16_t)size);
                for (uint8_t dx = 0; dx < size; dx++)
                {
                    for (uint8_t dy = 0; dy < size; dy++)
                    {
                        put_pixel_((int16_t)(sx + dx), (int16_t)(sy + dy), WHITE);
                    }
                }
            }
        }
    }
}

static void flush_page_shifted_(int page)
{
    const uint8_t *src = &s_buffer[(size_t)page * (size_t)ARDUBOY_WIDTH];

    uint8_t shifted[ARDUBOY_WIDTH];
    if (s_x_shift == 0)
    {
        (void)memcpy(shifted, src, sizeof(shifted));
    }
    else
    {
        int shift = (int)s_x_shift % ARDUBOY_WIDTH;
        if (shift < 0)
        {
            shift += ARDUBOY_WIDTH;
        }

        for (int x = 0; x < ARDUBOY_WIDTH; x++)
        {
            shifted[(x + shift) & (ARDUBOY_WIDTH - 1)] = src[x];
        }
    }

    oled_transport_display_image(&s_dev, page, 0, shifted, ARDUBOY_WIDTH);
}

extern "C" {

void poom_arduboy_set_x_shift(int8_t shift)
{
    s_x_shift = shift;
}

uint8_t *poom_arduboy_get_buffer(void)
{
    return s_buffer;
}

void poom_arduboy_set_text_size(uint8_t size)
{
    s_text_size = (size == 0u) ? 1u : size;
}

void poom_arduboy_begin(void)
{
    (void)i2c_init();
    // Not strictly required (oled_transport_init will register again), but makes the flow explicit.
    (void)i2c_register_device((uint8_t)OLED_I2C_ADDRESS_DEFAULT);

    oled_driver_init(&s_dev, ARDUBOY_WIDTH, ARDUBOY_HEIGHT);
    poom_arduboy_clear();
    poom_arduboy_display();
}

void poom_arduboy_clear(void)
{
    memset(s_buffer, 0, sizeof(s_buffer));
    s_cursor_x = 0;
    s_cursor_y = 0;
    s_text_size = 1;
}

void poom_arduboy_display(void)
{
    const int pages = (ARDUBOY_HEIGHT / 8);
    for (int page = 0; page < pages; page++)
    {
        flush_page_shifted_(page);
    }
}

void poom_arduboy_set_cursor(int16_t x, int16_t y)
{
    s_cursor_x = x;
    s_cursor_y = y;
}

size_t poom_arduboy_print(const char *s)
{
    if (s == NULL)
    {
        return 0;
    }

    const uint8_t size = (s_text_size == 0u) ? 1u : s_text_size;
    const int16_t x_advance = (int16_t)(6 * size);
    const int16_t y_advance = (int16_t)(8 * size);

    size_t written = 0;
    while (*s != '\0')
    {
        const char c = *s++;
        if (c == '\r')
        {
            continue;
        }
        if (c == '\n')
        {
            s_cursor_x = 0;
            s_cursor_y = (int16_t)(s_cursor_y + y_advance);
            continue;
        }

        if (s_cursor_x > (ARDUBOY_WIDTH - x_advance))
        {
            s_cursor_x = 0;
            s_cursor_y = (int16_t)(s_cursor_y + y_advance);
        }
        if (s_cursor_y > (ARDUBOY_HEIGHT - y_advance))
        {
            break;
        }

        draw_char5x7_(s_cursor_x, s_cursor_y, c);
        s_cursor_x = (int16_t)(s_cursor_x + x_advance);
        written++;
    }

    return written;
}

void poom_arduboy_delay_short(uint16_t ms)
{
    if (ms == 0u)
    {
        taskYIELD();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void poom_arduboy_draw_pixel(int16_t x, int16_t y, uint8_t color)
{
    put_pixel_(x, y, color);
}

void poom_arduboy_draw_fast_vline(int16_t x, int16_t y, int16_t h, uint8_t color)
{
    if (h <= 0)
    {
        return;
    }
    for (int16_t i = 0; i < h; i++)
    {
        put_pixel_(x, (int16_t)(y + i), color);
    }
}

void poom_arduboy_draw_fast_hline(int16_t x, int16_t y, int16_t w, uint8_t color)
{
    if (w <= 0)
    {
        return;
    }
    for (int16_t i = 0; i < w; i++)
    {
        put_pixel_((int16_t)(x + i), y, color);
    }
}

void poom_arduboy_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    poom_arduboy_draw_fast_hline(x, y, w, color);
    poom_arduboy_draw_fast_hline(x, (int16_t)(y + h - 1), w, color);
    poom_arduboy_draw_fast_vline(x, y, h, color);
    poom_arduboy_draw_fast_vline((int16_t)(x + w - 1), y, h, color);
}

void poom_arduboy_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color)
{
    int16_t dx = (x1 > x0) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
    int16_t dy = (y1 > y0) ? (int16_t)(y1 - y0) : (int16_t)(y0 - y1);

    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;

    int16_t err = (dx > dy ? dx : (int16_t)-dy) / 2;

    for (;;)
    {
        put_pixel_(x0, y0, color);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        const int16_t e2 = err;
        if (e2 > -dx)
        {
            err = (int16_t)(err - dy);
            x0 = (int16_t)(x0 + sx);
        }
        if (e2 < dy)
        {
            err = (int16_t)(err + dx);
            y0 = (int16_t)(y0 + sy);
        }
    }
}

void poom_arduboy_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    for (int16_t yy = 0; yy < h; yy++)
    {
        poom_arduboy_draw_fast_hline(x, (int16_t)(y + yy), w, color);
    }
}

static inline void swap_i16_(int16_t *a, int16_t *b)
{
    int16_t t = *a;
    *a = *b;
    *b = t;
}

void poom_arduboy_fill_triangle(int16_t x0,
                                int16_t y0,
                                int16_t x1,
                                int16_t y1,
                                int16_t x2,
                                int16_t y2,
                                uint8_t color)
{
    // Sort vertices by Y (y0 <= y1 <= y2)
    if (y0 > y1)
    {
        swap_i16_(&y0, &y1);
        swap_i16_(&x0, &x1);
    }
    if (y1 > y2)
    {
        swap_i16_(&y1, &y2);
        swap_i16_(&x1, &x2);
    }
    if (y0 > y1)
    {
        swap_i16_(&y0, &y1);
        swap_i16_(&x0, &x1);
    }

    if (y0 == y2)
    {
        int16_t a = x0;
        int16_t b = x1;
        int16_t c = x2;
        if (a > b) swap_i16_(&a, &b);
        if (b > c) swap_i16_(&b, &c);
        if (a > b) swap_i16_(&a, &b);
        poom_arduboy_draw_fast_hline(a, y0, (int16_t)(c - a + 1), color);
        return;
    }

    const int32_t dx01 = (int32_t)x1 - (int32_t)x0;
    const int32_t dy01 = (int32_t)y1 - (int32_t)y0;
    const int32_t dx02 = (int32_t)x2 - (int32_t)x0;
    const int32_t dy02 = (int32_t)y2 - (int32_t)y0;
    const int32_t dx12 = (int32_t)x2 - (int32_t)x1;
    const int32_t dy12 = (int32_t)y2 - (int32_t)y1;

    int32_t sa = 0;
    int32_t sb = 0;

    int16_t last = (y1 == y2) ? y1 : (int16_t)(y1 - 1);

    for (int16_t y = y0; y <= last; y++)
    {
        const int16_t a = (int16_t)((int32_t)x0 + (sa / dy01));
        const int16_t b = (int16_t)((int32_t)x0 + (sb / dy02));
        sa += dx01;
        sb += dx02;

        int16_t x_start = a;
        int16_t x_end = b;
        if (x_start > x_end) swap_i16_(&x_start, &x_end);
        poom_arduboy_draw_fast_hline(x_start, y, (int16_t)(x_end - x_start + 1), color);
    }

    sa = (int32_t)dx12 * (int32_t)(last + 1 - y1);
    sb = (int32_t)dx02 * (int32_t)(last + 1 - y0);

    for (int16_t y = (int16_t)(last + 1); y <= y2; y++)
    {
        const int16_t a = (int16_t)((int32_t)x1 + (sa / dy12));
        const int16_t b = (int16_t)((int32_t)x0 + (sb / dy02));
        sa += dx12;
        sb += dx02;

        int16_t x_start = a;
        int16_t x_end = b;
        if (x_start > x_end) swap_i16_(&x_start, &x_end);
        poom_arduboy_draw_fast_hline(x_start, y, (int16_t)(x_end - x_start + 1), color);
    }
}


void poom_arduboy_draw_bitmap_rows(int16_t x,
                                  int16_t y,
                                  const uint8_t *bitmap,
                                  int16_t width,
                                  int16_t height,
                                  bool invert)
{
    if (bitmap == NULL || width <= 0 || height <= 0)
    {
        return;
    }

    if ((width & 7) != 0)
    {
        // Must be 8-pixels aligned (same limitation as legacy oled_driver_bitmaps).
        return;
    }

    const int16_t bytes_per_row = (int16_t)(width / 8);

    for (int16_t yy = 0; yy < height; yy++)
    {
        const uint8_t *row = bitmap + (int32_t)yy * (int32_t)bytes_per_row;
        for (int16_t bx = 0; bx < bytes_per_row; bx++)
        {
            uint8_t v = row[bx];
            if (invert)
            {
                v = (uint8_t)~v;
            }

            const int16_t base_x = (int16_t)(x + bx * 8);

            // MSB-first: bit 7 is left-most pixel.
            for (int16_t bit = 0; bit < 8; bit++)
            {
                const bool on = ((v & (uint8_t)(1u << (7 - bit))) != 0u);
                poom_arduboy_draw_pixel((int16_t)(base_x + bit), (int16_t)(y + yy), on ? WHITE : BLACK);
            }
        }
    }
}

} // extern "C"

Arduboy2 arduboy;
