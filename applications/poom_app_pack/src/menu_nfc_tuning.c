// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_nfc_tuning.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_nfc_controller.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_NFC_TUNE_REFRESH_MS (200U)
#define MENU_NFC_TUNE_STACK (3584U)
#define MENU_NFC_TUNE_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define LINE_Y0 (14)
#define LINE_STEP (10)

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
} menu_nfc_tune_button_msg_t;

static bool s_menu_nfc_tune_active = false;
static bool s_menu_nfc_tune_buttons_subscribed = false;
static bool s_menu_nfc_tune_exit_requested = false;
static TaskHandle_t s_menu_nfc_tune_ui_task = NULL;
static QueueHandle_t s_menu_nfc_tune_btn_q = NULL;
static char s_menu_nfc_tune_sbus_user[] = "menu_nfc_tuning";

static poom_nfc_tuning_result_t s_last = {0};
static bool s_has_last = false;

static char s_status[22] = "READY";

static void menu_nfc_tune_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_nfc_tune_ui_task_(void* arg);

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_tune_draw_frame_(void)
{
    char st_short[8];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(32, 2);
    (void)poom_arduboy_print(F("NFC TUNE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    (void)snprintf(st_short, sizeof(st_short), "%.7s", s_status);
    poom_arduboy_set_cursor(86, 2);
    (void)poom_arduboy_print(st_short);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] line0 Parameter passed to the helper.
 * @param[in] line1 Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_tune_draw_busy_(const char* line0, const char* line1)
{
    menu_nfc_tune_draw_frame_();

    poom_arduboy_set_cursor(10, 24);
    (void)poom_arduboy_print(line0 ? line0 : "Working...");

    if(line1 != NULL && line1[0] != '\0')
    {
        poom_arduboy_set_cursor(10, 34);
        (void)poom_arduboy_print(line1);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_nfc_tune_render_(void)
{
    char line0[22];
    char line1[22];
    char line2[22];
    char line3[22];

    menu_nfc_tune_draw_frame_();

    if(s_has_last)
    {
        (void)snprintf(line0, sizeof(line0), "AAT_A:%3u AAT_B:%3u",
                       (unsigned)s_last.aat_a, (unsigned)s_last.aat_b);
        (void)snprintf(line1, sizeof(line1), "PHASE:%4d deg", (int)s_last.phase_degree);
        (void)snprintf(line2, sizeof(line2), "AMP:%5d mVpp", (int)s_last.amplitude_mvpp);
        if(s_last.measure_count > 0U)
        {
            (void)snprintf(line3, sizeof(line3), "MEAS:%4u", (unsigned)s_last.measure_count);
        }
        else
        {
            (void)snprintf(line3, sizeof(line3), "MEAS:<none>");
        }
    }
    else
    {
        (void)snprintf(line0, sizeof(line0), "AAT_A:--- AAT_B:---");
        (void)snprintf(line1, sizeof(line1), "PHASE:---- deg");
        (void)snprintf(line2, sizeof(line2), "AMP:----- mVpp");
        (void)snprintf(line3, sizeof(line3), "MEAS:<none>");
    }

    poom_arduboy_set_cursor(4, (int16_t)(LINE_Y0 + 0 * LINE_STEP));
    (void)poom_arduboy_print(line0);
    poom_arduboy_set_cursor(4, (int16_t)(LINE_Y0 + 1 * LINE_STEP));
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, (int16_t)(LINE_Y0 + 2 * LINE_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(4, (int16_t)(LINE_Y0 + 3 * LINE_STEP));
    (void)poom_arduboy_print(line3);

    {
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:AUTO UP:GET"));

        poom_arduboy_set_cursor(85, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_nfc_tune_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_nfc_tune_active = false;
    s_menu_nfc_tune_exit_requested = false;

    if(s_menu_nfc_tune_ui_task != NULL)
    {
        if(s_menu_nfc_tune_ui_task != current_task)
        {
            TaskHandle_t t = s_menu_nfc_tune_ui_task;
            s_menu_nfc_tune_ui_task = NULL;
            vTaskDelete(t);
        }
        else
        {
            s_menu_nfc_tune_ui_task = NULL;
        }
    }

    if(s_menu_nfc_tune_btn_q != NULL)
    {
        QueueHandle_t q = s_menu_nfc_tune_btn_q;
        s_menu_nfc_tune_btn_q = NULL;
        vQueueDelete(q);
    }

    if(s_menu_nfc_tune_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_nfc_tune_button_cb_, s_menu_nfc_tune_sbus_user);
        s_menu_nfc_tune_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Internal helper for `menu_nfc_tune_do_get`.
 *
 * @return void
 */
static void menu_nfc_tune_do_get_(void)
{
    menu_nfc_tune_draw_busy_("Reading...", "");
    s_has_last = poom_nfc_controller_tune_get(&s_last);
    (void)snprintf(s_status, sizeof(s_status), s_has_last ? "GET OK" : "GET FAIL");
}

/**
 * @brief Internal helper for `menu_nfc_tune_do_auto`.
 *
 * @return void
 */
static void menu_nfc_tune_do_auto_(void)
{
    menu_nfc_tune_draw_busy_("Auto tuning...", "");
    s_has_last = poom_nfc_controller_tune_auto(&s_last);
    (void)snprintf(s_status, sizeof(s_status), s_has_last ? "AUTO OK" : "AUTO FAIL");
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_tune_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if(!s_menu_nfc_tune_active)
    {
        return;
    }
    if(msg == NULL || msg->len < sizeof(button_event_msg_t))
    {
        return;
    }
    if(s_menu_nfc_tune_btn_q == NULL)
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    menu_nfc_tune_button_msg_t qmsg = {
        .button = ev.button,
        .event = ev.event,
        .ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
    };

    (void)xQueueSend(s_menu_nfc_tune_btn_q, &qmsg, 0);
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_tune_ui_task_(void* arg)
{
    (void)arg;

    menu_nfc_tune_do_get_();
    menu_nfc_tune_render_();

    while(s_menu_nfc_tune_active && !s_menu_nfc_tune_exit_requested)
    {
        menu_nfc_tune_button_msg_t msg;
        bool handled = false;

        if(s_menu_nfc_tune_btn_q != NULL &&
           xQueueReceive(s_menu_nfc_tune_btn_q,
                         &msg,
                         pdMS_TO_TICKS(MENU_NFC_TUNE_REFRESH_MS)) == pdTRUE)
        {
            if(msg.event == BUTTON_SINGLE_CLICK)
            {
                if(msg.button == BTN_A)
                {
                    menu_nfc_tune_do_auto_();
                    handled = true;
                }
                else if(msg.button == BTN_UP)
                {
                    menu_nfc_tune_do_get_();
                    handled = true;
                }
                else if(msg.button == BTN_B)
                {
                    s_menu_nfc_tune_exit_requested = true;
                }
            }
        }

        if(s_menu_nfc_tune_exit_requested)
        {
            break;
        }

        if(handled)
        {
            menu_nfc_tune_render_();
        }
    }

    menu_nfc_tune_exit_();
    vTaskDelete(NULL);
}

void menu_nfc_tuning_show(void)
{
    s_menu_nfc_tune_active = true;
    s_menu_nfc_tune_exit_requested = false;
    s_has_last = false;
    (void)memset(&s_last, 0, sizeof(s_last));
    (void)snprintf(s_status, sizeof(s_status), "READY");

    if(!s_menu_nfc_tune_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_nfc_tune_button_cb_, s_menu_nfc_tune_sbus_user))
        {
            s_menu_nfc_tune_buttons_subscribed = true;
        }
        else
        {
            s_menu_nfc_tune_active = false;
            poom_arduboy_clear();
            poom_arduboy_set_text_size(1);
            poom_arduboy_set_cursor(12, 26);
            (void)poom_arduboy_print(F("Button sub err"));
            poom_arduboy_display();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_nfc_tune_btn_q == NULL)
    {
        s_menu_nfc_tune_btn_q = xQueueCreate(8, sizeof(menu_nfc_tune_button_msg_t));
        if(s_menu_nfc_tune_btn_q == NULL)
        {
            s_menu_nfc_tune_active = false;
            poom_arduboy_clear();
            poom_arduboy_set_text_size(1);
            poom_arduboy_set_cursor(18, 26);
            (void)poom_arduboy_print(F("Queue error"));
            poom_arduboy_display();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_nfc_tune_ui_task == NULL)
    {
        (void)xTaskCreate(menu_nfc_tune_ui_task_,
                          "menu_nfc_tune",
                          MENU_NFC_TUNE_STACK,
                          NULL,
                          MENU_NFC_TUNE_PRIO,
                          &s_menu_nfc_tune_ui_task);
    }
}
