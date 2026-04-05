/**
 * @file oled_driver.h
 * @brief Minimal OLED init API (SSD1306).
 *
 * This project now renders through `poom_arduboy_display`, which uses the
 * low-level transport (`oled_transport_*`) and only needs the init helper.
 */

#pragma once

#include "oled_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize OLED transport and clear internal page buffers. */
void oled_driver_init(oled_driver_t *dev, int width, int height);

#ifdef __cplusplus
}
#endif
