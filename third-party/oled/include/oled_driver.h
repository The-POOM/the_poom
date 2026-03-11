/**
 * @file oled_driver.h
 * @brief High-level OLED drawing API with software framebuffer support.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "oled_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize OLED driver and clear internal page buffers. */
void oled_driver_init(oled_driver_t *dev, int width, int height);
/** @brief Flush software framebuffer to the OLED panel. */
void oled_driver_show_buffer(oled_driver_t *dev);
/** @brief Copy external buffer into internal page buffers. */
void oled_driver_set_buffer(oled_driver_t *dev, uint8_t *buffer);
/** @brief Copy internal page buffers into external buffer. */
void oled_driver_get_buffer(oled_driver_t *dev, uint8_t *buffer);
/** @brief Draw text in regular 8x8 font. */
void oled_driver_display_text(oled_driver_t *dev, int page, char *text, int x, bool invert);
/** @brief Clear internal software framebuffer. */
void oled_driver_clear_buffer(oled_driver_t *dev);
/** @brief Clear internal framebuffer and flush it to OLED. */
void oled_driver_clear_screen(oled_driver_t *dev, bool invert);
/** @brief Clear one text line region. */
void oled_driver_clear_line(oled_driver_t *dev, int page, int length, bool invert);
/** @brief Draw bitmap with arbitrary width/height alignment. */
void oled_driver_bitmaps(oled_driver_t *dev,
                         int xpos,
                         int ypos,
                         const uint8_t *bitmap,
                         int width,
                         int height,
                         bool invert);
/** @brief Draw a single pixel in software framebuffer. */
void oled_driver_draw_pixel(oled_driver_t *dev, int xpos, int ypos, bool invert);
/** @brief Draw a generic line on software framebuffer. */
void oled_driver_draw_line(oled_driver_t *dev, int x1, int y1, int x2, int y2, bool invert);
/** @brief Draw a horizontal line. */
void oled_driver_draw_hline(oled_driver_t *dev, int x, int y, int width, bool invert);
/** @brief Draw a vertical line. */
void oled_driver_draw_vline(oled_driver_t *dev, int x, int y, int height, bool invert);
/** @brief Draw a rounded rectangle outline. */
void oled_driver_draw_round_rect(oled_driver_t *dev, int x, int y, int width, int height, int r, bool invert);
/** @brief Draw a rectangle outline. */
void oled_driver_draw_rect(oled_driver_t *dev, int x, int y, int width, int height, bool invert);
/** @brief Draw fixed custom border box used by UI screens. */
void oled_driver_draw_custom_box(oled_driver_t *dev);
/** @brief Draw modal-style border box with configurable vertical anchor. */
void oled_driver_draw_modal_box(oled_driver_t *dev, int pos_x, int modal_height);

#ifdef __cplusplus
}
#endif
