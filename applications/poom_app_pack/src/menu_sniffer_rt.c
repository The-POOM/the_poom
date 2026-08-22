// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_sniffer_rt.h"

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
#include "menu_ble_scan.h"
#include "poom_ieee802154_sniffer.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_SNIFFER_RT_REFRESH_MS (250U)
#define MENU_SNIFFER_RT_STACK (3072U)
#define MENU_SNIFFER_RT_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)
#define TEXT_X (4)
#define LIST_Y0 (18)
#define ROW0_Y (16)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_SNIFFER_RT_SCREEN_SELECT = 0,
    MENU_SNIFFER_RT_SCREEN_IEEE802154_CH,
    MENU_SNIFFER_RT_SCREEN_IEEE802154_RUNNING,
} menu_sniffer_rt_screen_t;

typedef enum
{
    MENU_SNIFFER_RT_MODE_BLE = 0,
    MENU_SNIFFER_RT_MODE_IEEE802154,
    MENU_SNIFFER_RT_MODE_COUNT,
} menu_sniffer_rt_mode_t;

typedef enum
{
    MENU_SNIFFER_RT_LAUNCH_NONE = 0,
    MENU_SNIFFER_RT_LAUNCH_BLE,
} menu_sniffer_rt_launch_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_sniffer_rt_button_msg_t;

static bool s_menu_sniffer_rt_active = false;
static bool s_menu_sniffer_rt_buttons_subscribed = false;
static bool s_menu_sniffer_rt_exit_requested = false;
static TaskHandle_t s_menu_sniffer_rt_task = NULL;
static char s_menu_sniffer_rt_sbus_user[] = "menu_sniffer_rt";
static menu_sniffer_rt_screen_t s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_SELECT;
static menu_sniffer_rt_mode_t s_menu_sniffer_rt_mode = MENU_SNIFFER_RT_MODE_BLE;
static menu_sniffer_rt_launch_t s_menu_sniffer_rt_launch = MENU_SNIFFER_RT_LAUNCH_NONE;
static uint8_t s_menu_sniffer_rt_ieee802154_channel = POOM_IEEE802154_SNIFFER_CHANNEL_DEFAULT;

static void menu_sniffer_rt_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static esp_err_t menu_sniffer_rt_exit_(bool resume_menu);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_sniffer_rt_draw_header_(void)
{
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("SNNIFER RT"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] mode Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_sniffer_rt_mode_label_(menu_sniffer_rt_mode_t mode)
{
    switch (mode)
    {
        case MENU_SNIFFER_RT_MODE_BLE:
            return "BLE";
        case MENU_SNIFFER_RT_MODE_IEEE802154:
            return "802.15.4";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_sniffer_rt_draw_select_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_sniffer_rt_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    for (int i = 0; i < (int)MENU_SNIFFER_RT_MODE_COUNT; i++)
    {
        const int16_t y = (int16_t)(LIST_Y0 + i * ROW_STEP);

        poom_arduboy_set_cursor(TEXT_X, y);
        (void)poom_arduboy_print(menu_sniffer_rt_mode_label_((menu_sniffer_rt_mode_t)i));

        if (i == (int)s_menu_sniffer_rt_mode)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_sniffer_rt_draw_ieee802154_ch_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];

    (void)snprintf(
        line1,
        sizeof(line1),
        "802.15.4 CH:%u",
        (unsigned)s_menu_sniffer_rt_ieee802154_channel);
    (void)snprintf(line2, sizeof(line2), "Up/Down: ch");
    (void)snprintf(line3, sizeof(line3), "A:Start B:Back");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_sniffer_rt_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:START"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_sniffer_rt_draw_ieee802154_running_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];
    const uint32_t packet_count = poom_ieee802154_sniffer_get_packet_count();
    const int8_t rssi = poom_ieee802154_sniffer_get_recent_rssi();
    const uint8_t channel = poom_ieee802154_sniffer_get_channel();

    (void)snprintf(line1, sizeof(line1), "802.15.4 RUN");
    (void)snprintf(
        line2,
        sizeof(line2),
        "CH:%u PKT:%lu",
        (unsigned)channel,
        (unsigned long)packet_count);

    if (packet_count > 0U)
    {
        (void)snprintf(line3, sizeof(line3), "RSSI:%d dBm", (int)rssi);
    }
    else
    {
        (void)snprintf(line3, sizeof(line3), "Waiting frames");
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_sniffer_rt_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:STOP"));
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_sniffer_rt_draw_(void)
{
    if (s_menu_sniffer_rt_screen == MENU_SNIFFER_RT_SCREEN_IEEE802154_CH)
    {
        menu_sniffer_rt_draw_ieee802154_ch_();
        return;
    }

    if (s_menu_sniffer_rt_screen == MENU_SNIFFER_RT_SCREEN_IEEE802154_RUNNING)
    {
        menu_sniffer_rt_draw_ieee802154_running_();
        return;
    }

    menu_sniffer_rt_draw_select_();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @param[in] resume_menu Parameter passed to the helper.
 * @return esp_err_t
 */
static esp_err_t menu_sniffer_rt_exit_(bool resume_menu)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_sniffer_rt_active = false;
    s_menu_sniffer_rt_exit_requested = false;
    s_menu_sniffer_rt_launch = MENU_SNIFFER_RT_LAUNCH_NONE;

    if (poom_ieee802154_sniffer_is_active())
    {
        (void)poom_ieee802154_sniffer_stop();
    }

    if (s_menu_sniffer_rt_task != NULL)
    {
        if (s_menu_sniffer_rt_task != current_task)
        {
            TaskHandle_t status_task = s_menu_sniffer_rt_task;
            s_menu_sniffer_rt_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_sniffer_rt_task = NULL;
        }
    }

    if (s_menu_sniffer_rt_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_sniffer_rt_button_cb_, s_menu_sniffer_rt_sbus_user);
        s_menu_sniffer_rt_buttons_subscribed = false;
    }

    if (resume_menu)
    {
        const uint8_t token = 1;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }

    return ESP_OK;
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_sniffer_rt_task_(void *task_arg)
{
    (void)task_arg;

    while (s_menu_sniffer_rt_active)
    {
        if (s_menu_sniffer_rt_exit_requested)
        {
            (void)menu_sniffer_rt_exit_(true);
            break;
        }

        if (s_menu_sniffer_rt_launch == MENU_SNIFFER_RT_LAUNCH_BLE)
        {
            s_menu_sniffer_rt_launch = MENU_SNIFFER_RT_LAUNCH_NONE;
            (void)menu_sniffer_rt_exit_(false);
            app_ble_scan();
            break;
        }

        menu_sniffer_rt_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_SNIFFER_RT_REFRESH_MS));
    }

    s_menu_sniffer_rt_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_sniffer_rt_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    menu_sniffer_rt_button_msg_t button_msg;

    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if (button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (s_menu_sniffer_rt_screen == MENU_SNIFFER_RT_SCREEN_IEEE802154_RUNNING)
    {
        if (button_msg.button == BUTTON_B)
        {
            (void)poom_ieee802154_sniffer_stop();
            s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_IEEE802154_CH;
        }
        return;
    }

    if (s_menu_sniffer_rt_screen == MENU_SNIFFER_RT_SCREEN_IEEE802154_CH)
    {
        if (button_msg.button == BUTTON_B)
        {
            s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_SELECT;
            return;
        }

        if (button_msg.button == BUTTON_UP)
        {
            if (s_menu_sniffer_rt_ieee802154_channel >= POOM_IEEE802154_SNIFFER_CHANNEL_MAX)
            {
                s_menu_sniffer_rt_ieee802154_channel = POOM_IEEE802154_SNIFFER_CHANNEL_MIN;
            }
            else
            {
                s_menu_sniffer_rt_ieee802154_channel++;
            }
            return;
        }

        if (button_msg.button == BUTTON_DOWN)
        {
            if (s_menu_sniffer_rt_ieee802154_channel <= POOM_IEEE802154_SNIFFER_CHANNEL_MIN)
            {
                s_menu_sniffer_rt_ieee802154_channel = POOM_IEEE802154_SNIFFER_CHANNEL_MAX;
            }
            else
            {
                s_menu_sniffer_rt_ieee802154_channel--;
            }
            return;
        }

        if (button_msg.button == BUTTON_A)
        {
            if (poom_ieee802154_sniffer_start(s_menu_sniffer_rt_ieee802154_channel) == ESP_OK)
            {
                poom_ieee802154_sniffer_set_uart_forward_enabled(true);
                s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_IEEE802154_RUNNING;
            }
        }
        return;
    }

    if (button_msg.button == BUTTON_B)
    {
        if (s_menu_sniffer_rt_task == NULL)
        {
            (void)menu_sniffer_rt_exit_(true);
        }
        else
        {
            s_menu_sniffer_rt_exit_requested = true;
        }
        return;
    }

    if (button_msg.button == BUTTON_UP)
    {
        s_menu_sniffer_rt_mode = (s_menu_sniffer_rt_mode == MENU_SNIFFER_RT_MODE_BLE)
                                     ? MENU_SNIFFER_RT_MODE_IEEE802154
                                     : MENU_SNIFFER_RT_MODE_BLE;
        return;
    }

    if (button_msg.button == BUTTON_DOWN)
    {
        s_menu_sniffer_rt_mode = (s_menu_sniffer_rt_mode == MENU_SNIFFER_RT_MODE_BLE)
                                     ? MENU_SNIFFER_RT_MODE_IEEE802154
                                     : MENU_SNIFFER_RT_MODE_BLE;
        return;
    }

    if (button_msg.button == BUTTON_A)
    {
        if (s_menu_sniffer_rt_mode == MENU_SNIFFER_RT_MODE_BLE)
        {
            s_menu_sniffer_rt_launch = MENU_SNIFFER_RT_LAUNCH_BLE;
        }
        else
        {
            s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_IEEE802154_CH;
        }
    }
}

void menu_sniffer_rt_show(void)
{
    s_menu_sniffer_rt_active = true;
    s_menu_sniffer_rt_exit_requested = false;
    s_menu_sniffer_rt_screen = MENU_SNIFFER_RT_SCREEN_SELECT;
    s_menu_sniffer_rt_mode = MENU_SNIFFER_RT_MODE_BLE;
    s_menu_sniffer_rt_launch = MENU_SNIFFER_RT_LAUNCH_NONE;
    s_menu_sniffer_rt_ieee802154_channel = POOM_IEEE802154_SNIFFER_CHANNEL_DEFAULT;

    if (!s_menu_sniffer_rt_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_sniffer_rt_button_cb_, s_menu_sniffer_rt_sbus_user))
        {
            s_menu_sniffer_rt_buttons_subscribed = true;
        }
        else
        {
            s_menu_sniffer_rt_active = false;
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    menu_sniffer_rt_draw_();

    if (s_menu_sniffer_rt_task == NULL)
    {
        (void)xTaskCreate(
            menu_sniffer_rt_task_,
            "menu_sniffer_rt",
            MENU_SNIFFER_RT_STACK,
            NULL,
            MENU_SNIFFER_RT_PRIO,
            &s_menu_sniffer_rt_task);
    }
}
