// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_oled_screen.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "oled_driver.h"

static oled_driver_t s_poom_oled_screen_dev;
static SemaphoreHandle_t s_poom_oled_screen_mutex = NULL;
static uint8_t *s_poom_oled_screen_last_buffer = NULL;

/**
 * @brief Locks OLED shared resources.
 * @return void
 */
static void poom_oled_screen_lock_(void)
{
    if(s_poom_oled_screen_mutex != NULL)
    {
        (void)xSemaphoreTake(s_poom_oled_screen_mutex, portMAX_DELAY);
    }
}

/**
 * @brief Unlocks OLED shared resources.
 * @return void
 */
static void poom_oled_screen_unlock_(void)
{
    if(s_poom_oled_screen_mutex != NULL)
    {
        (void)xSemaphoreGive(s_poom_oled_screen_mutex);
    }
}

/**
 * @brief Initializes the OLED screen module.
 * @return void
 */
void poom_oled_screen_begin(void)
{
    if(s_poom_oled_screen_mutex == NULL)
    {
        s_poom_oled_screen_mutex = xSemaphoreCreateMutex();
    }

#if CONFIG_FLIP
    s_poom_oled_screen_dev._flip = true;
#endif

#if CONFIG_RESOLUTION_128X64
    oled_driver_init(&s_poom_oled_screen_dev, 128, 64);
#elif CONFIG_RESOLUTION_128X32
    oled_driver_init(&s_poom_oled_screen_dev, 128, 32);
#else
    oled_driver_init(&s_poom_oled_screen_dev, 128, 64);
#endif
}

/**
 * @brief Stores a snapshot of the current framebuffer.
 * @return void
 */
void poom_oled_screen_get_last_buffer(void)
{
    const size_t buffer_size = (size_t)s_poom_oled_screen_dev._pages * (size_t)s_poom_oled_screen_dev._width;

    if(buffer_size == 0U)
    {
        return;
    }

    poom_oled_screen_lock_();

    if(s_poom_oled_screen_last_buffer != NULL)
    {
        free(s_poom_oled_screen_last_buffer);
        s_poom_oled_screen_last_buffer = NULL;
    }

    s_poom_oled_screen_last_buffer = (uint8_t *)malloc(buffer_size);
    if(s_poom_oled_screen_last_buffer != NULL)
    {
        oled_driver_get_buffer(&s_poom_oled_screen_dev, s_poom_oled_screen_last_buffer);
    }

    poom_oled_screen_unlock_();
}

/**
 * @brief Restores the last stored framebuffer snapshot.
 * @return void
 */
void poom_oled_screen_set_last_buffer(void)
{
    poom_oled_screen_lock_();

    if(s_poom_oled_screen_last_buffer != NULL)
    {
        oled_driver_set_buffer(&s_poom_oled_screen_dev, s_poom_oled_screen_last_buffer);
        oled_driver_show_buffer(&s_poom_oled_screen_dev);
        free(s_poom_oled_screen_last_buffer);
        s_poom_oled_screen_last_buffer = NULL;
    }

    poom_oled_screen_unlock_();
}

/**
 * @brief Clears the OLED screen and flushes it.
 * @return void
 */
void poom_oled_screen_clear(void)
{
    poom_oled_screen_lock_();
    oled_driver_clear_screen(&s_poom_oled_screen_dev, OLED_DISPLAY_NORMAL);
    poom_oled_screen_unlock_();
}

/**
 * @brief Flushes the internal framebuffer to the display.
 * @return void
 */
void poom_oled_screen_display_show(void)
{
    poom_oled_screen_lock_();
    oled_driver_show_buffer(&s_poom_oled_screen_dev);
    poom_oled_screen_unlock_();
}

/**
 * @brief Clears the internal framebuffer without flushing.
 * @return void
 */
void poom_oled_screen_clear_buffer(void)
{
    poom_oled_screen_lock_();
    oled_driver_clear_buffer(&s_poom_oled_screen_dev);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws text at the specified page and X position.
 * @param[in,out] text Text buffer to render.
 * @param[in] x Horizontal pixel offset.
 * @param[in] page Display page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text(char *text, int x, int page, bool invert)
{
    uint8_t x_offset;

    if(text == NULL)
    {
        return;
    }

    x_offset = ((x + ((int)strlen(text) * 8)) > 128) ? 0U : (uint8_t)x;

    poom_oled_screen_lock_();
    oled_driver_display_text(&s_poom_oled_screen_dev, page, text, x_offset, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws centered text on the specified page.
 * @param[in,out] text Text buffer to render.
 * @param[in] page Display page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text_center(char *text, int page, bool invert)
{
    const int text_length = (text != NULL) ? (int)strlen(text) : 0;

    if(text == NULL)
    {
        return;
    }

    if(text_length > MAX_LINE_CHAR)
    {
        poom_oled_screen_display_text(text, 0, page, invert);
        return;
    }

    poom_oled_screen_display_text(text, (128 / 2) - ((text_length * 8) / 2), page, invert);
}

/**
 * @brief Clears one text line region.
 * @param[in] x Horizontal offset.
 * @param[in] page Display page index.
 * @param[in] invert Invert cleared pixels when true.
 * @return void
 */
void poom_oled_screen_clear_line(int x, int page, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_clear_line(&s_poom_oled_screen_dev, x, page, invert);
    poom_oled_screen_unlock_();
}

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
void poom_oled_screen_buffer_bitmap(const uint8_t *bitmap, int x, int y, int width, int height, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_bitmaps(&s_poom_oled_screen_dev, x, y, bitmap, width, height, invert);
    poom_oled_screen_unlock_();
}

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
void poom_oled_screen_display_bitmap(const uint8_t *bitmap, int x, int y, int width, int height, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_bitmaps(&s_poom_oled_screen_dev, x, y, bitmap, width, height, invert);
    poom_oled_screen_unlock_();
}

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
void poom_oled_screen_display_bmp_text(const uint8_t *bitmap, char *text, int page, int width, int height, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_bitmaps(&s_poom_oled_screen_dev, 0, (page * height), bitmap, width, height, !invert);
    oled_driver_display_text(&s_poom_oled_screen_dev, page, text, width + 8, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a single pixel.
 * @param[in] x Horizontal pixel position.
 * @param[in] y Vertical pixel position.
 * @param[in] invert Invert pixel value when true.
 * @return void
 */
void poom_oled_screen_draw_pixel(int x, int y, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_draw_pixel(&s_poom_oled_screen_dev, x, y, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a rectangle outline.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical position.
 * @param[in] width Rectangle width.
 * @param[in] height Rectangle height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_rect(int x, int y, int width, int height, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_draw_rect(&s_poom_oled_screen_dev, x, y, width, height, invert);
    poom_oled_screen_unlock_();
}

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
void poom_oled_screen_draw_rect_round(int x, int y, int width, int height, int r, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_draw_round_rect(&s_poom_oled_screen_dev, x, y, width, height, r, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a generic line.
 * @param[in] x1 Start X position.
 * @param[in] y1 Start Y position.
 * @param[in] x2 End X position.
 * @param[in] y2 End Y position.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_line(int x1, int y1, int x2, int y2, bool invert)
{
    (void)y1;
    poom_oled_screen_lock_();
    oled_driver_draw_line(&s_poom_oled_screen_dev, x1, y2, x2, y2, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a vertical line.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical start position.
 * @param[in] height Line height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_vline(int x, int y, int height, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_draw_vline(&s_poom_oled_screen_dev, x, y, height, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a horizontal line.
 * @param[in] x Horizontal start position.
 * @param[in] y Vertical position.
 * @param[in] width Line width.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_hline(int x, int y, int width, bool invert)
{
    poom_oled_screen_lock_();
    oled_driver_draw_hline(&s_poom_oled_screen_dev, x, y, width, invert);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws a filled rectangle.
 * @param[in] x Horizontal position.
 * @param[in] y Vertical position.
 * @param[in] width Box width.
 * @param[in] height Box height.
 * @param[in] invert Invert drawn pixels when true.
 * @return void
 */
void poom_oled_screen_draw_box(int x, int y, int width, int height, bool invert)
{
    poom_oled_screen_lock_();
    while(height != 0)
    {
        oled_driver_draw_hline(&s_poom_oled_screen_dev, x, y++, width, invert);
        height--;
    }
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws the selection box used by UI menus.
 * @return void
 */
void poom_oled_screen_display_selected_item_box(void)
{
    poom_oled_screen_lock_();
    oled_driver_draw_custom_box(&s_poom_oled_screen_dev);
    poom_oled_screen_unlock_();
}

/**
 * @brief Draws the card border used by modal screens.
 * @return void
 */
void poom_oled_screen_display_card_border(void)
{
    poom_oled_screen_lock_();
    oled_driver_draw_modal_box(&s_poom_oled_screen_dev, 0, 3);
    poom_oled_screen_unlock_();
}

/**
 * @brief Splits long text into multiple lines and renders them.
 * @param[in,out] p_text Input text buffer.
 * @param[in,out] p_started_page Pointer to current page index.
 * @param[in] invert Invert text pixels when true.
 * @return void
 */
void poom_oled_screen_display_text_splited(char *p_text, int *p_started_page, int invert)
{
    char text_copy[100];
    char current_line[MAX_LINE_CHAR + 1];
    char *token;

    if((p_text == NULL) || (p_started_page == NULL))
    {
        return;
    }

    if(strlen(p_text) <= MAX_LINE_CHAR)
    {
        poom_oled_screen_display_text(p_text, 3, *p_started_page, invert);
        (*p_started_page)++;
        return;
    }

    memset(current_line, 0, sizeof(current_line));
    strncpy(text_copy, p_text, sizeof(text_copy) - 1);
    text_copy[sizeof(text_copy) - 1] = '\0';

    token = strtok(text_copy, " ");
    while(token != NULL)
    {
        if((strlen(current_line) + strlen(token) + 1U) <= MAX_LINE_CHAR)
        {
            if(strlen(current_line) > 0U)
            {
                strncat(current_line, " ", sizeof(current_line) - strlen(current_line) - 1U);
            }
            strncat(current_line, token, sizeof(current_line) - strlen(current_line) - 1U);
        }
        else
        {
            poom_oled_screen_display_text(current_line, 3, *p_started_page, invert);
            (*p_started_page)++;
            strncpy(current_line, token, sizeof(current_line) - 1U);
            current_line[sizeof(current_line) - 1U] = '\0';
        }
        token = strtok(NULL, " ");
    }

    if(strlen(current_line) > 0U)
    {
        poom_oled_screen_display_text(current_line, 3, *p_started_page, invert);
        (*p_started_page)++;
    }
}

/**
 * @brief Draws a horizontal loading bar.
 * @param[in] value Percentage value from 0 to 100.
 * @param[in] page Display page index.
 * @return void
 */
void poom_oled_screen_display_loading_bar(uint8_t value, uint8_t page)
{
    uint8_t bar_bitmap[8][16];
    uint8_t active_cols;

    if(value > 100U)
    {
        value = 100U;
    }

    active_cols = (uint8_t)(((uint32_t)value * 128U) / 100U);
    memset(bar_bitmap, 0, sizeof(bar_bitmap));

    for(int y = 0; y < 8; y++)
    {
        for(uint8_t x = 0; x < active_cols; x++)
        {
            bar_bitmap[y][x / 8U] |= (uint8_t)(1U << (7U - (x % 8U)));
        }
    }

    poom_oled_screen_display_bitmap((const uint8_t *)bar_bitmap, 0, (int)(page * 8U), 128, 8, OLED_DISPLAY_NORMAL);
}

/**
 * @brief Returns the configured OLED page count.
 * @return uint8_t
 */
uint8_t poom_oled_screen_get_pages(void)
{
    return (uint8_t)s_poom_oled_screen_dev._pages;
}
