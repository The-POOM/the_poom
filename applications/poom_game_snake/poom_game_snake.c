// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_game_snake.h"
#include "poom_sbus.h"
#include "poom_oled_screen.h"
#include "poom_buz_theme.h"
#include "buzzer.h"
#include "bsp_pong.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

/**
 * @file poom_game_snake.c
 * @brief Snake game implementation with SBUS input and OLED rendering.
 */

#if GAME_SNAKE_LOG_ENABLED
#define GAME_SNAKE_TAG "poom_game_snake"

#define GAME_SNAKE_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", GAME_SNAKE_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define GAME_SNAKE_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", GAME_SNAKE_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define GAME_SNAKE_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", GAME_SNAKE_TAG, __func__, __LINE__, ##__VA_ARGS__)

#if GAME_SNAKE_DEBUG_LOG_ENABLED
#define GAME_SNAKE_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", GAME_SNAKE_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define GAME_SNAKE_PRINTF_D(...) do { } while (0)
#endif

#else
#define GAME_SNAKE_PRINTF_E(...) do { } while (0)
#define GAME_SNAKE_PRINTF_W(...) do { } while (0)
#define GAME_SNAKE_PRINTF_I(...) do { } while (0)
#define GAME_SNAKE_PRINTF_D(...) do { } while (0)
#endif

enum {
    kScreenW=128,
    kScreenH=64,
    kHudHeightPx=10,
    kGameW=64,
    kGameH=27,
    kMaxLen=464,
    kStartLen=6
};

#ifndef BTN_LEFT
#define BTN_LEFT  2
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT 3
#endif
#ifndef BTN_UP
#define BTN_UP    4
#endif
#ifndef BTN_DOWN
#define BTN_DOWN  5
#endif
#ifndef BTN_A
#define BTN_A     0
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif
#ifndef BUTTON_DOUBLE_CLICK
#define BUTTON_DOUBLE_CLICK 5
#endif
typedef struct {
    uint8_t  button;
    uint8_t  event;
    uint32_t ts_ms;
} button_event_msg_t;

static int32_t s_hiscore_ram = 0;

typedef struct { int8_t x,y; } Position;

typedef struct {
    Position pos;
    uint8_t tail[kMaxLen];
    uint8_t direction;
    int size;
    int moved;
} Player;

typedef struct { Position pos; } Item;

typedef enum {
    POOM_GAME_SNAKE_STATE_INTRO = 0,
    POOM_GAME_SNAKE_STATE_PLAY,
    POOM_GAME_SNAKE_STATE_PAUSED,
    POOM_GAME_SNAKE_STATE_GAMEOVER
} poom_game_snake_state_t;

/**
 * @brief Compare two positions for exact match.
 *
 * @param a First position.
 * @param b Second position.
 *
 * @return true when both coordinates are equal.
 */
static inline bool snake_pos_eq_(Position a, Position b){ return a.x==b.x && a.y==b.y; }
static const Position s_dir_pos[4] = { {0,-1},{1,0},{0,1},{-1,0} };

static uint8_t s_board_cells[kGameW*kGameH];
static Player s_player;
static Item s_item;
static poom_game_snake_state_t s_state = POOM_GAME_SNAKE_STATE_INTRO;
static int32_t s_hiscore = 0;
static bool s_running = false;
static uint32_t s_period_ms = 90;
static TimerHandle_t s_timer = NULL;

/**
 * @brief Validate whether an input event is a supported click.
 *
 * @param ev Raw event value from button message.
 *
 * @return true for single or double click.
 */
static inline bool snake_is_click_event_(uint8_t ev){
    return (ev == BUTTON_SINGLE_CLICK) || (ev == BUTTON_DOUBLE_CLICK);
}
/**
 * @brief Check if a position belongs to the logical game grid.
 *
 * @param p Grid position.
 *
 * @return true when the position is inside grid limits.
 */
static inline bool snake_in_bounds_(Position p){ return p.x>=0 && p.x<kGameW && p.y>=0 && p.y<kGameH; }
/**
 * @brief Convert a 2D position to linear array index.
 *
 * @param p Grid position.
 *
 * @return Linear index for s_board_cells.
 */
static inline int snake_idx_(Position p){ return (int)p.y*kGameW + (int)p.x; }
/**
 * @brief Read occupancy value from board, treating out-of-bounds as occupied.
 *
 * @param p Grid position.
 *
 * @return Occupancy value at position.
 */
static inline uint8_t snake_board_cell_get_(Position p){ if(!snake_in_bounds_(p)) return 1; return s_board_cells[snake_idx_(p)]; }
/**
 * @brief Write occupancy value on board when position is valid.
 *
 * @param p Grid position.
 * @param v Value to store.
 */
static inline void snake_board_cell_set_(Position p, uint8_t v){ if(snake_in_bounds_(p)) s_board_cells[snake_idx_(p)] = v; }
/**
 * @brief Clear the full occupancy board.
 */
static void snake_board_clear_(void){ memset(s_board_cells,0,sizeof(s_board_cells)); }

/**
 * @brief Load persisted hi-score from RAM stub backend.
 *
 * @param out Destination pointer for loaded score.
 *
 * @return true when load succeeds.
 */
static bool snake_record_load_(int32_t* out){ *out = s_hiscore_ram; return true; }
/**
 * @brief Save hi-score to RAM stub backend.
 *
 * @param hs Score value to persist.
 *
 * @return true when save succeeds.
 */
static bool snake_record_save_(int32_t hs){ s_hiscore_ram = hs; return true; }

/**
 * @brief Generate pseudo-random 32-bit value.
 *
 * @return Next random value.
 */
static uint32_t snake_rng_u32_(void){
    static uint32_t s_rng_state = 0xA5A5A5A5u;
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return s_rng_state;
}
/**
 * @brief Generate pseudo-random integer in half-open range.
 *
 * @param lo Inclusive lower bound.
 * @param hi_excl Exclusive upper bound.
 *
 * @return Random integer in [lo, hi_excl).
 */
static int snake_rng_range_(int lo, int hi_excl){
    int span = hi_excl - lo;
    if (span <= 0) return lo;
    return lo + (int)(snake_rng_u32_() % (uint32_t)span);
}

/**
 * @brief Compute current score from snake length.
 *
 * @return Non-negative score value.
 */
static int snake_score_now_(void){
    int s = s_player.size - kStartLen;
    return (s<0)?0:s;
}

/**
 * @brief Draw one snake/item cell on OLED.
 *
 * @param p Grid position to render.
 */
static inline void snake_draw_square_(Position p){
    poom_oled_screen_draw_box((int)p.x*2, ((int)p.y*2) + kHudHeightPx, 2, 2, OLED_DISPLAY_NORMAL);
}
/**
 * @brief Render HUD area (score and separator line).
 */
static void snake_render_hud_(void){
    char score_text[20];
    snprintf(score_text, sizeof(score_text), "Score %d", snake_score_now_());
    poom_oled_screen_display_text_center(score_text, 0, OLED_DISPLAY_NORMAL);
    poom_oled_screen_draw_hline(0, kHudHeightPx - 1, kScreenW, OLED_DISPLAY_NORMAL);
}
/**
 * @brief Render complete gameplay frame (HUD + board + item).
 *
 * @param item_pos Current item position.
 */
static void snake_render_full_(Position item_pos){
    poom_oled_screen_clear_buffer();
    snake_render_hud_();
    for(int y=0;y<kGameH;y++){
        for(int x=0;x<kGameW;x++){
            if(s_board_cells[y*kGameW + x]){
                snake_draw_square_((Position){(int8_t)x,(int8_t)y});
            }
        }
    }
    snake_draw_square_(item_pos);
    poom_oled_screen_display_show();
}
/**
 * @brief Mark collision borders as occupied on the board.
 */
static void snake_board_draw_borders_(void){
    for(int x=0;x<kGameW;x++){
        snake_board_cell_set_((Position){(int8_t)x,0},1);
        snake_board_cell_set_((Position){(int8_t)x,(int8_t)(kGameH-1)},1);
    }
    for(int y=0;y<kGameH;y++){
        snake_board_cell_set_((Position){0,(int8_t)y},1);
        snake_board_cell_set_((Position){(int8_t)(kGameW-1),(int8_t)y},1);
    }
}

/**
 * @brief Compute current tail-end position from packed tail directions.
 *
 * @param head Current head position.
 * @param tail Packed tail direction history.
 * @param size Current snake size.
 *
 * @return Grid position corresponding to tail end.
 */
static Position snake_compute_tail_end_(Position head, const uint8_t* tail, int size){
    Position tp = head;
    for(int i=0;i<size;i++){
        uint8_t packed = tail[(i>>2)];
        uint8_t dir2b  = (packed >> ((i&3)*2)) & 3u;
        tp.x += s_dir_pos[dir2b].x;
        tp.y += s_dir_pos[dir2b].y;
    }
    return tp;
}
/**
 * @brief Reset player state to initial values.
 */
static void snake_player_reset_(void){
    s_player.pos = (Position){(int8_t)(kGameW / 2), (int8_t)(kGameH / 2)};
    s_player.direction = 1;
    s_player.size = kStartLen;
    s_player.moved = 0;
    memset(s_player.tail,0,sizeof(s_player.tail));
}
/**
 * @brief Spawn item at a random free-range board position.
 */
static void snake_item_spawn_(void){
    s_item.pos.x = (int8_t)snake_rng_range_(1, kGameW - 1);
    s_item.pos.y = (int8_t)snake_rng_range_(1, kGameH - 1);
}
/**
 * @brief Determine whether next direction is opposite of current.
 *
 * @param current_direction Current movement direction.
 * @param next_direction Candidate movement direction.
 *
 * @return true when next direction is a 180-degree turn.
 */
static inline bool snake_is_reverse_direction_(uint8_t current_direction, uint8_t next_direction){
    return (uint8_t)((current_direction + 2u) % 4u) == next_direction;
}
/**
 * @brief Map button id to snake direction code.
 *
 * @param button Raw button id.
 * @param out_direction Output pointer for mapped direction.
 *
 * @return true when mapping is valid.
 */
static bool snake_map_button_to_direction_(uint8_t button, uint8_t* out_direction){
    if(!out_direction) return false;

    switch(button){
        case BTN_UP:    *out_direction = 0; return true;
        case BTN_RIGHT: *out_direction = 1; return true;
        case BTN_DOWN:  *out_direction = 2; return true;
        case BTN_LEFT:  *out_direction = 3; return true;
        default:        return false;
    }
}
/**
 * @brief Apply new player direction if it is valid.
 *
 * @param next_direction Candidate direction code.
 *
 * @return true when direction change is accepted.
 */
static bool snake_player_set_direction_(uint8_t next_direction){
    if(next_direction > 3u) return false;
    if(snake_is_reverse_direction_(s_player.direction, next_direction)) return false;

    s_player.direction = next_direction;
    return true;
}
/**
 * @brief Advance player position and packed tail state by one tick.
 */
static void snake_player_update_(void){
    for(int i=kMaxLen-1;i>0;--i){
        s_player.tail[i] = (uint8_t)((s_player.tail[i]<<2) | ((s_player.tail[i-1]>>6)&3u));
    }
    s_player.tail[0] = (uint8_t)((s_player.tail[0]<<2) | ((s_player.direction + 2) % 4));
    s_player.pos.x += s_dir_pos[s_player.direction].x;
    s_player.pos.y += s_dir_pos[s_player.direction].y;
    if(s_player.moved < s_player.size) s_player.moved++;
}
static void snake_step_(uint32_t now_ms);

/**
 * @brief FreeRTOS timer callback for game loop ticks.
 *
 * @param xTimer Timer handle.
 */
static void snake_timer_cb_(TimerHandle_t xTimer){
    (void)xTimer;
    if(!s_running) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    snake_step_(now_ms);
}

/**
 * @brief Create/start timer or update period when already created.
 */
static void snake_timer_start_or_update_(void){
    TickType_t period_ticks = pdMS_TO_TICKS(s_period_ms);
    if (period_ticks == 0) period_ticks = 1;

    if (s_timer == NULL){
        s_timer = xTimerCreate(
            "snake_tmr",
            period_ticks,
            pdTRUE,
            NULL,
            snake_timer_cb_
        );
    } else {
        xTimerChangePeriod(s_timer, period_ticks, 0);
    }

    if (s_timer){
        xTimerStart(s_timer, 0);
    }
}

/**
 * @brief Stop game timer when active.
 */
static void snake_timer_stop_(void){
    if (s_timer){
        xTimerStop(s_timer, 0);
    }
}

static const uint8_t s_snake_72x64[]= {
	0xff, 0xff, 0xff, 0xfe, 0x00, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe0, 0x00, 0x00, 0x7f, 
	0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xfc, 0x00, 0xff, 
	0xf8, 0x03, 0xff, 0xff, 0xff, 0xff, 0xf0, 0x1f, 0xff, 0xff, 0x80, 0xff, 0xff, 0xff, 0xff, 0xe0, 
	0x7f, 0xff, 0xff, 0xe0, 0x3f, 0xff, 0xff, 0xff, 0x81, 0xff, 0xff, 0xff, 0xfc, 0x0f, 0xff, 0xff, 
	0xff, 0x07, 0xff, 0xff, 0xff, 0xfe, 0x07, 0xff, 0xff, 0xfe, 0x0f, 0xff, 0xff, 0xff, 0xff, 0x83, 
	0xff, 0xff, 0xfc, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xc1, 0xff, 0xff, 0xf8, 0x3f, 0xff, 0xff, 0xff, 
	0xff, 0xf0, 0xff, 0xff, 0xe0, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x7f, 0xff, 0x00, 0xff, 0xff, 
	0xff, 0xff, 0xff, 0xfc, 0x3f, 0xfe, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x1f, 0xfc, 0x1f, 
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0xf8, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f, 
	0xf0, 0xff, 0xff, 0xff, 0xfc, 0x0f, 0xff, 0xff, 0x87, 0xe1, 0xff, 0xff, 0xff, 0xe0, 0x01, 0xff, 
	0xff, 0xc7, 0xe3, 0xff, 0xff, 0xff, 0x80, 0x00, 0x7f, 0xff, 0xc3, 0xc3, 0xff, 0xff, 0xff, 0x03, 
	0xe0, 0x1f, 0xff, 0xe3, 0xc7, 0xff, 0xff, 0xfe, 0x0f, 0xfc, 0x0f, 0xff, 0xe3, 0x87, 0xff, 0xff, 
	0xfe, 0x3f, 0xff, 0x07, 0xff, 0xf1, 0x8f, 0xff, 0xff, 0xfc, 0x3f, 0xff, 0xc3, 0xff, 0xf1, 0x89, 
	0xff, 0xff, 0xfc, 0x7f, 0xff, 0xe3, 0xff, 0xf1, 0x80, 0xff, 0xff, 0xfe, 0x3f, 0xff, 0xe1, 0xff, 
	0xf0, 0x00, 0xff, 0xff, 0xfe, 0x3f, 0xff, 0xf1, 0xff, 0xf8, 0x00, 0xff, 0xff, 0xfe, 0x1f, 0xff, 
	0xf0, 0xff, 0xf8, 0x07, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xf8, 0xff, 0xf8, 0x0f, 0xff, 0xff, 0xff, 
	0x1f, 0xff, 0xf8, 0xff, 0xf8, 0x05, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xf8, 0xff, 0xf8, 0x00, 0xff, 
	0xff, 0xff, 0x1f, 0xff, 0xf8, 0xff, 0xf8, 0x01, 0xff, 0xf8, 0x3f, 0x1f, 0xff, 0xf8, 0xff, 0xf8, 
	0x11, 0xff, 0xf8, 0x3f, 0x1f, 0xff, 0xf8, 0xff, 0xf8, 0x1f, 0xff, 0xf8, 0x3e, 0x3f, 0xff, 0xf8, 
	0xff, 0xf8, 0x1f, 0xff, 0xdc, 0x3c, 0x3f, 0xff, 0xf1, 0xff, 0xf8, 0x1f, 0xff, 0xdc, 0x78, 0x7f, 
	0xff, 0xf1, 0xff, 0xf0, 0x1f, 0xff, 0x8c, 0x78, 0x7f, 0xff, 0xe1, 0xff, 0xf1, 0x8f, 0xff, 0x84, 
	0xf0, 0xff, 0xff, 0xe3, 0xff, 0xf1, 0x8f, 0xff, 0xc1, 0xc1, 0xff, 0xff, 0xc3, 0xff, 0xf1, 0x8f, 
	0xff, 0xc7, 0x83, 0xff, 0xff, 0x87, 0xff, 0xe1, 0x87, 0xff, 0xff, 0x07, 0xff, 0xff, 0x0f, 0xff, 
	0xe3, 0xc7, 0xff, 0xfe, 0x0f, 0xff, 0xff, 0x0f, 0xff, 0xe3, 0xc3, 0xff, 0xf8, 0x3f, 0xff, 0xfe, 
	0x1f, 0xff, 0xc7, 0xe1, 0xff, 0xe0, 0x7f, 0xff, 0xfc, 0x3f, 0xff, 0xc7, 0xf0, 0x7f, 0x80, 0xff, 
	0xff, 0xf8, 0x7f, 0xff, 0x87, 0xf8, 0x00, 0x03, 0xff, 0xff, 0xf0, 0xff, 0xff, 0x0f, 0xfc, 0x00, 
	0x0f, 0xff, 0xff, 0xf1, 0xff, 0xff, 0x1f, 0xf8, 0x00, 0x7f, 0xff, 0xff, 0xe1, 0xff, 0xfe, 0x1f, 
	0xf8, 0x3f, 0xff, 0xff, 0xff, 0xc3, 0xff, 0xfc, 0x3f, 0xf0, 0x7f, 0xff, 0xff, 0xff, 0x87, 0xff, 
	0xfc, 0x7f, 0xf0, 0x7f, 0xff, 0xff, 0xff, 0x8f, 0xff, 0xf8, 0x7f, 0xf0, 0xff, 0xff, 0xff, 0xff, 
	0x0f, 0xff, 0xf0, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xe1, 0xff, 0xf0, 0xff, 0xff, 
	0xff, 0xfe, 0x3f, 0xff, 0xe3, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xfc, 0x3f, 0xff, 0xc3, 0xff, 0xf0, 
	0xff, 0xff, 0xff, 0xfc, 0x7f, 0xff, 0x87, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xf8, 0x7f, 0xff, 0x0f, 
	0xff, 0xf0, 0xff, 0xff, 0xff, 0xf8, 0xff, 0xff, 0x1f, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xf8, 0xff, 
	0xfe, 0x1f, 0xff, 0xf0, 0x7f, 0xff, 0xff, 0xf1, 0xff, 0xfc, 0x3f, 0xff, 0xf0, 0x7f, 0xff, 0xff, 
	0xf1, 0xff, 0xfc, 0x7f, 0xff, 0xf0, 0x3f, 0xff, 0xff, 0xf1, 0xff, 0xf8, 0x7f, 0xff, 0xf0, 0x3f, 
	0xff, 0xff, 0xe3, 0xff, 0xf8, 0xff, 0xff, 0xf2, 0x1f, 0xff, 0xff, 0xe3, 0xff, 0xf0, 0xff, 0xff, 
	0xf3, 0x0f, 0xff, 0xff, 0xe3, 0xff, 0xf1, 0xff, 0xff, 0xe3, 0x87, 0xff, 0xff, 0xe3, 0xff, 0xe1, 
	0xff, 0xff, 0xe7, 0xc3, 0xff, 0xff, 0xe3, 0xff, 0xe3, 0xff, 0xff, 0xc7, 0xe1, 0xff, 0xff, 0xc7, 
	0xff, 0xe3, 0xff, 0xff, 0xcf, 0xf8, 0xff, 0xff, 0xc7, 0xff, 0xe3, 0xff, 0xff, 0xcf, 0xfe, 0x7f, 
	0xff, 0xc7, 0xff, 0xc3, 0xff, 0xff, 0x9f, 0xff, 0xff, 0xff, 0xc7, 0xff, 0xc7, 0xff, 0xff, 0x7f
};

/**
 * @brief Render intro screen.
 */
static void snake_show_intro_(void){
    poom_oled_screen_clear_buffer();

    const int w = 72;
    const int h = 64;
    int x = (128 - w) / 2;
    int y = 0;

    poom_oled_screen_display_bitmap(s_snake_72x64, x, y, w, h, OLED_DISPLAY_INVERT);
    poom_oled_screen_display_text("SNAKE", 48, 0, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_text("PUSH TO START", 12, 7, OLED_DISPLAY_NORMAL);

    poom_oled_screen_display_show();
}

/**
 * @brief Render game-over screen and update hi-score if needed.
 */
static void snake_show_gameover_(void){
    int sc = snake_score_now_();
    if(sc > s_hiscore){
        s_hiscore = sc;
        snake_record_save_(s_hiscore);
    }

    poom_oled_screen_clear_buffer();

    char buf[24];
    char hi_buf[24];

    poom_oled_screen_draw_rect_round(8, 8, 112, 40, 4, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_text_center("GAME OVER", 1, OLED_DISPLAY_NORMAL);
    poom_oled_screen_draw_hline(16, 23, 96, OLED_DISPLAY_NORMAL);

    snprintf(buf, sizeof(buf), "Score %d", sc);
    poom_oled_screen_display_text_center(buf, 4, OLED_DISPLAY_NORMAL);

    snprintf(hi_buf, sizeof(hi_buf), "Hi-Score %ld", (long)s_hiscore);
    poom_oled_screen_display_text_center(hi_buf, 5, OLED_DISPLAY_NORMAL);

    poom_oled_screen_display_text_center("PUSH TO START", 7, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_show();
}

/**
 * @brief Render pause screen with current score.
 */
static void snake_show_paused_(void){
    char buf[24];

    poom_oled_screen_clear_buffer();
    poom_oled_screen_draw_rect_round(12, 12, 104, 32, 4, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_text_center("PAUSED", 2, OLED_DISPLAY_NORMAL);

    snprintf(buf, sizeof(buf), "Score %d", snake_score_now_());
    poom_oled_screen_display_text_center(buf, 4, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_text_center("A TO RESUME", 6, OLED_DISPLAY_NORMAL);
    poom_oled_screen_display_show();
}

/**
 * @brief Reset board, player, item and render first gameplay frame.
 */
static void snake_reset_game_(void){
    snake_board_clear_();
    snake_board_draw_borders_();
    snake_player_reset_();
    snake_item_spawn_();
    snake_board_cell_set_(s_player.pos, 1);
    snake_render_full_(s_item.pos);
}

/**
 * @brief Handle button events from SBUS subscription.
 *
 * @param msg Incoming SBUS message.
 * @param user User context (unused).
 */
static void snake_on_button_any_(const poom_sbus_msg_t* msg, void* user){
    (void)user;
    if(!s_running) return;
    if(!msg || msg->len < sizeof(button_event_msg_t)) return;

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if(!snake_is_click_event_(ev.event)) return;

    if((s_state == POOM_GAME_SNAKE_STATE_INTRO) ||
       (s_state == POOM_GAME_SNAKE_STATE_GAMEOVER)) {
        if(ev.button == BTN_A){
            s_state = POOM_GAME_SNAKE_STATE_PLAY;
            snake_reset_game_();
        }
        return;
    }

    if((s_state == POOM_GAME_SNAKE_STATE_PLAY) && (ev.button == BTN_A)){
        s_state = POOM_GAME_SNAKE_STATE_PAUSED;
        snake_show_paused_();
        return;
    }

    if((s_state == POOM_GAME_SNAKE_STATE_PAUSED) && (ev.button == BTN_A)){
        s_state = POOM_GAME_SNAKE_STATE_PLAY;
        snake_render_full_(s_item.pos);
        return;
    }

    if(s_state != POOM_GAME_SNAKE_STATE_PLAY) return;

    uint8_t next_direction = 0;
    if(!snake_map_button_to_direction_(ev.button, &next_direction)) return;

    if(!snake_player_set_direction_(next_direction)){
        GAME_SNAKE_PRINTF_D("Ignored reverse direction: %u", (unsigned)next_direction);
        return;
    }

    GAME_SNAKE_PRINTF_D("Direction changed to: %u", (unsigned)next_direction);
}

/**
 * @brief Subscribe button callback to SBUS input topic.
 */
static void snake_buttons_subscribe_(void){
    poom_sbus_subscribe_cb("input/button", snake_on_button_any_, NULL);
}

/**
 * @brief Execute one gameplay simulation step.
 *
 * @param now_ms Current tick time in milliseconds.
 */
static void snake_step_(uint32_t now_ms){
    (void)now_ms;
    if(!s_running) return;
    if(s_state != POOM_GAME_SNAKE_STATE_PLAY) return;

    snake_player_update_();

    if(snake_pos_eq_(s_player.pos, s_item.pos)){
        s_player.size++;
        poom_buz_theme_snake_eat_fx();
        snake_item_spawn_();
    } else if(snake_board_cell_get_(s_player.pos)){
        s_state = POOM_GAME_SNAKE_STATE_GAMEOVER;
        snake_show_gameover_();
        poom_buz_theme_snake_gameover_fx();
        return;
    }

    snake_board_cell_set_(s_player.pos, 1);

    if(s_player.moved >= s_player.size){
        Position tail_end = snake_compute_tail_end_(s_player.pos, s_player.tail, s_player.size);
        snake_board_cell_set_(tail_end, 0);
    }

    snake_render_full_(s_item.pos);
}

/**
 * @brief Set game loop period in milliseconds.
 *
 * @param period_ms Desired period. Values below 20 ms are clamped.
 */
void poom_game_snake_set_period_ms(uint32_t period_ms){
    if (period_ms < 20) period_ms = 20;
    s_period_ms = period_ms;
    if (s_running) snake_timer_start_or_update_();
}

/**
 * @brief Start Snake game runtime.
 */
void poom_game_snake_start(void){
    buzzer_init(PIN_NUM_BUZZER);
    
    snake_record_load_(&s_hiscore);

    s_state = POOM_GAME_SNAKE_STATE_INTRO;
    s_running = true;

    snake_show_intro_();

    snake_buttons_subscribe_();

    snake_timer_start_or_update_();
    poom_buz_theme_snake();
}

/**
 * @brief Stop Snake game runtime.
 */
void poom_game_snake_stop(void){
    s_running = false;
    snake_timer_stop_();
}
