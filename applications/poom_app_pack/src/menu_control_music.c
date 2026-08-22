// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_control_music.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "poom_ble_keyboard.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN (0U)
#endif

#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP (1U)
#endif

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

static bool s_menu_control_music_connected = false;
static bool s_menu_control_music_buttons_subscribed = false;
static bool s_menu_control_music_left_down = false;
static bool s_menu_control_music_right_down = false;
static char s_menu_control_music_sbus_user[] = "menu_control_music";
static char s_menu_control_music_last_btn[4] = "-";
static char s_menu_control_music_last_action[12] = "-";

static void menu_control_music_draw_(void);
static void menu_control_music_exit_(void);
static void menu_control_music_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Internal helper for menu_control_music_btn_name.
 *
 * @param[in] button Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_control_music_btn_name_(uint8_t button)
{
    switch (button)
    {
        case BUTTON_A:
            return "A";
        case BUTTON_B:
            return "B";
        case BUTTON_LEFT:
            return "L";
        case BUTTON_RIGHT:
            return "R";
        case BUTTON_UP:
            return "U";
        case BUTTON_DOWN:
            return "D";
        default:
            return "?";
    }
}

/**
 * @brief Internal helper for menu_control_music_action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_control_music_action_(uint8_t button)
{
    switch (button)
    {
        case BUTTON_A:
            return "PLAY";
        case BUTTON_B:
            return "PAUSE";
        case BUTTON_UP:
            return "VOL+";
        case BUTTON_DOWN:
            return "VOL-";
        case BUTTON_RIGHT:
            return "NEXT";
        case BUTTON_LEFT:
            return "BACK";
        default:
            return "-";
    }
}

/**
 * @brief Internal helper for `menu_control_music_on_connection`.
 *
 * @param[in] connected Parameter passed to the helper.
 * @return void
 */
static void menu_control_music_on_connection_(bool connected)
{
    s_menu_control_music_connected = connected;
    menu_control_music_draw_();
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_control_music_draw_header_(void)
{
    poom_arduboy_set_cursor(22, 2);
    (void)poom_arduboy_print(F("MEDIA CONTROL"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_control_music_draw_(void)
{
    char line_ble[22];
    char line_btn[22];
    char line_act[22];

    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", s_menu_control_music_connected ? "CONNECTED" : "PAIRING");
    (void)snprintf(line_btn, sizeof(line_btn), "BTN: %s", s_menu_control_music_last_btn);
    (void)snprintf(line_act, sizeof(line_act), "ACT: %s", s_menu_control_music_last_action);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_control_music_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_ble);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line_btn);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line_act);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("L+R:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_control_music_exit_(void)
{
    poom_ble_keyboard_stop();
    s_menu_control_music_connected = false;

    if (s_menu_control_music_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_control_music_button_cb_, s_menu_control_music_sbus_user);
        s_menu_control_music_buttons_subscribed = false;
    }

    s_menu_control_music_left_down = false;
    s_menu_control_music_right_down = false;

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_control_music_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event == BUTTON_PRESS_DOWN)
    {
        (void)snprintf(s_menu_control_music_last_btn, sizeof(s_menu_control_music_last_btn), "%s", menu_control_music_btn_name_(ev.button));
        (void)snprintf(s_menu_control_music_last_action, sizeof(s_menu_control_music_last_action), "%s", menu_control_music_action_(ev.button));

        if (ev.button == BUTTON_LEFT)
        {
            s_menu_control_music_left_down = true;
        }
        else if (ev.button == BUTTON_RIGHT)
        {
            s_menu_control_music_right_down = true;
        }

        if (s_menu_control_music_left_down && s_menu_control_music_right_down)
        {
            menu_control_music_exit_();
            return;
        }

        menu_control_music_draw_();
    }
    else if (ev.event == BUTTON_PRESS_UP)
    {
        if (ev.button == BUTTON_LEFT)
        {
            s_menu_control_music_left_down = false;
        }
        else if (ev.button == BUTTON_RIGHT)
        {
            s_menu_control_music_right_down = false;
        }
    }
}

void menu_control_init(void)
{
    poom_ble_keyboard_set_connection_callback(menu_control_music_on_connection_);

    s_menu_control_music_connected = poom_ble_keyboard_is_connected();
    s_menu_control_music_left_down = false;
    s_menu_control_music_right_down = false;
    (void)snprintf(s_menu_control_music_last_btn, sizeof(s_menu_control_music_last_btn), "%s", "-");
    (void)snprintf(s_menu_control_music_last_action, sizeof(s_menu_control_music_last_action), "%s", "-");

    if (!s_menu_control_music_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_control_music_button_cb_, s_menu_control_music_sbus_user))
        {
            s_menu_control_music_buttons_subscribed = true;
        }
        else
        {
            menu_control_music_draw_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    poom_ble_keyboard_set_keyboard_mode(false);
    poom_ble_keyboard_start();

    menu_control_music_draw_();
}
