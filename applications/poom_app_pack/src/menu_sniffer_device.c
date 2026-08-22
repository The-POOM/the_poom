// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_sniffer_device.h"

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
#include "poom_sniffer_device.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_SNIFFER_DEVICE_REFRESH_MS (250U)
#define MENU_SNIFFER_DEVICE_STACK (3072U)
#define MENU_SNIFFER_DEVICE_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)
#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (10)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

static bool s_menu_sniffer_device_active = false;
static bool s_menu_sniffer_device_running = false;
static bool s_menu_sniffer_device_buttons_subscribed = false;
static bool s_menu_sniffer_device_exit_requested = false;
static TaskHandle_t s_menu_sniffer_device_task = NULL;
static char s_menu_sniffer_device_sbus_user[] = "menu_sniffer_device";
static char s_menu_sniffer_device_status[22] = "Press A to run";

static esp_err_t menu_sniffer_device_exit_(void);
static void menu_sniffer_device_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_sniffer_device_draw_header_(void)
{
    poom_arduboy_set_cursor(22, 2);
    (void)poom_arduboy_print(F("SNIFFER DEVICE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws sniffer device menu status on display.
 * @param[in] running Current running state.
 * @param[in] status_text Status line text.
 * @return esp_err_t
 */
static esp_err_t menu_sniffer_device_draw_(bool running, const char *status_text)
{
    char line1[22];
    char line2[22];
    char line3[22];

    (void)snprintf(line1, sizeof(line1), "Mode: ProbeReq");
    (void)snprintf(line2, sizeof(line2), "Sniffer: %s", running ? "RUN" : "OFF");
    (void)snprintf(line3, sizeof(line3), "%.18s", (status_text != NULL) ? status_text : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_sniffer_device_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line1);

    const int16_t y_state = (int16_t)(ROW0_Y + ROW_STEP);
    poom_arduboy_set_cursor(TEXT_X, y_state);
    (void)poom_arduboy_print(line2);
    if(running)
    {
        poom_arduboy_fill_rect(1, (int16_t)(y_state - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
    }

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:TOGGLE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Toggles sniffer runtime.
 */
static void menu_sniffer_device_toggle_(void)
{
    esp_err_t status;

    if(s_menu_sniffer_device_running)
    {
        (void)poom_sniffer_device_stop();
        s_menu_sniffer_device_running = false;
        (void)snprintf(s_menu_sniffer_device_status, sizeof(s_menu_sniffer_device_status), "Paused");
        return;
    }

    status = poom_sniffer_device_start();
    if(status != ESP_OK)
    {
        s_menu_sniffer_device_running = false;
        (void)snprintf(s_menu_sniffer_device_status, sizeof(s_menu_sniffer_device_status), "Start failed");
        return;
    }

    s_menu_sniffer_device_running = true;
    s_menu_sniffer_device_status[0] = '\0';
}

/**
 * @brief Status task that refreshes display and handles deferred exit.
 */
static void menu_sniffer_device_task_(void *task_arg)
{
    (void)task_arg;

    while(s_menu_sniffer_device_active)
    {
        if(s_menu_sniffer_device_exit_requested)
        {
            (void)menu_sniffer_device_exit_();
            break;
        }

        if(poom_sniffer_device_get_running(&s_menu_sniffer_device_running) != ESP_OK)
        {
            s_menu_sniffer_device_running = false;
        }

        (void)menu_sniffer_device_draw_(s_menu_sniffer_device_running, s_menu_sniffer_device_status);
        vTaskDelay(pdMS_TO_TICKS(MENU_SNIFFER_DEVICE_REFRESH_MS));
    }

    s_menu_sniffer_device_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Stops runtime and returns to main menu.
 */
static esp_err_t menu_sniffer_device_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_sniffer_device_active = false;
    s_menu_sniffer_device_exit_requested = false;

    (void)poom_sniffer_device_stop();
    s_menu_sniffer_device_running = false;

    if(s_menu_sniffer_device_task != NULL)
    {
        if(s_menu_sniffer_device_task != current_task)
        {
            TaskHandle_t status_task = s_menu_sniffer_device_task;
            s_menu_sniffer_device_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_sniffer_device_task = NULL;
        }
    }

    if(s_menu_sniffer_device_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_sniffer_device_button_cb_, s_menu_sniffer_device_sbus_user);
        s_menu_sniffer_device_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Handles button events for sniffer device menu.
 */
static void menu_sniffer_device_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t button_msg;
    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if(button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(button_msg.button == BUTTON_B)
    {
        if(s_menu_sniffer_device_task == NULL)
        {
            (void)menu_sniffer_device_exit_();
        }
        else
        {
            s_menu_sniffer_device_exit_requested = true;
        }
        return;
    }

    if(button_msg.button == BUTTON_A)
    {
        menu_sniffer_device_toggle_();
        (void)menu_sniffer_device_draw_(s_menu_sniffer_device_running, s_menu_sniffer_device_status);
    }
}

void menu_sniffer_device_show(void)
{
    s_menu_sniffer_device_active = true;
    if(poom_sniffer_device_get_running(&s_menu_sniffer_device_running) != ESP_OK)
    {
        s_menu_sniffer_device_running = false;
    }

    s_menu_sniffer_device_exit_requested = false;
    if(s_menu_sniffer_device_running)
    {
        s_menu_sniffer_device_status[0] = '\0';
    }
    else
    {
        (void)snprintf(s_menu_sniffer_device_status, sizeof(s_menu_sniffer_device_status), "Press A to run");
    }

    if(!s_menu_sniffer_device_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_sniffer_device_button_cb_, s_menu_sniffer_device_sbus_user))
        {
            s_menu_sniffer_device_buttons_subscribed = true;
        }
        else
        {
            s_menu_sniffer_device_active = false;
            (void)menu_sniffer_device_draw_(false, "Button sub error");

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_sniffer_device_task == NULL)
    {
        (void)xTaskCreate(menu_sniffer_device_task_,
                          "menu_sniffer_dev",
                          MENU_SNIFFER_DEVICE_STACK,
                          NULL,
                          MENU_SNIFFER_DEVICE_PRIO,
                          &s_menu_sniffer_device_task);
    }

    (void)menu_sniffer_device_draw_(s_menu_sniffer_device_running, s_menu_sniffer_device_status);
}
