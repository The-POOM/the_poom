// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ble_spam.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_ble_spam.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define MENU_BLE_SPAM_REFRESH_MS (250U)
#define MENU_BLE_SPAM_TASK_STACK (3072U)
#define MENU_BLE_SPAM_TASK_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

static bool s_menu_ble_spam_active = false;
static bool s_menu_ble_spam_running = false;
static bool s_menu_ble_spam_buttons_subscribed = false;
static bool s_menu_ble_spam_exit_requested = false;
static TaskHandle_t s_menu_ble_spam_task = NULL;
static char s_menu_ble_spam_sbus_user[] = "menu_ble_spam";
static char s_menu_ble_spam_status[22] = "Starting...";
static char s_menu_ble_spam_name[22] = "scanning...";

static void menu_ble_spam_draw_(void);
static void menu_ble_spam_exit_(void);
static void menu_ble_spam_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Internal helper for `menu_ble_spam_on_name`.
 *
 * @param[in] name Parameter passed to the helper.
 * @return void
 */
static void menu_ble_spam_on_name_(const char *name)
{
    if (name == NULL)
    {
        return;
    }

    (void)snprintf(s_menu_ble_spam_name, sizeof(s_menu_ble_spam_name), "%.14s", name);
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_ble_spam_draw_header_(void)
{
    poom_arduboy_set_cursor(40, 2);
    (void)poom_arduboy_print(F("BLE SPAM"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_ble_spam_draw_(void)
{
    char line_state[22];

    (void)snprintf(line_state, sizeof(line_state), "State: %s", s_menu_ble_spam_running ? "RUNNING" : "PAUSED");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ble_spam_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_state);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(F("Name:"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(s_menu_ble_spam_name);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(s_menu_ble_spam_status);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:TOGGLE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_ble_spam_task_(void *arg)
{
    (void)arg;

    while (s_menu_ble_spam_active)
    {
        if (s_menu_ble_spam_exit_requested)
        {
            menu_ble_spam_exit_();
            break;
        }

        menu_ble_spam_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_BLE_SPAM_REFRESH_MS));
    }

    s_menu_ble_spam_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_ble_spam_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_ble_spam_active = false;
    s_menu_ble_spam_exit_requested = false;

    poom_ble_spam_register_cb(NULL);
    poom_ble_spam_app_stop();
    s_menu_ble_spam_running = false;

    if (s_menu_ble_spam_task != NULL)
    {
        if (s_menu_ble_spam_task != current_task)
        {
            TaskHandle_t task = s_menu_ble_spam_task;
            s_menu_ble_spam_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_ble_spam_task = NULL;
        }
    }

    if (s_menu_ble_spam_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ble_spam_button_cb_, s_menu_ble_spam_sbus_user);
        s_menu_ble_spam_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Toggles the current runtime state.
 *
 * @return void
 */
static void menu_ble_spam_toggle_(void)
{
    if (s_menu_ble_spam_running)
    {
        poom_ble_spam_app_stop();
        s_menu_ble_spam_running = false;
        (void)snprintf(s_menu_ble_spam_status, sizeof(s_menu_ble_spam_status), "Paused");
        (void)snprintf(s_menu_ble_spam_name, sizeof(s_menu_ble_spam_name), "-");
        return;
    }

    poom_ble_spam_start();
    s_menu_ble_spam_running = true;
    (void)snprintf(s_menu_ble_spam_status, sizeof(s_menu_ble_spam_status), "Running");
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_ble_spam_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BUTTON_B)
    {
        if (s_menu_ble_spam_task == NULL)
        {
            menu_ble_spam_exit_();
        }
        else
        {
            s_menu_ble_spam_exit_requested = true;
        }
        return;
    }

    if (ev.button == BUTTON_A)
    {
        menu_ble_spam_toggle_();
        menu_ble_spam_draw_();
    }
}

void menu_ble_spam_display(void)
{
    s_menu_ble_spam_active = true;
    s_menu_ble_spam_exit_requested = false;

    s_menu_ble_spam_running = true;
    (void)snprintf(s_menu_ble_spam_status, sizeof(s_menu_ble_spam_status), "Running");
    (void)snprintf(s_menu_ble_spam_name, sizeof(s_menu_ble_spam_name), "scanning...");

    poom_ble_spam_register_cb(menu_ble_spam_on_name_);
    poom_ble_spam_start();

    if (!s_menu_ble_spam_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_ble_spam_button_cb_, s_menu_ble_spam_sbus_user))
        {
            s_menu_ble_spam_buttons_subscribed = true;
        }
        else
        {
            s_menu_ble_spam_active = false;
            (void)snprintf(s_menu_ble_spam_status, sizeof(s_menu_ble_spam_status), "Btn sub error");
            menu_ble_spam_draw_();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if (s_menu_ble_spam_task == NULL)
    {
        (void)xTaskCreate(menu_ble_spam_task_,
                          "menu_ble_spam",
                          MENU_BLE_SPAM_TASK_STACK,
                          NULL,
                          MENU_BLE_SPAM_TASK_PRIO,
                          &s_menu_ble_spam_task);
    }

    menu_ble_spam_draw_();
}
