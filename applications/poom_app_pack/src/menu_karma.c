// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_karma.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "poom_sbus.h"
#include "poom_wifi_karma.h"
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_KARMA_OLED_COL (8)
#define MENU_KARMA_BOX_Y (10)
#define MENU_KARMA_BOX_H (50)
#define MENU_KARMA_REFRESH_MS (250U)
#define MENU_KARMA_STACK (3072U)
#define MENU_KARMA_PRIO (4U)
#define MENU_KARMA_TEXT_MAX_CHARS (11U)
#define MENU_KARMA_STATUS_MAX_CHARS (13U)

#ifndef BTN_A
#define BTN_A (0U)
#endif

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
} menu_karma_button_msg_t;

static bool s_menu_karma_active = false;
static bool s_menu_karma_buttons_subscribed = false;
static bool s_menu_karma_exit_requested = false;
static bool s_menu_karma_restart_requested = false;
static TaskHandle_t s_menu_karma_task = NULL;
static char s_menu_karma_sbus_user[] = "menu_karma";
static char s_menu_karma_status[22] = "Starting...";
static char s_menu_karma_active_ssid[33] = {0};

static esp_err_t menu_karma_exit_(void);
static void menu_karma_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_karma_sync_runtime_status_(void);

/**
 * @brief Formats SSID text to fit in OLED frame width.
 * @param[out] out_text Output line buffer.
 * @param[in] out_len Output line buffer length.
 * @param[in] ssid Source SSID text.
 * @return void
 */
static void menu_karma_format_ssid_line_(char *out_text, size_t out_len, const char *ssid)
{
    if((out_text == NULL) || (out_len == 0U))
    {
        return;
    }

    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        (void)snprintf(out_text, out_len, "--");
        return;
    }

    (void)snprintf(out_text, out_len, "%.*s", (int)MENU_KARMA_TEXT_MAX_CHARS, ssid);
}

/**
 * @brief Converts verbose runtime text into a short OLED-friendly status.
 * @param[out] out_text Output line buffer.
 * @param[in] out_len Output line buffer length.
 * @param[in] status Source status text.
 * @return void
 */
static void menu_karma_format_status_line_(char *out_text, size_t out_len, const char *status)
{
    if((out_text == NULL) || (out_len == 0U))
    {
        return;
    }

    if((status == NULL) || (status[0] == '\0'))
    {
        (void)snprintf(out_text, out_len, "--");
        return;
    }

    if(strstr(status, "Scanning") != NULL)
    {
        (void)snprintf(out_text, out_len, "SCAN");
        return;
    }

    if(strstr(status, "Cloning") != NULL)
    {
        (void)snprintf(out_text, out_len, "CLONING");
        return;
    }

    if(strstr(status, "Captive") != NULL)
    {
        (void)snprintf(out_text, out_len, "CAPTIVE");
        return;
    }

    if(strstr(status, "Restart") != NULL)
    {
        (void)snprintf(out_text, out_len, "RESTART");
        return;
    }

    if(strstr(status, "Start failed") != NULL)
    {
        (void)snprintf(out_text, out_len, "FAIL");
        return;
    }

    if(strstr(status, "Button sub error") != NULL)
    {
        (void)snprintf(out_text, out_len, "BTN ERR");
        return;
    }

    (void)snprintf(out_text, out_len, "%.*s", (int)MENU_KARMA_STATUS_MAX_CHARS, status);
}

/**
 * @brief Starts karma runtime and updates status string.
 * @return esp_err_t
 */
static esp_err_t menu_karma_runtime_start_(void)
{
    esp_err_t status = poom_wifi_karma_start();

    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Start failed");
        return status;
    }

    (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Scanning probes");
    return ESP_OK;
}

/**
 * @brief Refreshes UI status text from current Karma runtime state.
 * @return void
 */
static void menu_karma_sync_runtime_status_(void)
{
    if((strcmp(s_menu_karma_status, "Start failed") == 0) ||
       (strcmp(s_menu_karma_status, "Button sub error") == 0) ||
       (strcmp(s_menu_karma_status, "Restarting...") == 0))
    {
        return;
    }

    switch(poom_wifi_karma_get_state())
    {
        case POOM_WIFI_KARMA_STATE_CAPTIVE:
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Captive clone");
            break;

        case POOM_WIFI_KARMA_STATE_CLONING:
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Cloning WiFi");
            break;

        case POOM_WIFI_KARMA_STATE_SCANNING:
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Scanning probes");
            break;

        case POOM_WIFI_KARMA_STATE_STOPPED:
        default:
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Stopped");
            break;
    }
}

/**
 * @brief Draws current karma status and active SSID on OLED.
 * @return esp_err_t
 */
static esp_err_t menu_karma_draw_(void)
{
    char line1[22];
    char line2[22];
    char ssid_text[MENU_KARMA_TEXT_MAX_CHARS + 1U];
    char status_text[MENU_KARMA_STATUS_MAX_CHARS + 1U];

    if(!poom_wifi_karma_get_active_ssid(s_menu_karma_active_ssid, sizeof(s_menu_karma_active_ssid)))
    {
        s_menu_karma_active_ssid[0] = '\0';
    }

    menu_karma_sync_runtime_status_();
    menu_karma_format_ssid_line_(ssid_text, sizeof(ssid_text), s_menu_karma_active_ssid);
    menu_karma_format_status_line_(status_text, sizeof(status_text), s_menu_karma_status);

    (void)snprintf(line1, sizeof(line1), "SSID: %s", ssid_text);
    (void)snprintf(line2, sizeof(line2), "STATUS: %s", status_text);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("POOM KARMA"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, 30);
    (void)poom_arduboy_print(line2);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SCAN"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Task that handles deferred restart/exit and refreshes OLED status.
 * @param[in] task_arg Not used.
 * @return void
 */
static void menu_karma_task_(void *task_arg)
{
    (void)task_arg;

    while(s_menu_karma_active)
    {
        if(s_menu_karma_exit_requested)
        {
            (void)menu_karma_exit_();
            break;
        }

        if(s_menu_karma_restart_requested)
        {
            s_menu_karma_restart_requested = false;
            (void)poom_wifi_karma_stop();
            s_menu_karma_active_ssid[0] = '\0';
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Restarting...");
            (void)menu_karma_runtime_start_();
        }

        (void)menu_karma_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_KARMA_REFRESH_MS));
    }

    s_menu_karma_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Stops karma menu runtime and returns to submenu.
 * @return esp_err_t
 */
static esp_err_t menu_karma_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_karma_active = false;
    s_menu_karma_exit_requested = false;
    s_menu_karma_restart_requested = false;
    s_menu_karma_active_ssid[0] = '\0';

    (void)poom_wifi_karma_stop();

    if(s_menu_karma_task != NULL)
    {
        if(s_menu_karma_task != current_task)
        {
            TaskHandle_t task = s_menu_karma_task;
            s_menu_karma_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_karma_task = NULL;
        }
    }

    if(s_menu_karma_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_karma_button_cb_, s_menu_karma_sbus_user);
        s_menu_karma_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Handles A/B single-click events for karma menu.
 * @param[in] msg SBUS message.
 * @param[in] user_ctx Not used.
 * @return void
 */
static void menu_karma_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    menu_karma_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if(button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(button_msg.button == BTN_B)
    {
        if(s_menu_karma_task == NULL)
        {
            (void)menu_karma_exit_();
        }
        else
        {
            s_menu_karma_exit_requested = true;
        }
        return;
    }

    if(button_msg.button == BTN_A)
    {
        s_menu_karma_restart_requested = true;
    }
}

void menu_karma_init(void)
{
    s_menu_karma_active = true;
    s_menu_karma_exit_requested = false;
    s_menu_karma_restart_requested = false;
    s_menu_karma_active_ssid[0] = '\0';

    if(!s_menu_karma_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_karma_button_cb_, s_menu_karma_sbus_user))
        {
            s_menu_karma_buttons_subscribed = true;
        }
        else
        {
            s_menu_karma_active = false;
            (void)snprintf(s_menu_karma_status, sizeof(s_menu_karma_status), "Button sub error");
            (void)menu_karma_draw_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(menu_karma_runtime_start_() != ESP_OK)
    {
        (void)menu_karma_draw_();
    }

    if(s_menu_karma_task == NULL)
    {
        (void)xTaskCreate(menu_karma_task_,
                          "menu_karma",
                          MENU_KARMA_STACK,
                          NULL,
                          MENU_KARMA_PRIO,
                          &s_menu_karma_task);
    }
}
