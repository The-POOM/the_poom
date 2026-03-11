// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_OLED_SCREEN_H
#define POOM_OLED_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_DISPLAY_INVERT true
#define OLED_DISPLAY_NORMAL false
#define MAX_LINE_CHAR 16

#if CONFIG_RESOLUTION_128X64
#define MAX_PAGE 7
#else
#define MAX_PAGE 3
#endif

/**
 * @brief Initializes the OLED screen module.
 * @return void
 */
void poom_oled_screen_begin(void);

/**
 * @brief Stores a snapshot of the current framebuffer.
 * @return void
 */
void poom_oled_screen_get_last_buffer(void);

/**
 * @brief Restores the last stored framebuffer snapshot.
 * @return void
 */
void poom_oled_screen_set_last_buffer(void);

/**
 * @brief Clears the OLED screen and flushes it.
 * @return void
 */
void poom_oled_screen_clear(void);

/**
 * @brief Flushes the internal framebuffer to the display.
 * @return void
 */
void poom_oled_screen_display_show(void);

/**
 * @brief Clears the internal framebuffer without flushing.
 * @return void
 */
void poom_oled_screen_clear_buffer(void);

/**
 * @brief Draws text at the specified page and X position.
 * @param[in,out] text Text buffer to render.
 * @param[in] x Horizontal pixel offset.
 * @param[in] page Display page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text(char *text, int x, int page, bool invert);

/**
 * @brief Draws centered text on the specified page.
 * @param[in,out] text Text buffer to render.
 * @param[in] page Display page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text_center(char *text, int page, bool invert);

/**
 * @brief Clears one text line region.
 * @param[in] x Horizontal offset.
 * @param[in] page Display page index.
 * @param[in] invert Invert cleared pixels when true.
 * @return void
 */
void poom_oled_screen_clear_line(int x, int page, bool invert);

/**
 * @brief Buffers a bitmap without immediate flush.
 * @param[in] bitmap Bitmap source data.
 * @param[in] x Horizontal pixel position.
 * @param[in] y Vertical pixel position.
 * @param[in] width Bitmap width in pixels.
 * @param[in] height Bitmap height in pixels.
 * @param[in] invert Invert bitmap pixels when true.
 * @return void
 */
void poom_oled_screen_buffer_bitmap(const uint8_t *bitmap, int x, int y, int width, int height, bool invert);

/**
 * @brief Draws a bitmap into framebuffer.
 * @param[in] bitmap Bitmap source data.
 * @param[in] x Horizontal pixel position.
 * @param[in] y Vertical pixel position.
 * @param[in] width Bitmap width in pixels.
 * @param[in] height Bitmap height in pixels.
 * @param[in] invert Invert bitmap pixels when true.
 * @return void
 */
void poom_oled_screen_display_bitmap(const uint8_t *bitmap, int x, int y, int width, int height, bool invert);

/**
 * @brief Draws a bitmap followed by text on the same page.
 * @param[in] bitmap Bitmap source data.
 * @param[in,out] text Text buffer to render.
 * @param[in] page Display page index.
 * @param[in] width Bitmap width in pixels.
 * @param[in] height Bitmap height in pixels.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_bmp_text(const uint8_t *bitmap, char *text, int page, int width, int height, bool invert);

/**
 * @brief Draws a single pixel.
 * @param[in] x Horizontal pixel position.
 * @param[in] y Vertical pixel position.
 * @param[in] invert Invert pixel value when true.
 * @return void
 */
void poom_oled_screen_draw_pixel(int x, int y, bool invert);

/**
 * @brief Draws a rectangle outline.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical position.
 * @param[in] width Rectangle width.
 * @param[in] height Rectangle height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_rect(int x, int y, int width, int height, bool invert);

/**
 * @brief Draws a rounded rectangle outline.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical position.
 * @param[in] width Rectangle width.
 * @param[in] height Rectangle height.
 * @param[in] r Corner radius.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_rect_round(int x, int y, int width, int height, int r, bool invert);

/**
 * @brief Draws a generic line.
 * @param[in] x1 Start X position.
 * @param[in] y1 Start Y position.
 * @param[in] x2 End X position.
 * @param[in] y2 End Y position.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_line(int x1, int y1, int x2, int y2, bool invert);

/**
 * @brief Draws a vertical line.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical start position.
 * @param[in] height Line height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_vline(int x, int y, int height, bool invert);

/**
 * @brief Draws a horizontal line.
 * @param[in] x Horizontal start position.
 * @param[in] y Vertical position.
 * @param[in] width Line width.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_hline(int x, int y, int width, bool invert);

/**
 * @brief Draws a filled rectangle.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical position.
 * @param[in] width Box width.
 * @param[in] height Box height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_box(int x, int y, int width, int height, bool invert);

/**
 * @brief Draws the selection box used by UI menus.
 * @return void
 */
void poom_oled_screen_display_selected_item_box(void);

/**
 * @brief Draws the card border used by modal screens.
 * @return void
 */
void poom_oled_screen_display_card_border(void);

/**
 * @brief Splits long text into multiple lines and renders them.
 * @param[in,out] p_text Input text buffer.
 * @param[in,out] p_started_page Pointer to current page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text_splited(char *p_text, int *p_started_page, int invert);

/**
 * @brief Draws a horizontal loading bar.
 * @param[in] value Percentage value from 0 to 100.
 * @param[in] page Display page index.
 * @return void
 */
void poom_oled_screen_display_loading_bar(uint8_t value, uint8_t page);

/**
 * @brief Returns the configured OLED page count.
 * @return uint8_t
 */
uint8_t poom_oled_screen_get_pages(void);

#ifdef __cplusplus
}
#endif

#endif
