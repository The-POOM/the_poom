// Minimal Arduboy2-style display API (no buttons, no audio, no SPI/SD).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Arduino compatibility: keep F("...") usable.
#ifndef F
#define F(x) (x)
#endif

// Geometry (fixed to classic Arduboy size).
#define ARDUBOY_WIDTH  128
#define ARDUBOY_HEIGHT 64

// Color constants compatible with common Arduboy sketches.
#define BLACK  0u
#define WHITE  1u
#define INVERT 2u

// Default SH1106 horizontal shift used by poom (circular, column-based).
// Override at compile time if needed.
#ifndef POOM_ARDUBOY_OLED_X_SHIFT
#ifdef OLED_X_SHIFT
#define POOM_ARDUBOY_OLED_X_SHIFT (OLED_X_SHIFT)
#else
#define POOM_ARDUBOY_OLED_X_SHIFT (0)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Lower-level C API (usable from C and C++).
void poom_arduboy_begin(void);
void poom_arduboy_clear(void);
void poom_arduboy_display(void);

void poom_arduboy_set_cursor(int16_t x, int16_t y);
void poom_arduboy_set_text_size(uint8_t size);
size_t poom_arduboy_print(const char *s);
void poom_arduboy_delay_short(uint16_t ms);

void poom_arduboy_draw_pixel(int16_t x, int16_t y, uint8_t color);
void poom_arduboy_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
void poom_arduboy_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);

void poom_arduboy_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void poom_arduboy_draw_fast_vline(int16_t x, int16_t y, int16_t h, uint8_t color);
void poom_arduboy_draw_fast_hline(int16_t x, int16_t y, int16_t w, uint8_t color);
void poom_arduboy_fill_triangle(int16_t x0,
                                int16_t y0,
                                int16_t x1,
                                int16_t y1,
                                int16_t x2,
                                int16_t y2,
                                uint8_t color);


// Draws a 1bpp bitmap stored in *row-major* format (8 horizontal pixels per byte, MSB-first),
// matching the legacy poom_oled_screen_display_bitmap/oled_driver_bitmaps expectation.
//
// Notes:
// - width must be a multiple of 8.
// - Pixels are overwritten (bitmap 0 clears to BLACK, 1 sets to WHITE).
// - When invert is true, bitmap bits are inverted before drawing.
void poom_arduboy_draw_bitmap_rows(int16_t x,
                                  int16_t y,
                                  const uint8_t *bitmap,
                                  int16_t width,
                                  int16_t height,
                                  bool invert);

// Direct access to the 1bpp framebuffer (size: 1024 bytes).
uint8_t *poom_arduboy_get_buffer(void);

// Optional runtime override for SH1106 column shift (circular).
void poom_arduboy_set_x_shift(int8_t shift);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <cstdio>

class Arduboy2
{
public:
    void begin() { poom_arduboy_begin(); }
    void clear() { poom_arduboy_clear(); }
    void display() { poom_arduboy_display(); }

    void setCursor(int16_t x, int16_t y) { poom_arduboy_set_cursor(x, y); }
    void setTextSize(uint8_t size) { poom_arduboy_set_text_size(size); }

    size_t print(const char *s) { return poom_arduboy_print(s); }
    size_t print(char c)
    {
        char buf[2] = {c, '\0'};
        return poom_arduboy_print(buf);
    }
    size_t print(int value) { return print_int_(value); }
    size_t print(unsigned int value) { return print_uint_(value); }
    size_t print(long value) { return print_long_(value); }
    size_t print(unsigned long value) { return print_ulong_(value); }

    void delayShort(uint16_t ms) { poom_arduboy_delay_short(ms); }

    void drawPixel(int16_t x, int16_t y, uint8_t color) { poom_arduboy_draw_pixel(x, y, color); }
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color)
    {
        poom_arduboy_draw_line(x0, y0, x1, y1, color);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color)
    {
        poom_arduboy_draw_rect(x, y, w, h, color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color)
    {
        poom_arduboy_fill_rect(x, y, w, h, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint8_t color)
    {
        poom_arduboy_draw_fast_vline(x, y, h, color);
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint8_t color)
    {
        poom_arduboy_draw_fast_hline(x, y, w, color);
    }
    void fillTriangle(int16_t x0,
                      int16_t y0,
                      int16_t x1,
                      int16_t y1,
                      int16_t x2,
                      int16_t y2,
                      uint8_t color)
    {
        poom_arduboy_fill_triangle(x0, y0, x1, y1, x2, y2, color);
    }

private:
    template <typename T>
    size_t print_int_(T value)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d", (int)value);
        return poom_arduboy_print(buf);
    }

    template <typename T>
    size_t print_uint_(T value)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%u", (unsigned)value);
        return poom_arduboy_print(buf);
    }

    template <typename T>
    size_t print_long_(T value)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%ld", (long)value);
        return poom_arduboy_print(buf);
    }

    template <typename T>
    size_t print_ulong_(T value)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
        return poom_arduboy_print(buf);
    }
};

extern Arduboy2 arduboy;
#endif
