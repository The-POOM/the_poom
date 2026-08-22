// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_i2c_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "input_events.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_I2C_SCAN_REFRESH_MS (120U)
#define MENU_I2C_SCAN_STACK (3072U)
#define MENU_I2C_SCAN_PRIO (4U)
#define MENU_I2C_SCAN_VISIBLE_ROWS (4U)
#define MENU_I2C_SCAN_LIST_Y0 (16)
#define MENU_I2C_SCAN_ROW_STEP (10)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

static bool s_menu_i2c_scan_active = false;
static bool s_menu_i2c_scan_buttons_subscribed = false;
static volatile bool s_menu_i2c_scan_exit_requested = false;
static volatile bool s_menu_i2c_scan_rescan_requested = false;
static volatile bool s_menu_i2c_scan_scan_in_progress = false;
static TaskHandle_t s_menu_i2c_scan_task = NULL;
static char s_menu_i2c_scan_sbus_user[] = "menu_i2c_scan";
static uint8_t *s_menu_i2c_scan_addrs = NULL;
static size_t s_menu_i2c_scan_count = 0U;
static uint8_t s_menu_i2c_scan_scroll = 0U;

static void menu_i2c_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_i2c_scan_task_(void *arg);

/**
 * @brief Clears the internal state used by this menu module.
 *
 * @return void
 */
static void menu_i2c_scan_clear_results_(void)
{
    free(s_menu_i2c_scan_addrs);
    s_menu_i2c_scan_addrs = NULL;
    s_menu_i2c_scan_count = 0U;
    s_menu_i2c_scan_scroll = 0U;
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_i2c_scan_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(33, 2);
    (void)poom_arduboy_print(F("I2C SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Refreshes the internal state used by this menu module.
 *
 * @return void
 */
static void menu_i2c_scan_refresh_(void)
{
    uint8_t *found = NULL;
    size_t count = 0U;

    menu_i2c_scan_clear_results_();
    i2c_scan_devices(&found, &count);

    s_menu_i2c_scan_addrs = found;
    s_menu_i2c_scan_count = count;
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] delta Parameter passed to the helper.
 * @return void
 */
static void menu_i2c_scan_scroll_(int delta)
{
    const uint8_t count = (s_menu_i2c_scan_count > 255U) ? 255U : (uint8_t)s_menu_i2c_scan_count;
    const uint8_t max_scroll =
        (count > MENU_I2C_SCAN_VISIBLE_ROWS) ? (uint8_t)(count - MENU_I2C_SCAN_VISIBLE_ROWS) : 0U;
    int next = (int)s_menu_i2c_scan_scroll + delta;

    if (next < 0)
    {
        next = 0;
    }
    else if (next > (int)max_scroll)
    {
        next = (int)max_scroll;
    }

    s_menu_i2c_scan_scroll = (uint8_t)next;
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_i2c_scan_draw_(void)
{
    char line[22];
    const uint8_t count = (s_menu_i2c_scan_count > 255U) ? 255U : (uint8_t)s_menu_i2c_scan_count;

    poom_arduboy_clear();
    menu_i2c_scan_draw_header_();

    (void)snprintf(line, sizeof(line), "%u dev", (unsigned)count);
    poom_arduboy_set_cursor(86, 2);
    (void)poom_arduboy_print(line);

    if (s_menu_i2c_scan_scan_in_progress)
    {
        poom_arduboy_set_cursor(20, 26);
        (void)poom_arduboy_print(F("SCANNING..."));
    }
    else if (count == 0U)
    {
        poom_arduboy_set_cursor(12, 26);
        (void)poom_arduboy_print(F("No devices found"));
    }
    else
    {
        for (uint8_t row = 0U; row < MENU_I2C_SCAN_VISIBLE_ROWS; ++row)
        {
            const uint8_t idx = (uint8_t)(s_menu_i2c_scan_scroll + row);

            if (idx >= count)
            {
                break;
            }

            (void)snprintf(line,
                           sizeof(line),
                           "%u: 0x%02X",
                           (unsigned)(idx + 1U),
                           (unsigned)s_menu_i2c_scan_addrs[idx]);
            poom_arduboy_set_cursor(0, MENU_I2C_SCAN_LIST_Y0 + ((int)row * MENU_I2C_SCAN_ROW_STEP));
            (void)poom_arduboy_print(line);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SCAN U/D"));
    poom_arduboy_set_cursor(78, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_i2c_scan_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_i2c_scan_active = false;
    s_menu_i2c_scan_exit_requested = false;
    s_menu_i2c_scan_rescan_requested = false;
    s_menu_i2c_scan_scan_in_progress = false;

    if (s_menu_i2c_scan_task != NULL)
    {
        if (s_menu_i2c_scan_task != current_task)
        {
            TaskHandle_t task = s_menu_i2c_scan_task;
            s_menu_i2c_scan_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_i2c_scan_task = NULL;
        }
    }

    if (s_menu_i2c_scan_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_i2c_scan_button_cb_, s_menu_i2c_scan_sbus_user);
        s_menu_i2c_scan_buttons_subscribed = false;
    }

    menu_i2c_scan_clear_results_();

    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_i2c_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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
        s_menu_i2c_scan_exit_requested = true;
    }
    else if (ev.button == BUTTON_A)
    {
        s_menu_i2c_scan_rescan_requested = true;
    }
    else if (ev.button == BUTTON_UP)
    {
        menu_i2c_scan_scroll_(-1);
    }
    else if (ev.button == BUTTON_DOWN)
    {
        menu_i2c_scan_scroll_(+1);
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_i2c_scan_task_(void *arg)
{
    (void)arg;

    while (s_menu_i2c_scan_active)
    {
        if (s_menu_i2c_scan_exit_requested)
        {
            menu_i2c_scan_exit_();
            break;
        }

        if (s_menu_i2c_scan_rescan_requested)
        {
            s_menu_i2c_scan_rescan_requested = false;
            s_menu_i2c_scan_scan_in_progress = true;
            menu_i2c_scan_clear_results_();
            menu_i2c_scan_draw_();
            vTaskDelay(1);
            menu_i2c_scan_refresh_();
            s_menu_i2c_scan_scan_in_progress = false;
        }

        menu_i2c_scan_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_I2C_SCAN_REFRESH_MS));
    }

    s_menu_i2c_scan_task = NULL;
    vTaskDelete(NULL);
}

void menu_i2c_scan_show(void)
{
    if (s_menu_i2c_scan_task != NULL)
    {
        return;
    }

    s_menu_i2c_scan_active = true;
    s_menu_i2c_scan_exit_requested = false;
    s_menu_i2c_scan_rescan_requested = true;
    s_menu_i2c_scan_scan_in_progress = false;

    if (!s_menu_i2c_scan_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_i2c_scan_button_cb_, s_menu_i2c_scan_sbus_user))
        {
            s_menu_i2c_scan_buttons_subscribed = true;
        }
        else
        {
            s_menu_i2c_scan_active = false;
            {
                const uint8_t token = 1U;
                (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            }
            return;
        }
    }

    if (xTaskCreate(menu_i2c_scan_task_,
                    "menu_i2c_scan",
                    MENU_I2C_SCAN_STACK,
                    NULL,
                    MENU_I2C_SCAN_PRIO,
                    &s_menu_i2c_scan_task) != pdPASS)
    {
        menu_i2c_scan_exit_();
    }
}
