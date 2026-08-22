// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ble_control.h"

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

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

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

static bool s_menu_ble_control_connected = false;
static bool s_menu_ble_control_left_down = false;
static bool s_menu_ble_control_right_down = false;
static bool s_menu_ble_control_buttons_subscribed = false;
static char s_menu_ble_control_sbus_user[] = "menu_ble_control";

static void menu_ble_control_draw_(void);
static void menu_ble_control_exit_(void);
static void menu_ble_control_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

void estado_ble(bool value)
{
    s_menu_ble_control_connected = value;
    menu_ble_control_draw_();
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_ble_control_draw_header_(void)
{
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("TINY CONTROL"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_ble_control_draw_(void)
{
    char line_ble[22];
    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", s_menu_ble_control_connected ? "CONNECTED" : "PAIRING");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ble_control_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_ble);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(F("HID keyboard"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(F("Hold LEFT+RIGHT"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(F("to exit"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("L+R:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_ble_control_exit_(void)
{
    poom_ble_keyboard_stop();

    if (s_menu_ble_control_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ble_control_button_cb_, s_menu_ble_control_sbus_user);
        s_menu_ble_control_buttons_subscribed = false;
    }

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
static void menu_ble_control_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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
        if (ev.button == BUTTON_LEFT)
        {
            s_menu_ble_control_left_down = true;
        }
        else if (ev.button == BUTTON_RIGHT)
        {
            s_menu_ble_control_right_down = true;
        }

        if (s_menu_ble_control_left_down && s_menu_ble_control_right_down)
        {
            menu_ble_control_exit_();
        }
    }
    else if (ev.event == BUTTON_PRESS_UP)
    {
        if (ev.button == BUTTON_LEFT)
        {
            s_menu_ble_control_left_down = false;
        }
        else if (ev.button == BUTTON_RIGHT)
        {
            s_menu_ble_control_right_down = false;
        }
    }
}

void menu_control_display(void)
{
    poom_ble_keyboard_set_connection_callback(estado_ble);

    s_menu_ble_control_left_down = false;
    s_menu_ble_control_right_down = false;

    if (!s_menu_ble_control_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_ble_control_button_cb_, s_menu_ble_control_sbus_user))
        {
            s_menu_ble_control_buttons_subscribed = true;
        }
        else
        {
            menu_ble_control_draw_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    menu_ble_control_draw_();
}
