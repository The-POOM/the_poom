// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_led_rainbow.h
 * @brief Public API for WS2812 rainbow animation.
 */

/**
 * @brief Initializes WS2812 strip and leaves LEDs off.
 */
void poom_led_rainbow_init(void);

/**
 * @brief Starts rainbow animation task.
 *
 * @return true if task is running or created successfully.
 */
bool poom_led_rainbow_start(void);

/**
 * @brief Stops rainbow animation and turns all LEDs off.
 */
void poom_led_rainbow_stop(void);

#ifdef __cplusplus
}
#endif
