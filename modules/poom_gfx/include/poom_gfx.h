// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#include "oled_def.h"

#define POOM_GFX_WIDTH 128
#define POOM_GFX_HEIGHT 64

typedef enum {
    POOM_GFX_CLEAR = 0,
    POOM_GFX_SET = 1,
    POOM_GFX_XOR = 2,
} poom_gfx_color_t;

typedef struct {
    oled_driver_t oled;
    uint8_t framebuffer[(POOM_GFX_WIDTH * POOM_GFX_HEIGHT) / 8];
} poom_gfx_surface_t;

/* Stateless 1bpp technical renderer. The caller owns the surface, allowing
 * a view to allocate it for its lifetime and release it on exit. */
void poom_gfx_open(poom_gfx_surface_t *surface);
void poom_gfx_close(void);
void poom_gfx_clear(void);
void poom_gfx_present(void);
void poom_gfx_pixel(int16_t x, int16_t y, poom_gfx_color_t color);
void poom_gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, poom_gfx_color_t color);
void poom_gfx_hline(int16_t x, int16_t y, int16_t width, poom_gfx_color_t color);
void poom_gfx_vline(int16_t x, int16_t y, int16_t height, poom_gfx_color_t color);
void poom_gfx_box(int16_t x, int16_t y, int16_t width, int16_t height, poom_gfx_color_t color);
void poom_gfx_rect(int16_t x, int16_t y, int16_t width, int16_t height, poom_gfx_color_t color);
void poom_gfx_circle(int16_t x, int16_t y, uint8_t radius, poom_gfx_color_t color);
int16_t poom_gfx_text_width(const char *text, uint8_t scale);
void poom_gfx_text(int16_t x, int16_t y, const char *text, uint8_t scale, poom_gfx_color_t color);
void poom_gfx_tiny(int16_t x, int16_t y, const char *text, poom_gfx_color_t color);
void poom_gfx_tiny_scaled(int16_t x, int16_t y, const char *text, uint8_t scale, poom_gfx_color_t color);
/* Draw text inside a fixed content region.  Whole glyphs are kept; an
 * ellipsis replaces the final characters when the label does not fit. */
void poom_gfx_tiny_clipped(int16_t x, int16_t y, const char *text, int16_t max_width, poom_gfx_color_t color);
