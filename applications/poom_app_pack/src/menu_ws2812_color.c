// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ws2812_color.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#include "bsp_pong.h"
#include "poom_led_rainbow.h"
#include "ws2812.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_LED_REFRESH_MS (180U)
#define MENU_LED_STACK (3072U)
#define MENU_LED_PRIO (4U)

#define MENU_LED_RESOLUTION_HZ (10 * 1000 * 1000)
#define MENU_LED_IS_RGBW (false)

#define MENU_LED_DEFAULT_BRIGHTNESS (32U)
#define MENU_LED_STEP_PCT (5)

#define ROW_Y0 (16)
#define ROW_STEP (12)
#define ROW_HILITE_H (11)
#define VALUE_X (90)

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif

#ifndef BTN_UP
#define BTN_UP (4U)
#endif

#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_led_button_msg_t;

typedef enum
{
    MENU_LED_ROW_RED = 0,
    MENU_LED_ROW_GREEN,
    MENU_LED_ROW_BLUE,
    MENU_LED_ROW_COUNT,
} menu_led_row_t;

static bool s_led_active = false;
static bool s_led_buttons_subscribed = false;
static volatile bool s_led_exit_requested = false;
static TaskHandle_t s_led_task = NULL;
static char s_led_sbus_user[] = "menu_ws2812_color";

static ws2812_strip_t s_strip;
static bool s_strip_inited = false;
static bool s_rainbow_was_running = false;

static menu_led_row_t s_row = MENU_LED_ROW_RED;
static int s_red_pct = 0;
static int s_green_pct = 0;
static int s_blue_pct = 0;
static bool s_dirty = true;

static void menu_led_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_led_task_(void* arg);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void led_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(40, 2);
    (void)poom_arduboy_print(F("LED RGB"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Internal helper for `pct_to_u8`.
 *
 * @param[in] pct Parameter passed to the helper.
 * @return uint8_t
 */
static uint8_t pct_to_u8_(int pct)
{
    if (pct <= 0)
    {
        return 0U;
    }
    if (pct >= 100)
    {
        return 255U;
    }
    return (uint8_t)((pct * 255 + 50) / 100);
}

/**
 * @brief Internal helper for `led_apply`.
 *
 * @return void
 */
static void led_apply_(void)
{
    if (!s_strip_inited)
    {
        return;
    }

    const uint8_t r = pct_to_u8_(s_red_pct);
    const uint8_t g = pct_to_u8_(s_green_pct);
    const uint8_t b = pct_to_u8_(s_blue_pct);

    ws2812_fill(&s_strip, r, g, b, 0U);
    (void)ws2812_show(&s_strip);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void led_draw_(void)
{
    char val[8];

    poom_arduboy_clear();
    led_draw_header_();

    poom_arduboy_set_cursor(4, ROW_Y0);
    (void)poom_arduboy_print(F("RED"));
    (void)snprintf(val, sizeof(val), "%3d%%", s_red_pct);
    poom_arduboy_set_cursor(VALUE_X, ROW_Y0);
    (void)poom_arduboy_print(val);
    if (s_row == MENU_LED_ROW_RED)
    {
        poom_arduboy_fill_rect(0, (int16_t)(ROW_Y0 - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
    }

    poom_arduboy_set_cursor(4, (int16_t)(ROW_Y0 + ROW_STEP));
    (void)poom_arduboy_print(F("GREEN"));
    (void)snprintf(val, sizeof(val), "%3d%%", s_green_pct);
    poom_arduboy_set_cursor(VALUE_X, (int16_t)(ROW_Y0 + ROW_STEP));
    (void)poom_arduboy_print(val);
    if (s_row == MENU_LED_ROW_GREEN)
    {
        poom_arduboy_fill_rect(0, (int16_t)(ROW_Y0 + ROW_STEP - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
    }

    poom_arduboy_set_cursor(4, (int16_t)(ROW_Y0 + 2 * ROW_STEP));
    (void)poom_arduboy_print(F("BLUE"));
    (void)snprintf(val, sizeof(val), "%3d%%", s_blue_pct);
    poom_arduboy_set_cursor(VALUE_X, (int16_t)(ROW_Y0 + 2 * ROW_STEP));
    (void)poom_arduboy_print(val);
    if (s_row == MENU_LED_ROW_BLUE)
    {
        poom_arduboy_fill_rect(0, (int16_t)(ROW_Y0 + 2 * ROW_STEP - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("L/R:ADJ"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `led_takeover`.
 *
 * @return bool
 */
static bool led_takeover_(void)
{
    if (s_strip_inited)
    {
        return true;
    }

    s_rainbow_was_running = poom_led_rainbow_deinit();

    if (ws2812_init(&s_strip, PIN_NUM_WS2812, PIN_NUM_LEDS, MENU_LED_IS_RGBW, MENU_LED_RESOLUTION_HZ) != ESP_OK)
    {
        poom_led_rainbow_init();
        if (s_rainbow_was_running)
        {
            (void)poom_led_rainbow_start();
        }
        s_rainbow_was_running = false;
        return false;
    }

    ws2812_set_brightness(&s_strip, MENU_LED_DEFAULT_BRIGHTNESS);
    ws2812_clear(&s_strip);
    (void)ws2812_show(&s_strip);
    s_strip_inited = true;
    return true;
}

/**
 * @brief Internal helper for `led_release`.
 *
 * @return void
 */
static void led_release_(void)
{
    if (s_strip_inited)
    {
        ws2812_clear(&s_strip);
        (void)ws2812_show(&s_strip);
        ws2812_deinit(&s_strip);
        s_strip_inited = false;
    }

    poom_led_rainbow_init();
    if (s_rainbow_was_running)
    {
        (void)poom_led_rainbow_start();
    }
    s_rainbow_was_running = false;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_led_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_led_active = false;
    s_led_exit_requested = false;

    led_release_();

    if (s_led_task != NULL)
    {
        if (s_led_task != current_task)
        {
            TaskHandle_t task = s_led_task;
            s_led_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_led_task = NULL;
        }
    }

    if (s_led_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_led_button_cb_, s_led_sbus_user);
        s_led_buttons_subscribed = false;
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Internal helper for `pct_for_row`.
 *
 * @param[in] row Parameter passed to the helper.
 * @return int*
 */
static int* pct_for_row_(menu_led_row_t row)
{
    switch (row)
    {
        case MENU_LED_ROW_RED:   return &s_red_pct;
        case MENU_LED_ROW_GREEN: return &s_green_pct;
        case MENU_LED_ROW_BLUE:  return &s_blue_pct;
        default:                 return &s_red_pct;
    }
}

/**
 * @brief Internal helper for `pct_step`.
 *
 * @param[in] delta Parameter passed to the helper.
 * @return void
 */
static void pct_step_(int delta)
{
    int* pct = pct_for_row_(s_row);
    if (pct == NULL)
    {
        return;
    }

    int v = *pct + delta;
    if (v < 0)
    {
        v = 0;
    }
    if (v > 100)
    {
        v = 100;
    }

    if (v != *pct)
    {
        *pct = v;
        s_dirty = true;
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_led_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(menu_led_button_msg_t)))
    {
        return;
    }

    menu_led_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BTN_B)
    {
        s_led_exit_requested = true;
        return;
    }

    if (ev.button == BTN_UP)
    {
        if (s_row > 0)
        {
            s_row = (menu_led_row_t)((int)s_row - 1);
        }
    }
    else if (ev.button == BTN_DOWN)
    {
        if (((int)s_row + 1) < (int)MENU_LED_ROW_COUNT)
        {
            s_row = (menu_led_row_t)((int)s_row + 1);
        }
    }
    else if (ev.button == BTN_LEFT)
    {
        pct_step_(-MENU_LED_STEP_PCT);
    }
    else if (ev.button == BTN_RIGHT)
    {
        pct_step_(MENU_LED_STEP_PCT);
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_led_task_(void* arg)
{
    (void)arg;

    (void)led_takeover_();
    s_dirty = true;

    while (s_led_active)
    {
        if (s_led_exit_requested)
        {
            menu_led_exit_();
            break;
        }

        if (s_dirty)
        {
            (void)led_takeover_();
            led_apply_();
            s_dirty = false;
        }

        led_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_LED_REFRESH_MS));
    }

    s_led_task = NULL;
    vTaskDelete(NULL);
}

void menu_ws2812_color_show(void)
{
    if (s_led_task != NULL)
    {
        return;
    }

    s_led_active = true;
    s_led_exit_requested = false;

    s_row = MENU_LED_ROW_RED;
    s_red_pct = 0;
    s_green_pct = 0;
    s_blue_pct = 0;
    s_dirty = true;

    if (!s_led_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_led_button_cb_, s_led_sbus_user))
        {
            s_led_buttons_subscribed = true;
        }
        else
        {
            s_led_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_led_task_, "menu_led", MENU_LED_STACK, NULL, MENU_LED_PRIO, &s_led_task);
}
