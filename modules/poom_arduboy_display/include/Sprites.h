// Minimal Sprites API (display-only) compatible with Arduboy-style bitmaps.
#pragma once

#include <stdint.h>

#include "SpritesCommon.h"

#ifdef __cplusplus
extern "C" {
#endif

// C functions (usable from both C and C++).
void Sprites_drawOverwrite(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);
void Sprites_drawErase(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);

#ifdef __cplusplus
} // extern "C"

// C++ convenience wrapper to keep `Sprites::drawOverwrite(...)` working.
class Sprites
{
public:
    static inline void drawOverwrite(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame)
    {
        Sprites_drawOverwrite(x, y, bitmap, frame);
    }

    static inline void drawErase(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame)
    {
        Sprites_drawErase(x, y, bitmap, frame);
    }
};
#endif
