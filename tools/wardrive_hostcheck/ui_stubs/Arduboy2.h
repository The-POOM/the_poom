#pragma once
/* Host OLED mock interface for the Wardrive UI tests. */
#include <stdint.h>
#include <stdbool.h>
#define WHITE 1
#define BLACK 0
#define INVERT 2
void poom_arduboy_clear(void);
void poom_arduboy_set_text_size(uint8_t size);
void poom_arduboy_set_cursor(int16_t x,int16_t y);
void poom_arduboy_print(const char *text);
void poom_arduboy_draw_pixel(int16_t x,int16_t y,uint8_t color);
void poom_arduboy_draw_line(int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint8_t color);
void poom_arduboy_draw_rect(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t color);
void poom_arduboy_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t color);
void poom_arduboy_display(void);
void poom_arduboy_set_display_enabled(bool enabled);
