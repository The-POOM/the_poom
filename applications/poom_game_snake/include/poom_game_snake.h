// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_GAME_SNAKE_H
#define POOM_GAME_SNAKE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_game_snake.h
 * @brief Public API for the POOM Snake mini-game.
 */

/**
 * @brief Start Snake game runtime.
 *
 * Initializes game state, subscribes button events and starts internal timer.
 */
void poom_game_snake_start(void);

/**
 * @brief Stop Snake game runtime.
 *
 * Stops internal timer and input processing.
 */
void poom_game_snake_stop(void);

/**
 * @brief Configure game tick period.
 *
 * @param period_ms Tick period in milliseconds. Values below 20 are clamped.
 */
void poom_game_snake_set_period_ms(uint32_t period_ms);

#ifdef __cplusplus
}
#endif

#endif /* POOM_GAME_SNAKE_H */
