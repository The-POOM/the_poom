// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_breakout.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "Sprites.h"
#include "button_driver.h"
#include "buzzer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BTN_A
#define BTN_A (0U)
#endif

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN (0U)
#endif

#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define BREAKOUT_FRAME_MS (33U)
#define BREAKOUT_TASK_STACK (3584U)
#define BREAKOUT_TASK_PRIO (4U)
#define BREAKOUT_PADDLE_W (12)
#define BREAKOUT_PADDLE_H (2)
#define BREAKOUT_PADDLE_Y (60)
#define BREAKOUT_PADDLE_SPEED (3)
#define BREAKOUT_BALL_SIZE (2)
#define BREAKOUT_BRICK_W (8)
#define BREAKOUT_BRICK_H (4)
#define BREAKOUT_BRICK_STEP_X (10)
#define BREAKOUT_BRICK_STEP_Y (6)
#define BREAKOUT_BRICK_TOP (12)
#define BREAKOUT_PLAYFIELD_TOP (10)
#define BREAKOUT_ROWS (4)
#define BREAKOUT_COLS (13)
#define BREAKOUT_TITLE_FLASH_PERIOD (8U)

typedef enum
{
    BREAKOUT_SCREEN_TITLE = 0,
    BREAKOUT_SCREEN_PLAY,
    BREAKOUT_SCREEN_GAME_OVER,
} breakout_screen_t;

typedef struct
{
    breakout_screen_t screen;
    bool paused;
    bool released;
    uint8_t lives;
    uint8_t level;
    uint16_t score;
    uint16_t brick_count;
    int16_t paddle_x;
    int16_t ball_x;
    int16_t ball_y;
    int8_t dx;
    int8_t dy;
    bool bricks[BREAKOUT_ROWS][BREAKOUT_COLS];
} breakout_state_t;

static TaskHandle_t s_breakout_task = NULL;
static bool s_breakout_running = false;
static bool s_breakout_buttons_subscribed = false;
static volatile bool s_breakout_exit_requested = false;
static volatile bool s_breakout_left_down = false;
static volatile bool s_breakout_right_down = false;
static volatile bool s_breakout_a_clicked = false;
static volatile bool s_breakout_b_clicked = false;
static uint16_t s_breakout_title_tick = 0U;
static char s_breakout_sbus_user[] = "poom_breakout";
static poom_breakout_exit_cb_t s_breakout_exit_cb = NULL;
static void *s_breakout_exit_cb_ctx = NULL;
static uint16_t s_breakout_high_score = 0U;
static breakout_state_t s_breakout_state;

static void breakout_task_(void *arg);
static void breakout_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void breakout_menu_exit_cb_(void *user_ctx);

/**
 * @brief Internal helper for `breakout_play_tone`.
 *
 * @param[in] freq_hz Parameter passed to the function.
 * @param[in] duration_ms Parameter passed to the function.
 * @return void
 */
static void breakout_play_tone_(uint32_t freq_hz, uint32_t duration_ms)
{
    if ((freq_hz == 0U) || (duration_ms == 0U))
    {
        return;
    }

    buzzer_tone(freq_hz, duration_ms);
}

/**
 * @brief Internal helper for `breakout_random_dx`.
 *
 * @return int8_t
 */
static int8_t breakout_random_dx_(void)
{
    return ((esp_random() & 1U) != 0U) ? 1 : -1;
}

/**
 * @brief Internal helper for `breakout_reset_bricks`.
 *
 * @return void
 */
static void breakout_reset_bricks_(void)
{
    memset(s_breakout_state.bricks, 0, sizeof(s_breakout_state.bricks));
    s_breakout_state.brick_count = 0U;
    s_breakout_state.released = false;
    s_breakout_state.paused = false;
    s_breakout_state.paddle_x = (ARDUBOY_WIDTH - BREAKOUT_PADDLE_W) / 2;
    s_breakout_state.ball_x = s_breakout_state.paddle_x + (BREAKOUT_PADDLE_W / 2) - 1;
    s_breakout_state.ball_y = BREAKOUT_PADDLE_Y - 3;
    s_breakout_state.dx = breakout_random_dx_();
    s_breakout_state.dy = -1;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return void
 */
static void breakout_start_run_(void)
{
    s_breakout_state.screen = BREAKOUT_SCREEN_PLAY;
    s_breakout_state.lives = 3U;
    s_breakout_state.level = 1U;
    s_breakout_state.score = 0U;
    breakout_reset_bricks_();
}

/**
 * @brief Releases internal state before leaving this module.
 *
 * @return void
 */
static void breakout_request_exit_(void)
{
    s_breakout_exit_requested = true;
}

/**
 * @brief Internal helper for `breakout_create_title_logo`.
 *
 * @param[in] out_size Parameter passed to the function.
 * @return uint8_t *
 */
static uint8_t *breakout_create_title_logo_(size_t *out_size)
{
    /** @brief Monochrome POOM splash logo bitmap. */
    const uint8_t logo_template[] = {
        80, 40,
        0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
        0x80,0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x8f,0x91,0xb1,0xa1,
        0xa1,0xa1,0xa1,0xa1,0xa1,0x21,0x21,0x1e,0x00,0x0e,0x21,0xa1,0xa1,0xa1,0xa1,0xa1,
        0xa1,0xa1,0xa1,0xa1,0x9f,0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
        0x80,0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x00,0x00,
        0x00,0x00,0x00,0xfc,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xe0,0xe0,0x00,0x00,
        0x00,0x00,0x07,0x04,0x04,0xf0,0x04,0x04,0x04,0x07,0x00,0x00,0x00,0x00,0xe0,0x00,
        0x00,0x00,0x00,0x00,0x07,0x04,0x04,0xf8,0x04,0x04,0x07,0x00,0x00,0x00,0x00,0x00,
        0xe0,0x00,0x00,0x00,0x00,0x07,0x04,0x04,0xfc,0xff,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x3f,0x3c,0x04,0x04,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,
        0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc1,0xc1,0xc1,0xc1,
        0xc1,0xc0,0xf8,0x18,0xf8,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x43,0x43,
        0x7f,0x7f,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x43,0x43,0x7f,0x7f,0x00,0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,0x00,0xf8,0xf8,
        0x00,0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,
        0x00,0x00,0x00,0x3f,0x7f,0x70,0x70,0x70,0x70,0x70,0x70,0x30,0x0f,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x03,0x07,0x06,0x1e,0x3e,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x3e,0x06,0x06,0x07,0x06,0x3e,0x3e,0x30,0x70,0x70,0x70,0x70,
        0x70,0x70,0x70,0x30,0x30,0x0e,0x06,0x00,0x3f,0x7f,0x70,0x70,0x70,0x30,0x0f,0x07,
        0x06,0x06,0x06,0x00,0x3f,0x3f,0x70,0x70,0x70,0x70,0x70,0x70,0x78,0x7f,0x00,0x00,
        0xf8,0x28,0x10,0x00,0xf8,0xa8,0xa8,0x00,0xf8,0x10,0x20,0xf8,0x00,0x08,0xf8,0x08,
        0x00,0xf8,0xa8,0xa8,0x00,0x90,0xa8,0x48,0x00,0x08,0xf8,0x08,0x00,0x80,0x00,0x00,
        0x00,0x00,0xf8,0x28,0x10,0x00,0xf8,0x80,0x80,0x00,0xf0,0x28,0xf0,0x00,0x18,0xe0,
        0x18,0x00,0x80,0x00,0x00,0x00,0x00,0x70,0x88,0x88,0x00,0xf8,0x28,0xd0,0x00,0xf8,
        0xa8,0xa8,0x00,0xf0,0x28,0xf0,0x00,0x08,0xf8,0x08,0x00,0xf8,0xa8,0xa8,0x00,0x80
    };
    const size_t logo_size = sizeof(logo_template);
    uint8_t *logo = (uint8_t *)malloc(logo_size);

    if (out_size != NULL)
    {
        *out_size = 0U;
    }

    if (logo == NULL)
    {
        return NULL;
    }

    (void)memcpy(logo, logo_template, logo_size);

    if (out_size != NULL)
    {
        *out_size = logo_size;
    }

    return logo;
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] flash_on Parameter passed to the function.
 * @return void
 */
static void breakout_draw_title_footer_(bool flash_on)
{
    if (flash_on)
    {
        poom_arduboy_fill_rect(8, 55, 4, 3, WHITE);
    }
}

/**
 * @brief Renders the current module state.
 *
 * @return void
 */
static void breakout_render_title_(void)
{
    char hi_line[20];
    const bool flash_on = ((s_breakout_title_tick / BREAKOUT_TITLE_FLASH_PERIOD) & 1U) != 0U;
    size_t logo_size = 0U;
    uint8_t *logo = breakout_create_title_logo_(&logo_size);

    s_breakout_title_tick++;

    poom_arduboy_clear();
    if ((logo != NULL) && (logo_size > 2U))
    {
        Sprites_drawOverwrite(24, 4, logo, 0);
        free(logo);
    }

    breakout_draw_title_footer_(flash_on);

    poom_arduboy_set_text_size(1);
    (void)snprintf(hi_line, sizeof(hi_line), "HI:%u", s_breakout_high_score);
    poom_arduboy_set_cursor(48, 46);
    (void)poom_arduboy_print(hi_line);
    poom_arduboy_set_cursor(16, 54);
    (void)poom_arduboy_print(F("A PLAY"));
    poom_arduboy_set_cursor(88, 54);
    (void)poom_arduboy_print(F("B BACK"));
    poom_arduboy_display();
}

/**
 * @brief Renders the current module state.
 *
 * @return void
 */
static void breakout_render_game_over_(void)
{
    char score_line[20];
    char hi_line[20];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(2);
    poom_arduboy_set_cursor(18, 10);
    (void)poom_arduboy_print(F("GAME"));
    poom_arduboy_set_cursor(30, 28);
    (void)poom_arduboy_print(F("OVER"));

    poom_arduboy_set_text_size(1);
    (void)snprintf(score_line, sizeof(score_line), "SCORE:%u", s_breakout_state.score);
    (void)snprintf(hi_line, sizeof(hi_line), "HI:%u", s_breakout_high_score);
    poom_arduboy_set_cursor(18, 46);
    (void)poom_arduboy_print(score_line);
    poom_arduboy_set_cursor(84, 46);
    (void)poom_arduboy_print(hi_line);
    poom_arduboy_set_cursor(6, 56);
    (void)poom_arduboy_print(F("A:RETRY  B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Renders the current module state.
 *
 * @return void
 */
static void breakout_render_play_(void)
{
    char hud[22];

    poom_arduboy_clear();
    (void)snprintf(hud,
                   sizeof(hud),
                   "S:%u L:%u LV:%u",
                   s_breakout_state.score,
                   s_breakout_state.lives,
                   s_breakout_state.level);
    poom_arduboy_set_cursor(2, 1);
    (void)poom_arduboy_print(hud);
    poom_arduboy_draw_fast_hline(0, 9, ARDUBOY_WIDTH, WHITE);

    for (uint8_t row = 0; row < BREAKOUT_ROWS; ++row)
    {
        for (uint8_t col = 0; col < BREAKOUT_COLS; ++col)
        {
            if (!s_breakout_state.bricks[row][col])
            {
                poom_arduboy_fill_rect((int16_t)(col * BREAKOUT_BRICK_STEP_X),
                                       (int16_t)(BREAKOUT_BRICK_TOP + row * BREAKOUT_BRICK_STEP_Y),
                                       BREAKOUT_BRICK_W,
                                       BREAKOUT_BRICK_H,
                                       WHITE);
            }
        }
    }

    poom_arduboy_fill_rect(s_breakout_state.paddle_x,
                           BREAKOUT_PADDLE_Y,
                           BREAKOUT_PADDLE_W,
                           BREAKOUT_PADDLE_H,
                           WHITE);
    poom_arduboy_fill_rect(s_breakout_state.ball_x,
                           s_breakout_state.ball_y,
                           BREAKOUT_BALL_SIZE,
                           BREAKOUT_BALL_SIZE,
                           WHITE);

    if (s_breakout_state.paused)
    {
        poom_arduboy_fill_rect(35, 25, 58, 14, INVERT);
        poom_arduboy_set_cursor(48, 29);
        (void)poom_arduboy_print(F("PAUSE"));
    }
    else if (!s_breakout_state.released)
    {
        poom_arduboy_set_cursor(2, 53);
        (void)poom_arduboy_print(F("A:LAUNCH"));
        poom_arduboy_set_cursor(80, 53);
        (void)poom_arduboy_print(F("B:BACK"));
    }

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `breakout_move_paddle`.
 *
 * @return void
 */
static void breakout_move_paddle_(void)
{
    if (s_breakout_left_down && !s_breakout_right_down)
    {
        s_breakout_state.paddle_x -= BREAKOUT_PADDLE_SPEED;
    }
    else if (s_breakout_right_down && !s_breakout_left_down)
    {
        s_breakout_state.paddle_x += BREAKOUT_PADDLE_SPEED;
    }

    if (s_breakout_state.paddle_x < 0)
    {
        s_breakout_state.paddle_x = 0;
    }
    else if (s_breakout_state.paddle_x > (ARDUBOY_WIDTH - BREAKOUT_PADDLE_W))
    {
        s_breakout_state.paddle_x = (ARDUBOY_WIDTH - BREAKOUT_PADDLE_W);
    }

    if (!s_breakout_state.released)
    {
        s_breakout_state.ball_x = s_breakout_state.paddle_x + (BREAKOUT_PADDLE_W / 2) - 1;
    }
}

/**
 * @brief Internal helper for `breakout_on_life_lost`.
 *
 * @return void
 */
static void breakout_on_life_lost_(void)
{
    if (s_breakout_state.lives > 0U)
    {
        s_breakout_state.lives--;
    }

    s_breakout_state.released = false;
    s_breakout_state.paused = false;
    s_breakout_state.paddle_x = (ARDUBOY_WIDTH - BREAKOUT_PADDLE_W) / 2;
    s_breakout_state.ball_x = s_breakout_state.paddle_x + (BREAKOUT_PADDLE_W / 2) - 1;
    s_breakout_state.ball_y = BREAKOUT_PADDLE_Y - 3;
    s_breakout_state.dx = breakout_random_dx_();
    s_breakout_state.dy = -1;
    breakout_play_tone_(180U, 20U);

    if (s_breakout_state.lives == 0U)
    {
        if (s_breakout_state.score > s_breakout_high_score)
        {
            s_breakout_high_score = s_breakout_state.score;
        }
        s_breakout_state.screen = BREAKOUT_SCREEN_GAME_OVER;
        breakout_play_tone_(120U, 35U);
    }
}

/**
 * @brief Internal helper for `breakout_hit_brick`.
 *
 * @param[in] row Parameter passed to the function.
 * @param[in] col Parameter passed to the function.
 * @param[in] prev_x Parameter passed to the function.
 * @param[in] prev_y Parameter passed to the function.
 * @return void
 */
static void breakout_hit_brick_(uint8_t row, uint8_t col, int16_t prev_x, int16_t prev_y)
{
    const int16_t brick_x = (int16_t)(col * BREAKOUT_BRICK_STEP_X);
    const int16_t brick_y = (int16_t)(BREAKOUT_BRICK_TOP + row * BREAKOUT_BRICK_STEP_Y);
    const int16_t overlap_left = (s_breakout_state.ball_x + BREAKOUT_BALL_SIZE) - brick_x;
    const int16_t overlap_right = (brick_x + BREAKOUT_BRICK_W) - prev_x;
    const int16_t overlap_top = (s_breakout_state.ball_y + BREAKOUT_BALL_SIZE) - brick_y;

    s_breakout_state.bricks[row][col] = true;
    s_breakout_state.brick_count++;
    s_breakout_state.score += (uint16_t)(s_breakout_state.level * 10U);

    if ((overlap_left < overlap_top) || (overlap_right < overlap_top))
    {
        s_breakout_state.dx = (int8_t)(-s_breakout_state.dx);
        s_breakout_state.ball_x = prev_x;
    }
    else
    {
        s_breakout_state.dy = (int8_t)(-s_breakout_state.dy);
        s_breakout_state.ball_y = prev_y;
    }

    breakout_play_tone_(320U, 10U);
}

/**
 * @brief Internal helper for `breakout_move_ball`.
 *
 * @return void
 */
static void breakout_move_ball_(void)
{
    const int16_t prev_x = s_breakout_state.ball_x;
    const int16_t prev_y = s_breakout_state.ball_y;
    const int16_t paddle_center = s_breakout_state.paddle_x + (BREAKOUT_PADDLE_W / 2);

    s_breakout_state.ball_x += s_breakout_state.dx;
    s_breakout_state.ball_y += s_breakout_state.dy;

    if (s_breakout_state.ball_y <= BREAKOUT_PLAYFIELD_TOP)
    {
        s_breakout_state.ball_y = BREAKOUT_PLAYFIELD_TOP;
        s_breakout_state.dy = (int8_t)(-s_breakout_state.dy);
        breakout_play_tone_(520U, 8U);
    }

    if (s_breakout_state.ball_x <= 0)
    {
        s_breakout_state.ball_x = 0;
        s_breakout_state.dx = (int8_t)(-s_breakout_state.dx);
        breakout_play_tone_(520U, 8U);
    }
    else if (s_breakout_state.ball_x >= (ARDUBOY_WIDTH - BREAKOUT_BALL_SIZE))
    {
        s_breakout_state.ball_x = (ARDUBOY_WIDTH - BREAKOUT_BALL_SIZE);
        s_breakout_state.dx = (int8_t)(-s_breakout_state.dx);
        breakout_play_tone_(520U, 8U);
    }

    if ((s_breakout_state.ball_y + BREAKOUT_BALL_SIZE) >= BREAKOUT_PADDLE_Y &&
        s_breakout_state.ball_y <= (BREAKOUT_PADDLE_Y + BREAKOUT_PADDLE_H) &&
        (s_breakout_state.ball_x + BREAKOUT_BALL_SIZE) >= s_breakout_state.paddle_x &&
        s_breakout_state.ball_x <= (s_breakout_state.paddle_x + BREAKOUT_PADDLE_W))
    {
        int16_t offset;

        s_breakout_state.ball_y = BREAKOUT_PADDLE_Y - BREAKOUT_BALL_SIZE;
        s_breakout_state.dy = -1;

        offset = (s_breakout_state.ball_x + 1) - paddle_center;
        if (offset <= -4)
        {
            s_breakout_state.dx = -2;
        }
        else if (offset < 0)
        {
            s_breakout_state.dx = -1;
        }
        else if (offset >= 4)
        {
            s_breakout_state.dx = 2;
        }
        else
        {
            s_breakout_state.dx = 1;
        }

        breakout_play_tone_(220U, 10U);
    }

    for (uint8_t row = 0; row < BREAKOUT_ROWS; ++row)
    {
        bool row_done = false;

        for (uint8_t col = 0; col < BREAKOUT_COLS; ++col)
        {
            const int16_t brick_x = (int16_t)(col * BREAKOUT_BRICK_STEP_X);
            const int16_t brick_y = (int16_t)(BREAKOUT_BRICK_TOP + row * BREAKOUT_BRICK_STEP_Y);

            if (s_breakout_state.bricks[row][col])
            {
                continue;
            }

            if ((s_breakout_state.ball_x + BREAKOUT_BALL_SIZE) < brick_x ||
                s_breakout_state.ball_x > (brick_x + BREAKOUT_BRICK_W) ||
                (s_breakout_state.ball_y + BREAKOUT_BALL_SIZE) < brick_y ||
                s_breakout_state.ball_y > (brick_y + BREAKOUT_BRICK_H))
            {
                continue;
            }

            breakout_hit_brick_(row, col, prev_x, prev_y);
            row_done = true;
            break;
        }

        if (row_done)
        {
            break;
        }
    }

    if (s_breakout_state.ball_y >= ARDUBOY_HEIGHT)
    {
        breakout_on_life_lost_();
        return;
    }

    if (s_breakout_state.brick_count >= (BREAKOUT_ROWS * BREAKOUT_COLS))
    {
        s_breakout_state.level++;
        breakout_reset_bricks_();
        breakout_play_tone_(660U, 18U);
    }
}

/**
 * @brief Internal helper for `breakout_update_play`.
 *
 * @param[in] a_clicked Parameter passed to the function.
 * @param[in] b_clicked Parameter passed to the function.
 * @return void
 */
static void breakout_update_play_(bool a_clicked, bool b_clicked)
{
    if (b_clicked)
    {
        breakout_request_exit_();
        return;
    }

    if (a_clicked)
    {
        if (!s_breakout_state.released)
        {
            s_breakout_state.released = true;
            s_breakout_state.dx = breakout_random_dx_();
            s_breakout_state.dy = -1;
            breakout_play_tone_(800U, 10U);
        }
        else
        {
            s_breakout_state.paused = !s_breakout_state.paused;
            breakout_play_tone_(440U, 10U);
        }
    }

    breakout_move_paddle_();

    if (!s_breakout_state.paused && s_breakout_state.released)
    {
        breakout_move_ball_();
    }
}

/**
 * @brief Internal helper for `breakout_update`.
 *
 * @return void
 */
static void breakout_update_(void)
{
    const bool a_clicked = s_breakout_a_clicked;
    const bool b_clicked = s_breakout_b_clicked;

    s_breakout_a_clicked = false;
    s_breakout_b_clicked = false;

    switch (s_breakout_state.screen)
    {
        case BREAKOUT_SCREEN_TITLE:
            if (b_clicked)
            {
                breakout_request_exit_();
            }
            else if (a_clicked)
            {
                breakout_start_run_();
                breakout_play_tone_(740U, 12U);
            }
            breakout_render_title_();
            break;

        case BREAKOUT_SCREEN_PLAY:
            breakout_update_play_(a_clicked, b_clicked);
            breakout_render_play_();
            break;

        case BREAKOUT_SCREEN_GAME_OVER:
            if (b_clicked)
            {
                breakout_request_exit_();
            }
            else if (a_clicked)
            {
                breakout_start_run_();
                breakout_play_tone_(740U, 12U);
            }
            breakout_render_game_over_();
            break;

        default:
            break;
    }
}

/**
 * @brief Internal helper for `breakout_cleanup`.
 *
 * @return void
 */
static void breakout_cleanup_(void)
{
    poom_breakout_exit_cb_t exit_cb = s_breakout_exit_cb;
    void *exit_ctx = s_breakout_exit_cb_ctx;

    if (s_breakout_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", breakout_button_cb_, s_breakout_sbus_user);
        s_breakout_buttons_subscribed = false;
    }

    s_breakout_running = false;
    s_breakout_exit_requested = false;
    s_breakout_left_down = false;
    s_breakout_right_down = false;
    s_breakout_a_clicked = false;
    s_breakout_b_clicked = false;
    s_breakout_task = NULL;

    poom_arduboy_clear();
    poom_arduboy_display();

    if (exit_cb != NULL)
    {
        exit_cb(exit_ctx);
    }
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void breakout_task_(void *arg)
{
    (void)arg;

    poom_arduboy_begin();
    buzzer_init(PIN_NUM_BUZZER);

    memset(&s_breakout_state, 0, sizeof(s_breakout_state));
    s_breakout_state.screen = BREAKOUT_SCREEN_TITLE;

    while (!s_breakout_exit_requested)
    {
        breakout_update_();
        vTaskDelay(pdMS_TO_TICKS(BREAKOUT_FRAME_MS));
    }

    breakout_cleanup_();
    vTaskDelete(NULL);
}

/**
 * @brief Handles button events for this module.
 *
 * @param[in] msg Parameter passed to the function.
 * @param[in] user_ctx Parameter passed to the function.
 * @return void
 */
static void breakout_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    button_event_msg_t ev;

    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event == BUTTON_PRESS_DOWN)
    {
        if (ev.button == BTN_LEFT)
        {
            s_breakout_left_down = true;
        }
        else if (ev.button == BTN_RIGHT)
        {
            s_breakout_right_down = true;
        }
    }
    else if (ev.event == BUTTON_PRESS_UP)
    {
        if (ev.button == BTN_LEFT)
        {
            s_breakout_left_down = false;
        }
        else if (ev.button == BTN_RIGHT)
        {
            s_breakout_right_down = false;
        }
    }
    else if (ev.event == BUTTON_SINGLE_CLICK)
    {
        if (ev.button == BTN_A)
        {
            s_breakout_a_clicked = true;
        }
        else if (ev.button == BTN_B)
        {
            s_breakout_b_clicked = true;
        }
    }
}

/**
 * @brief Releases internal state before leaving this module.
 *
 * @param[in] user_ctx Parameter passed to the function.
 * @return void
 */
static void breakout_menu_exit_cb_(void *user_ctx)
{
    const uint8_t token = 1U;

    (void)user_ctx;

    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

esp_err_t poom_breakout_start(void)
{
    if (s_breakout_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_breakout_exit_requested = false;
    s_breakout_left_down = false;
    s_breakout_right_down = false;
    s_breakout_a_clicked = false;
    s_breakout_b_clicked = false;

    if (!poom_sbus_subscribe_cb("input/button", breakout_button_cb_, s_breakout_sbus_user))
    {
        return ESP_FAIL;
    }
    s_breakout_buttons_subscribed = true;

    if (xTaskCreate(breakout_task_,
                    "poom_breakout",
                    BREAKOUT_TASK_STACK,
                    NULL,
                    BREAKOUT_TASK_PRIO,
                    &s_breakout_task) != pdPASS)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", breakout_button_cb_, s_breakout_sbus_user);
        s_breakout_buttons_subscribed = false;
        s_breakout_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_breakout_running = true;
    return ESP_OK;
}

esp_err_t poom_breakout_stop(void)
{
    if (!s_breakout_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    breakout_request_exit_();
    return ESP_OK;
}

bool poom_breakout_is_running(void)
{
    return s_breakout_running;
}

esp_err_t poom_breakout_set_exit_callback(poom_breakout_exit_cb_t callback, void *user_ctx)
{
    s_breakout_exit_cb = callback;
    s_breakout_exit_cb_ctx = user_ctx;
    return ESP_OK;
}

void app_breakout_menu(void)
{
    (void)poom_breakout_set_exit_callback(breakout_menu_exit_cb_, NULL);
    (void)poom_breakout_start();
}
