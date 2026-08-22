// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Arduboy2.h"
#include "menu_cli_zigbee.h"

#include "cli.h"
#include "cli_zigbee.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

#define MENU_CLI_ZB_REFRESH_MS (250U)
#define MENU_CLI_ZB_UI_STACK (3072U)
#define MENU_CLI_ZB_PRIO (4U)

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} button_event_msg_t;

static bool s_menu_cli_zb_active = false;
static bool s_menu_cli_zb_buttons_subscribed = false;
static bool s_menu_cli_zb_exit_requested = false;
static TaskHandle_t s_menu_cli_zb_task = NULL;
static char s_menu_cli_zb_sbus_user[] = "menu_cli_zigbee";

static void menu_cli_zb_draw_(void);
static void menu_cli_zb_task_(void *task_arg);
static void menu_cli_zb_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_cli_zb_draw_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(33, 2);
    (void)poom_arduboy_print(F("CLI ZIGBEE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(F("Open USB console"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(F("Prompt: PoomBee"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(F("Type: help"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(F("B exits + kills"));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_cli_zb_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if ((ev.event == BUTTON_SINGLE_CLICK) && (ev.button == BTN_B))
    {
        s_menu_cli_zb_exit_requested = true;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_cli_zb_task_(void *task_arg)
{
    (void)task_arg;

    menu_cli_zb_draw_();
    while (s_menu_cli_zb_active)
    {
        if (s_menu_cli_zb_exit_requested)
        {
            s_menu_cli_zb_active = false;
            s_menu_cli_zb_exit_requested = false;

            if (s_menu_cli_zb_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", menu_cli_zb_button_cb_, s_menu_cli_zb_sbus_user);
                s_menu_cli_zb_buttons_subscribed = false;
            }

            cli_poom_zigbee_stop();
            poom_console_resume();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_CLI_ZB_REFRESH_MS));
    }

    s_menu_cli_zb_task = NULL;
    vTaskDelete(NULL);
}

void menu_cli_zigbee(void)
{
    if (s_menu_cli_zb_task != NULL)
    {
        return;
    }

    s_menu_cli_zb_active = true;
    s_menu_cli_zb_exit_requested = false;

    if (!s_menu_cli_zb_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_cli_zb_button_cb_, s_menu_cli_zb_sbus_user))
        {
            s_menu_cli_zb_buttons_subscribed = true;
        }
        else
        {
            s_menu_cli_zb_active = false;
            menu_cli_zb_draw_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    poom_console_pause();
    cli_poom_zigbee_begin();

    (void)xTaskCreate(menu_cli_zb_task_,
                     "menu_cli_zigbee",
                     MENU_CLI_ZB_UI_STACK,
                     NULL,
                     MENU_CLI_ZB_PRIO,
                     &s_menu_cli_zb_task);
}
