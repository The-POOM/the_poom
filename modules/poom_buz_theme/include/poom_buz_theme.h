// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POOM_BUZ_THEME_MELODY_MARIO = 0,
    POOM_BUZ_THEME_MELODY_ZELDA,
    POOM_BUZ_THEME_MELODY_TETRIS,
    POOM_BUZ_THEME_MELODY_PACMAN,
    POOM_BUZ_THEME_MELODY_GAMEBOY,
    POOM_BUZ_THEME_MELODY_SONIC,
    POOM_BUZ_THEME_MELODY_MEGAMAN
} poom_buz_theme_melody_id_t;

/**
 * @brief Play Mario melody.
 *
 * @return void
 */
void poom_buz_theme_mario(void);

/**
 * @brief Play Zelda treasure melody.
 *
 * @return void
 */
void poom_buz_theme_zelda_treasure(void);

/**
 * @brief Play Tetris melody.
 *
 * @return void
 */
void poom_buz_theme_tetris(void);

/**
 * @brief Play Pacman intro melody.
 *
 * @return void
 */
void poom_buz_theme_pacman_intro(void);

/**
 * @brief Play Gameboy startup melody.
 *
 * @return void
 */
void poom_buz_theme_gameboy_startup(void);

/**
 * @brief Play Sonic ring melody.
 *
 * @return void
 */
void poom_buz_theme_sonic_ring(void);

/**
 * @brief Play Megaman jump melody.
 *
 * @return void
 */
void poom_buz_theme_megaman_jump(void);

/**
 * @brief Play Snake background melody.
 *
 * @return void
 */
void poom_buz_theme_snake(void);

/**
 * @brief Play Snake eat effect.
 *
 * @return void
 */
void poom_buz_theme_snake_eat_fx(void);

/**
 * @brief Play Snake game over effect.
 *
 * @return void
 */
void poom_buz_theme_snake_gameover_fx(void);

/**
 * @brief Start a melody task for a selected melody.
 *
 * @param[in] id Melody identifier.
 * @return void
 */
void poom_buz_theme_init_melody(poom_buz_theme_melody_id_t id);

/**
 * @brief Stop currently running melody task.
 *
 * @return void
 */
void poom_buz_theme_stop(void);

#ifdef __cplusplus
}
#endif
