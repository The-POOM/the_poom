// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ssid_spam.h"

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
#include "poom_wifi_spam.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_SSID_SPAM_REFRESH_MS (250U)
#define MENU_SSID_SPAM_STATUS_STACK (3072U)
#define MENU_SSID_SPAM_STATUS_PRIO (4U)

#define HEADER_H (11)
#define CONTENT_Y0 (12)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (10)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

static bool s_menu_ssid_spam_active = false;
static bool s_menu_ssid_spam_running = false;
static bool s_menu_ssid_spam_buttons_subscribed = false;
static bool s_menu_ssid_spam_exit_requested = false;
static bool s_menu_ssid_spam_toggle_requested = false;
static bool s_menu_ssid_spam_input_dirty = false;
static TaskHandle_t s_menu_ssid_spam_status_task = NULL;
static char s_menu_ssid_spam_sbus_user[] = "menu_ssid_spam";
static char s_menu_ssid_spam_status[22] = "Press A to run";

static esp_err_t menu_ssid_spam_exit_(void);
static void menu_ssid_spam_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_ssid_spam_request_render_from_input_(void);

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_ssid_spam_request_render_from_input_(void)
{
    s_menu_ssid_spam_input_dirty = true;
    if (s_menu_ssid_spam_status_task != NULL)
    {
        (void)xTaskNotifyGive(s_menu_ssid_spam_status_task);
    }
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_ssid_spam_draw_header_(void)
{
    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("WIFI SPAM"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] running Parameter passed to the helper.
 * @param[in] status_text Parameter passed to the helper.
 * @return esp_err_t
 */
static esp_err_t menu_ssid_spam_draw_(bool running, const char* status_text)
{
    char line_mode[22];
    char line_spam[22];
    char line_status[22];

    (void)snprintf(line_mode, sizeof(line_mode), "Mode: AP beacon");
    (void)snprintf(line_spam, sizeof(line_spam), "State: %s", running ? "RUNNING" : "PAUSED");
    (void)snprintf(line_status, sizeof(line_status), "%.18s", (status_text != NULL) ? status_text : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ssid_spam_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_mode);

    const int16_t y_state = (int16_t)(ROW0_Y + ROW_STEP);
    poom_arduboy_set_cursor(TEXT_X, y_state);
    (void)poom_arduboy_print(line_spam);

    if (running)
    {
        poom_arduboy_fill_rect(1, (int16_t)(y_state - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
    }

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line_status);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:TOGGLE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Toggles the current runtime state.
 *
 * @return void
 */
static void menu_ssid_spam_toggle_(void)
{
    esp_err_t status;

    if(s_menu_ssid_spam_running)
    {
        (void)poom_wifi_spam_stop();
        s_menu_ssid_spam_running = false;
        (void)snprintf(s_menu_ssid_spam_status, sizeof(s_menu_ssid_spam_status), "Paused");
        return;
    }

    status = poom_wifi_spam_start();
    if(status == ESP_OK)
    {
        status = poom_wifi_spam_get_running(&s_menu_ssid_spam_running);
    }
    if(status != ESP_OK)
    {
        s_menu_ssid_spam_running = false;
    }

    (void)snprintf(s_menu_ssid_spam_status,
                   sizeof(s_menu_ssid_spam_status),
                   s_menu_ssid_spam_running ? "Running" : "Start failed");
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_ssid_spam_status_task_(void* task_arg)
{
    (void)task_arg;
    bool last_draw_running = s_menu_ssid_spam_running;
    char last_draw_status[sizeof(s_menu_ssid_spam_status)];
    (void)snprintf(last_draw_status, sizeof(last_draw_status), "%s", s_menu_ssid_spam_status);

    (void)menu_ssid_spam_draw_(s_menu_ssid_spam_running, s_menu_ssid_spam_status);
    s_menu_ssid_spam_input_dirty = false;

    while(s_menu_ssid_spam_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MENU_SSID_SPAM_REFRESH_MS));

        if(s_menu_ssid_spam_exit_requested)
        {
            (void)menu_ssid_spam_exit_();
            break;
        }

        if (s_menu_ssid_spam_toggle_requested)
        {
            s_menu_ssid_spam_toggle_requested = false;
            menu_ssid_spam_toggle_();
            s_menu_ssid_spam_input_dirty = true;
        }

        bool running_now = s_menu_ssid_spam_running;
        if(poom_wifi_spam_get_running(&s_menu_ssid_spam_running) != ESP_OK)
        {
            s_menu_ssid_spam_running = false;
        }

        if (running_now != s_menu_ssid_spam_running)
        {
            if (!s_menu_ssid_spam_running)
            {
                if (strcmp(s_menu_ssid_spam_status, "Running") == 0)
                {
                    (void)snprintf(s_menu_ssid_spam_status, sizeof(s_menu_ssid_spam_status), "Stopped");
                }
            }
            else
            {
                if ((strcmp(s_menu_ssid_spam_status, "Paused") == 0) || (strcmp(s_menu_ssid_spam_status, "Stopped") == 0))
                {
                    (void)snprintf(s_menu_ssid_spam_status, sizeof(s_menu_ssid_spam_status), "Running");
                }
            }
            s_menu_ssid_spam_input_dirty = true;
        }

        const bool status_changed = (strncmp(last_draw_status, s_menu_ssid_spam_status, sizeof(last_draw_status)) != 0);
        const bool state_changed = (last_draw_running != s_menu_ssid_spam_running);
        if (s_menu_ssid_spam_input_dirty || status_changed || state_changed)
        {
            (void)menu_ssid_spam_draw_(s_menu_ssid_spam_running, s_menu_ssid_spam_status);
            s_menu_ssid_spam_input_dirty = false;
            last_draw_running = s_menu_ssid_spam_running;
            (void)snprintf(last_draw_status, sizeof(last_draw_status), "%s", s_menu_ssid_spam_status);
        }
    }

    s_menu_ssid_spam_status_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_ssid_spam_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_ssid_spam_active = false;
    s_menu_ssid_spam_exit_requested = false;

    (void)poom_wifi_spam_stop();
    s_menu_ssid_spam_running = false;

    if(s_menu_ssid_spam_status_task != NULL)
    {
        if(s_menu_ssid_spam_status_task != current_task)
        {
            TaskHandle_t status_task = s_menu_ssid_spam_status_task;
            s_menu_ssid_spam_status_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_ssid_spam_status_task = NULL;
        }
    }

    if(s_menu_ssid_spam_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ssid_spam_button_cb_, s_menu_ssid_spam_sbus_user);
        s_menu_ssid_spam_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_ssid_spam_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
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
        if(s_menu_ssid_spam_status_task == NULL)
        {
            (void)menu_ssid_spam_exit_();
        }
        else
        {
            s_menu_ssid_spam_exit_requested = true;
            menu_ssid_spam_request_render_from_input_();
        }
        return;
    }

    if(button_msg.button == BUTTON_A)
    {
        if (s_menu_ssid_spam_status_task == NULL)
        {
            menu_ssid_spam_toggle_();
            (void)menu_ssid_spam_draw_(s_menu_ssid_spam_running, s_menu_ssid_spam_status);
        }
        else
        {
            s_menu_ssid_spam_toggle_requested = true;
            menu_ssid_spam_request_render_from_input_();
        }
    }
}

void menu_ssid_spam_init(void)
{
    s_menu_ssid_spam_active = true;
    if(poom_wifi_spam_get_running(&s_menu_ssid_spam_running) != ESP_OK)
    {
        s_menu_ssid_spam_running = false;
    }
    s_menu_ssid_spam_exit_requested = false;
    (void)snprintf(s_menu_ssid_spam_status,
                   sizeof(s_menu_ssid_spam_status),
                   s_menu_ssid_spam_running ? "Running" : "Press A to run");

    if(!s_menu_ssid_spam_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_ssid_spam_button_cb_, s_menu_ssid_spam_sbus_user))
        {
            s_menu_ssid_spam_buttons_subscribed = true;
        }
        else
        {
            s_menu_ssid_spam_active = false;
            (void)menu_ssid_spam_draw_(false, "Button sub error");

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_ssid_spam_status_task == NULL)
    {
        (void)xTaskCreate(menu_ssid_spam_status_task_,
                          "menu_wifi_spam",
                          MENU_SSID_SPAM_STATUS_STACK,
                          NULL,
                          MENU_SSID_SPAM_STATUS_PRIO,
                          &s_menu_ssid_spam_status_task);
    }

    menu_ssid_spam_request_render_from_input_();
}
