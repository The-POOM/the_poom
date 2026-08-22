// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "poom_wii.h"
#include "menu_air_ble.h"
#include "Arduboy2.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"


static bool s_air_ble_initialized = false;
static bool s_air_ble_initializing = false;
static bool s_air_ble_init_ok = true;
static bool s_air_ble_handler_registered = false;
static bool s_air_ble_buttons_subscribed = false;

static void menu_air_ble_on_button_event_(const poom_sbus_msg_t *msg, void *user);

#ifndef BTN_A
#define BTN_A 0
#endif

#ifndef BTN_LEFT
#define BTN_LEFT 2
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT 3
#endif

#ifndef BTN_UP
#define BTN_UP 4
#endif

#ifndef BTN_DOWN
#define BTN_DOWN 5
#endif

#ifndef BTN_B
#define BTN_B 1
#endif

#define MENU_AIR_BLE_KEY_A 'a'
#define MENU_AIR_BLE_KEY_LEFT 'q'
#define MENU_AIR_BLE_KEY_RIGHT 'e'

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN 0
#endif

#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP 1
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} button_event_msg_t;

/**
 * @brief Draws the current menu state.
 *
 * @param[in] init_ok Parameter passed to the helper.
 * @param[in] running Parameter passed to the helper.
 * @return void
 */
static void menu_air_ble_draw_(bool init_ok, bool running)
{
    char line_init[18];
    char line_run[18];
    char line_hint[18];

    (void)snprintf(line_init, sizeof(line_init), "Init: %s", init_ok ? "OK" : "ERROR");
    if(s_air_ble_initializing)
    {
        (void)snprintf(line_run, sizeof(line_run), "State: init...");
    }
    else
    {
        (void)snprintf(line_run, sizeof(line_run), "State: %s", running ? "Running" : "wait");
    }
    (void)snprintf(line_hint, sizeof(line_hint), "Move to POOM");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(40, 2);
    (void)poom_arduboy_print(F("POOM WII"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line_init);
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print(line_run);
    poom_arduboy_set_cursor(4, 40);
    (void)poom_arduboy_print(line_hint);

    poom_arduboy_set_cursor(76, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Handles connection state updates.
 *
 * @param[in] connected Parameter passed to the helper.
 * @return void
 */
static void menu_air_ble_connection_handler_(bool connected)
{
    bool running = (connected && poom_wii_is_running());
    menu_air_ble_draw_(s_air_ble_init_ok, running);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_air_ble_exit_(void)
{
    if (s_air_ble_handler_registered)
    {
        poom_wii_set_connection_handler(NULL);
        s_air_ble_handler_registered = false;
    }

    if (s_air_ble_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_air_ble_on_button_event_, "menu_air_ble");
        s_air_ble_buttons_subscribed = false;
    }

    poom_wii_key_release_all();
    poom_wii_stop();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_air_ble_on_button_event_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t ev;

    (void)user;

    if ((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if ((ev.button == BTN_B) && (ev.event == BUTTON_SINGLE_CLICK))
    {
        menu_air_ble_exit_();
        return;
    }

    if (ev.button == BTN_A)
    {
        if (ev.event == BUTTON_SINGLE_CLICK)
        {
            poom_wii_toggle_motion_enabled();
        }
    }
    else if (ev.button == BTN_LEFT)
    {
        if (ev.event == BUTTON_PRESS_DOWN)
        {
            poom_wii_key_press_ascii(MENU_AIR_BLE_KEY_LEFT);
        }
        else if (ev.event == BUTTON_PRESS_UP)
        {
            poom_wii_key_release_ascii(MENU_AIR_BLE_KEY_LEFT);
        }
    }
    else if (ev.button == BTN_RIGHT)
    {
        if (ev.event == BUTTON_PRESS_DOWN)
        {
            poom_wii_key_press_ascii(MENU_AIR_BLE_KEY_RIGHT);
        }
        else if (ev.event == BUTTON_PRESS_UP)
        {
            poom_wii_key_release_ascii(MENU_AIR_BLE_KEY_RIGHT);
        }
    }
    else if (ev.button == BTN_UP)
    {
        if (ev.event == BUTTON_PRESS_DOWN)
        {
            poom_wii_key_press_ascii('w');
        }
        else if (ev.event == BUTTON_PRESS_UP)
        {
            poom_wii_key_release_ascii('w');
        }
    }
    else if (ev.button == BTN_DOWN)
    {
        if (ev.event == BUTTON_PRESS_DOWN)
        {
            poom_wii_key_press_ascii('s');
        }
        else if (ev.event == BUTTON_PRESS_UP)
        {
            poom_wii_key_release_ascii('s');
        }
    }
}

void menu_air_ble_display(void)
{
    bool init_ok = s_air_ble_init_ok;

    if (!s_air_ble_initialized)
    {
        s_air_ble_initializing = true;
        menu_air_ble_draw_(true, false);

        if (poom_wii_init() == 0U)
        {
            s_air_ble_initialized = true;
            init_ok = true;
        }
        else
        {
            init_ok = false;
        }

        s_air_ble_init_ok = init_ok;

        s_air_ble_initializing = false;
    }

    if (s_air_ble_initialized && !s_air_ble_handler_registered)
    {
        poom_wii_set_connection_handler(menu_air_ble_connection_handler_);
        s_air_ble_handler_registered = true;
    }

    if (!s_air_ble_buttons_subscribed)
    {
        poom_sbus_subscribe_cb("input/button", menu_air_ble_on_button_event_, "menu_air_ble");
        s_air_ble_buttons_subscribed = true;
    }

    if (init_ok && s_air_ble_initialized)
    {
        poom_wii_start();
    }

    menu_air_ble_draw_(init_ok, (poom_wii_is_connected() && poom_wii_is_running()));
}
