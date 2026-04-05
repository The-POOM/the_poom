// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

// Port of the Arduboy2 Sprites fast blitter (overwrite + erase only).

#include "Sprites.h"

#include <stdlib.h>

#include "Arduboy2.h"

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif

static inline uint8_t *buffer_(void)
{
    return poom_arduboy_get_buffer();
}

static void draw_bitmap_(int16_t x,
                         int16_t y,
                         const uint8_t *bitmap,
                         uint8_t w,
                         uint8_t h,
                         uint8_t draw_mode)
{
    if ((x + (int16_t)w) <= 0 || x > (ARDUBOY_WIDTH - 1) || (y + (int16_t)h) <= 0 || y > (ARDUBOY_HEIGHT - 1))
    {
        return;
    }
    if (bitmap == NULL)
    {
        return;
    }

    int16_t xOffset;
    int16_t ofs;
    int16_t yOffset = y & 7;
    int16_t sRow = y / 8;
    uint16_t loop_h;
    uint16_t start_h;
    uint16_t rendered_width;

    if (y < 0 && yOffset > 0)
    {
        --sRow;
    }

    xOffset = (x < 0) ? (int16_t)abs(x) : 0;

    if (x + (int16_t)w > (ARDUBOY_WIDTH - 1))
    {
        rendered_width = (uint16_t)((ARDUBOY_WIDTH - x) - xOffset);
    }
    else
    {
        rendered_width = (uint16_t)(w - (uint16_t)xOffset);
    }

    if (sRow < -1)
    {
        start_h = (uint16_t)abs(sRow) - 1u;
    }
    else
    {
        start_h = 0;
    }

    loop_h = (uint16_t)(h / 8u + ((h % 8u) > 0u ? 1u : 0u));

    if ((int16_t)loop_h + sRow > (ARDUBOY_HEIGHT / 8))
    {
        loop_h = (uint16_t)((ARDUBOY_HEIGHT / 8) - sRow);
    }

    loop_h = (uint16_t)(loop_h - start_h);

    sRow = (int16_t)(sRow + (int16_t)start_h);
    ofs = (sRow * ARDUBOY_WIDTH) + x + xOffset;
    const uint8_t *bofs = bitmap + ((uint16_t)start_h * (uint16_t)w) + (uint16_t)xOffset;

    const uint8_t mul_amt = (uint8_t)(1u << yOffset);
    uint16_t bitmap_data;

    uint8_t *buf = buffer_();

    switch (draw_mode)
    {
    case SPRITE_UNMASKED: {
        const uint16_t mask_data = (uint16_t)~(0xFFu * mul_amt);

        for (uint8_t a = 0; a < loop_h; ++a)
        {
            for (uint8_t iCol = 0; iCol < rendered_width; ++iCol)
            {
                bitmap_data = (uint16_t)pgm_read_byte(bofs) * (uint16_t)mul_amt;

                if (sRow >= 0)
                {
                    uint8_t data = buf[ofs];
                    data &= (uint8_t)mask_data;
                    data |= (uint8_t)bitmap_data;
                    buf[ofs] = data;
                }

                if (yOffset != 0 && sRow < 7)
                {
                    uint8_t data = buf[ofs + ARDUBOY_WIDTH];
                    data &= *((const uint8_t *)(&mask_data) + 1);
                    data |= *((const uint8_t *)(&bitmap_data) + 1);
                    buf[ofs + ARDUBOY_WIDTH] = data;
                }

                ++ofs;
                ++bofs;
            }

            ++sRow;
            bofs += (uint16_t)w - rendered_width;
            ofs += ARDUBOY_WIDTH - (int16_t)rendered_width;
        }
        break;
    }

    case SPRITE_IS_MASK_ERASE:
        for (uint8_t a = 0; a < loop_h; ++a)
        {
            for (uint8_t iCol = 0; iCol < rendered_width; ++iCol)
            {
                bitmap_data = (uint16_t)pgm_read_byte(bofs) * (uint16_t)mul_amt;
                if (sRow >= 0)
                {
                    buf[ofs] &= (uint8_t)~(uint8_t)bitmap_data;
                }
                if (yOffset != 0 && sRow < 7)
                {
                    buf[ofs + ARDUBOY_WIDTH] &= (uint8_t)~(*((const uint8_t *)(&bitmap_data) + 1));
                }
                ++ofs;
                ++bofs;
            }
            ++sRow;
            bofs += (uint16_t)w - rendered_width;
            ofs += ARDUBOY_WIDTH - (int16_t)rendered_width;
        }
        break;

    default:
        break;
    }
}

static void draw_(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame, uint8_t drawMode)
{
    if (bitmap == NULL)
    {
        return;
    }

    const uint8_t width = pgm_read_byte(bitmap);
    const uint8_t height = pgm_read_byte(bitmap + 1);
    const uint8_t *data = bitmap + 2;

    if (frame > 0)
    {
        const uint16_t frame_offset = (uint16_t)width * (uint16_t)(height / 8u + (height % 8u == 0u ? 0u : 1u));
        data += (uint16_t)frame * frame_offset;
    }

    draw_bitmap_(x, y, data, width, height, drawMode);
}

void Sprites_drawOverwrite(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame)
{
    draw_(x, y, bitmap, frame, SPRITE_OVERWRITE);
}

void Sprites_drawErase(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame)
{
    draw_(x, y, bitmap, frame, SPRITE_IS_MASK_ERASE);
}
