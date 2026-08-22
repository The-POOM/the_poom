// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_WS2812_COLOR_H
#define MENU_WS2812_COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows WS2812 color control screen.
 *
 * Controls all LEDs with a single RGB color. Exits back to menu on B.
 */
void menu_ws2812_color_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_WS2812_COLOR_H */
