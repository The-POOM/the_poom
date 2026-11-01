// SPDX-License-Identifier: MIT
#include "poom_gfx.h"

#include <string.h>

#include "esp_attr.h"
#include "i2c.h"
#include "oled_driver.h"
#include "oled_transport.h"
EXT_RAM_BSS_ATTR static poom_gfx_surface_t *s_surface;

#define G(a,b,c,d,e) ((uint16_t)(((uint16_t)(a) << 12) | ((uint16_t)(b) << 9) | \
                                  ((uint16_t)(c) << 6) | ((uint16_t)(d) << 3) | (uint16_t)(e)))

/* Purpose-built compact technical glyphs.  Stored in flash, never in HP SRAM. */
static const uint16_t s_glyph[41] = {
    G(0,0,0,0,0), G(2,5,5,5,2), G(2,6,2,2,7), G(6,1,2,4,7),
    G(5,5,7,1,1), G(7,4,6,1,6), G(3,4,6,5,2), G(7,1,2,2,2),
    G(2,5,2,5,2), G(2,5,3,1,6), G(0,2,0,2,0), G(0,0,7,0,0),
    G(0,0,0,0,2), G(1,1,2,4,4), G(2,5,7,5,5), G(6,5,6,5,6),
    G(3,4,4,4,3), G(6,5,5,5,6), G(7,4,6,4,7), G(7,4,6,4,4),
    G(3,4,5,5,3), G(5,5,7,5,5), G(7,2,2,2,7), G(1,1,1,5,2),
    G(5,5,6,5,5), G(4,4,4,4,7), G(5,7,7,5,5), G(5,7,7,7,5),
    G(2,5,5,5,2), G(6,5,6,4,4), G(2,5,5,7,3), G(6,5,6,5,5),
    G(3,4,2,1,6), G(7,2,2,2,2), G(5,5,5,5,7), G(5,5,5,5,2),
    G(5,5,7,7,5), G(5,5,2,5,5), G(5,5,2,2,2), G(7,1,2,4,7),
};
#undef G

static uint16_t glyph_(char c)
{
    if((c >= 'a') && (c <= 'z')) c = (char)(c - ('a' - 'A'));
    if((c >= '0') && (c <= '9')) return s_glyph[1 + (uint8_t)(c - '0')];
    if(c == ':') return s_glyph[11];
    if(c == '-') return s_glyph[12];
    if(c == '.') return s_glyph[13];
    if(c == '/') return s_glyph[14];
    if((c >= 'A') && (c <= 'Z')) return s_glyph[15 + (uint8_t)(c - 'A')];
    return s_glyph[0];
}

void poom_gfx_open(poom_gfx_surface_t *surface)
{
    if(surface == NULL) return;
    s_surface = surface;
    (void)i2c_init();
    (void)i2c_register_device((uint8_t)OLED_I2C_ADDRESS_DEFAULT);
    oled_driver_init(&s_surface->oled, POOM_GFX_WIDTH, POOM_GFX_HEIGHT);
    poom_gfx_clear();
}

void poom_gfx_close(void) { s_surface = NULL; }
void poom_gfx_clear(void)
{
    if(s_surface != NULL) (void)memset(s_surface->framebuffer, 0, sizeof(s_surface->framebuffer));
}
void poom_gfx_present(void)
{
    if(s_surface == NULL) return;
    for(int page = 0; page < POOM_GFX_HEIGHT / 8; ++page) {
        oled_transport_display_image(&s_surface->oled, page, 0,
                                     &s_surface->framebuffer[(size_t)page * POOM_GFX_WIDTH], POOM_GFX_WIDTH);
    }
}

void poom_gfx_pixel(int16_t x, int16_t y, poom_gfx_color_t color)
{
    if((x < 0) || (y < 0) || (x >= POOM_GFX_WIDTH) || (y >= POOM_GFX_HEIGHT)) return;
    if(s_surface == NULL) return;
    uint8_t *buffer = s_surface->framebuffer;
    const uint16_t pos = (uint16_t)(y >> 3) * POOM_GFX_WIDTH + (uint16_t)x;
    const uint8_t bit = (uint8_t)(1U << (y & 7));
    if(color == POOM_GFX_SET) buffer[pos] |= bit;
    else if(color == POOM_GFX_CLEAR) buffer[pos] &= (uint8_t)~bit;
    else buffer[pos] ^= bit;
}

void poom_gfx_hline(int16_t x, int16_t y, int16_t width, poom_gfx_color_t color)
{
    for(int16_t i = 0; i < width; ++i) poom_gfx_pixel((int16_t)(x + i), y, color);
}
void poom_gfx_vline(int16_t x, int16_t y, int16_t height, poom_gfx_color_t color)
{
    for(int16_t i = 0; i < height; ++i) poom_gfx_pixel(x, (int16_t)(y + i), color);
}
void poom_gfx_box(int16_t x, int16_t y, int16_t width, int16_t height, poom_gfx_color_t color)
{
    for(int16_t yy = 0; yy < height; ++yy) poom_gfx_hline(x, (int16_t)(y + yy), width, color);
}
void poom_gfx_rect(int16_t x, int16_t y, int16_t width, int16_t height, poom_gfx_color_t color)
{
    poom_gfx_hline(x, y, width, color); poom_gfx_hline(x, (int16_t)(y + height - 1), width, color);
    poom_gfx_vline(x, y, height, color); poom_gfx_vline((int16_t)(x + width - 1), y, height, color);
}
void poom_gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, poom_gfx_color_t color)
{
    int16_t dx = (x0 < x1) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t dy = (y0 < y1) ? (int16_t)(y0 - y1) : (int16_t)(y1 - y0);
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (int16_t)(dx + dy);
    for(;;) { poom_gfx_pixel(x0, y0, color); if((x0 == x1) && (y0 == y1)) break;
        const int16_t e2 = (int16_t)(2 * err);
        if(e2 >= dy) { err = (int16_t)(err + dy); x0 = (int16_t)(x0 + sx); }
        if(e2 <= dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
    }
}
void poom_gfx_circle(int16_t x0, int16_t y0, uint8_t radius, poom_gfx_color_t color)
{
    int16_t x = 0, y = radius, d = (int16_t)(1 - radius);
    while(x <= y) {
        poom_gfx_pixel((int16_t)(x0 + x), (int16_t)(y0 + y), color); poom_gfx_pixel((int16_t)(x0 - x), (int16_t)(y0 + y), color);
        poom_gfx_pixel((int16_t)(x0 + x), (int16_t)(y0 - y), color); poom_gfx_pixel((int16_t)(x0 - x), (int16_t)(y0 - y), color);
        poom_gfx_pixel((int16_t)(x0 + y), (int16_t)(y0 + x), color); poom_gfx_pixel((int16_t)(x0 - y), (int16_t)(y0 + x), color);
        poom_gfx_pixel((int16_t)(x0 + y), (int16_t)(y0 - x), color); poom_gfx_pixel((int16_t)(x0 - y), (int16_t)(y0 - x), color);
        ++x; if(d < 0) d = (int16_t)(d + 2 * x + 1); else { --y; d = (int16_t)(d + 2 * (x - y) + 1); }
    }
}
int16_t poom_gfx_text_width(const char *text, uint8_t scale)
{
    int16_t width = 0; if((text == NULL) || (scale == 0U)) return 0;
    while(*text++ != '\0') width = (int16_t)(width + 4 * scale);
    return (width > 0) ? (int16_t)(width - scale) : 0;
}
void poom_gfx_text(int16_t x, int16_t y, const char *text, uint8_t scale, poom_gfx_color_t color)
{
    if((text == NULL) || (scale == 0U)) return;
    while(*text != '\0') { const uint16_t glyph = glyph_(*text++);
        for(int16_t row = 0; row < 5; ++row) { const uint8_t bits = (uint8_t)((glyph >> (12 - row * 3)) & 7U);
            for(int16_t col = 0; col < 3; ++col) if((bits & (uint8_t)(4U >> col)) != 0U)
                poom_gfx_box((int16_t)(x + col * scale), (int16_t)(y + row * scale), scale, scale, color);
        } x = (int16_t)(x + 4 * scale);
    }
}
void poom_gfx_tiny(int16_t x, int16_t y, const char *text, poom_gfx_color_t color)
{
    poom_gfx_text(x, y, text, 1U, color);
}
void poom_gfx_tiny_scaled(int16_t x, int16_t y, const char *text, uint8_t scale, poom_gfx_color_t color)
{
    poom_gfx_text(x, y, text, scale, color);
}

void poom_gfx_tiny_clipped(int16_t x, int16_t y, const char *text, int16_t max_width, poom_gfx_color_t color)
{
    if((text == NULL) || (max_width <= 0)) return;
    const int16_t advance = 4;
    const int16_t ellipsis_width = 3 * advance;
    int16_t available = max_width;
    size_t count = 0U;
    while(text[count] != '\0' && available >= advance) {
        available = (int16_t)(available - advance);
        ++count;
    }
    if(text[count] == '\0') {
        poom_gfx_tiny(x, y, text, color);
        return;
    }
    while((count > 0U) && ((int16_t)(count * advance + ellipsis_width) > max_width)) --count;
    for(size_t index = 0U; index < count; ++index) {
        char glyph[2] = {text[index], '\0'};
        poom_gfx_tiny((int16_t)(x + (int16_t)index * advance), y, glyph, color);
    }
    poom_gfx_tiny((int16_t)(x + (int16_t)count * advance), y, "...", color);
}
