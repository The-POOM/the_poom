# ws2812

`ws2812` is a minimal RMT-based driver for WS2812/SK6812 LED strips.

## Purpose

- Initialize an RMT TX channel for LED waveform generation.
- Store pixel data in GRB/GRBW order.
- Transmit frames with optional global brightness scaling.

## Structure

```text
drivers/ws2812
├── CMakeLists.txt
├── component.mk
├── README.md
├── ws2812.c
└── include/
    └── ws2812.h
```

## Dependencies

Defined in `drivers/ws2812/CMakeLists.txt`:

- `driver`
- `esp_driver_rmt`

## Public API

Header: `drivers/ws2812/include/ws2812.h`

```c
esp_err_t ws2812_init(ws2812_strip_t *s, int gpio_num, int led_count,
                      bool is_rgbw, int resolution_hz);
void ws2812_deinit(ws2812_strip_t *s);
void ws2812_set_pixel(ws2812_strip_t *s, int idx,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void ws2812_fill(ws2812_strip_t *s, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void ws2812_clear(ws2812_strip_t *s);
void ws2812_set_brightness(ws2812_strip_t *s, uint8_t brightness);
esp_err_t ws2812_show(ws2812_strip_t *s);
```

## Runtime Notes

- RGB strips use 3 bytes/pixel (GRB); RGBW strips use 4 bytes/pixel (GRBW).
- Brightness is applied during `ws2812_show()` (the source buffer keeps raw values).
- Timing constants are centralized in `ws2812.c` macros (no magic numbers).
- The internal simple encoder uses a shared symbol context, which is suitable for
  the common single-strip usage pattern in this project.

## Usage

```c
#include "ws2812.h"

ws2812_strip_t strip = {0};

if (ws2812_init(&strip, 48, 8, false, 10 * 1000 * 1000) == ESP_OK) {
    ws2812_set_brightness(&strip, 32);
    ws2812_fill(&strip, 0, 0, 255, 0);
    (void)ws2812_show(&strip);
}

/* ... */

ws2812_deinit(&strip);
```
