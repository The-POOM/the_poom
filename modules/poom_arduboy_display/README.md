# poom_arduboy_display

## Purpose
`poom_arduboy_display` provides a tiny Arduboy2-style drawing API for POOM menus on a 128x64 1bpp OLED.

It is intended as a **minimal** replacement for the common Arduboy2 APIs used by many small UI examples:
- Framebuffer in RAM (1bpp).
- Text rendering (5x7 font).
- Primitive drawing helpers (pixel/line/rect/triangle).
- A small Sprites blitter for Arduboy-style bitmaps.

## Responsibilities
- Own a 128x64 1bpp framebuffer (`1024` bytes) and flush it to the OLED over I2C.
- Provide `poom_arduboy_*` C functions used across menus.
- Provide a small `Arduboy2` C++ wrapper class so code can call `arduboy.print(...)`, etc.
- Provide a minimal `Sprites` API for overwrite/erase sprite drawing.
- Support a configurable X shift for SH1106 column alignment.

## Directory Layout

```text
modules/poom_arduboy_display/
├── CMakeLists.txt
├── README.md
├── poom_arduboy_display.cpp
├── poom_arduboy_font5x7.c
├── poom_arduboy_sprites.c
└── include/
    ├── Arduboy2.h
    ├── Sprites.h
    └── SpritesCommon.h
```

## Public API Overview

From `include/Arduboy2.h` (C API):
- `poom_arduboy_begin`
  - Initializes I2C + OLED driver and clears/flushes the screen.
- `poom_arduboy_clear`
  - Clears the framebuffer and resets cursor/text size.
- `poom_arduboy_display`
  - Flushes the framebuffer to the OLED (page-by-page).
- `poom_arduboy_set_cursor`, `poom_arduboy_set_text_size`, `poom_arduboy_print`
  - Basic text rendering (5x7 font, fixed advance).
- `poom_arduboy_delay_short`
  - Small delay helper using FreeRTOS ticks.
- Drawing primitives:
  - `poom_arduboy_draw_pixel`
  - `poom_arduboy_draw_line`
  - `poom_arduboy_draw_rect`, `poom_arduboy_fill_rect`
  - `poom_arduboy_draw_fast_vline`, `poom_arduboy_draw_fast_hline`
  - `poom_arduboy_fill_triangle`
- Bitmap helper:
  - `poom_arduboy_draw_bitmap_rows(x, y, bitmap, w, h, invert)`
    - Draws a **row-major** 1bpp bitmap (8 horizontal pixels per byte, MSB-first).
    - `w` must be a multiple of 8.
- Framebuffer access:
  - `poom_arduboy_get_buffer()`
- OLED X shift:
  - `poom_arduboy_set_x_shift(shift)`

From `include/Arduboy2.h` (C++ wrapper):
- `Arduboy2` class mirrors common calls, and `extern Arduboy2 arduboy;`

From `include/Sprites.h`:
- `Sprites_drawOverwrite(x, y, bitmap, frame)`
- `Sprites_drawErase(x, y, bitmap, frame)`
- `Sprites::drawOverwrite(...)` / `Sprites::drawErase(...)` (C++ convenience)

## Framebuffer Format
- `poom_arduboy_get_buffer()` returns a `uint8_t[1024]` buffer.
- Layout is **page-based** (vertical): each byte stores **8 vertical pixels** at the same X coordinate.
  - Index: `(y / 8) * 128 + x`
  - Bit: `1 << (y & 7)`


## Usage (Typical Menu)

```c
#include "Arduboy2.h"

void render(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(2, 2);
    poom_arduboy_print("HELLO");
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);
    poom_arduboy_display();
}
```

## Notes
- This component is **display-only** (no button/audio helpers like the full Arduboy2 library).

