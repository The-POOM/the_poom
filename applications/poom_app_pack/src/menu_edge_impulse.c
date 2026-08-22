// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_edge_impulse.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "poom_edge_impulse.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_EDGE_IMPULSE_OLED_COL (6)
#define MENU_EDGE_IMPULSE_BOX_Y (10)
#define MENU_EDGE_IMPULSE_BOX_H (52)
#define MENU_EDGE_IMPULSE_REFRESH_MS (250U)
#define MENU_EDGE_IMPULSE_STATUS_STACK (3072U)
#define MENU_EDGE_IMPULSE_STATUS_PRIO (4U)
#define MENU_EDGE_IMPULSE_RESULT_HOLD_CYCLES (6U)

#ifndef BTN_B
#define BTN_B (1U)
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
} menu_edge_impulse_button_msg_t;

static bool s_menu_edge_impulse_active = false;
static bool s_menu_edge_impulse_buttons_subscribed = false;
static TaskHandle_t s_menu_edge_impulse_status_task = NULL;
static char s_menu_edge_impulse_sbus_user[] = "menu_edge_impulse";
static bool s_menu_edge_impulse_exit_requested = false;
static bool s_menu_edge_impulse_capture_ongoing = false;
static bool s_menu_edge_impulse_capture_request_pending = false;
static char s_menu_edge_impulse_capture_request_label[POOM_EDGE_IMPULSE_LABEL_MAX_LEN + 1U] = {0};
static bool s_menu_edge_impulse_capture_result_pending = false;
static esp_err_t s_menu_edge_impulse_capture_result = ESP_OK;

static void menu_edge_impulse_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_edge_impulse_capture_done_cb_(esp_err_t result, const char* label, void* user_ctx);
static esp_err_t menu_edge_impulse_exit_(void);

/**
 * @brief Maps capture start error code to short OLED status text.
 *
 * @param[in] status Capture start result.
 * @return const char*
 */
static const char* menu_edge_impulse_status_from_error_(esp_err_t status)
{
    if(status == ESP_ERR_INVALID_STATE)
    {
        return "Wait WiFi/Idle";
    }

    if(status == ESP_ERR_INVALID_ARG)
    {
        return "Invalid label";
    }

    return "Capture start fail";
}

/**
 * @brief Maps input button id to capture label string.
 *
 * @param[in] button_id Input button identifier.
 * @param[out] out_label Output label pointer for trigger API.
 * @return esp_err_t
 */
static esp_err_t menu_edge_impulse_label_from_button_(uint8_t button_id, const char** out_label)
{
    if(out_label == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    switch(button_id)
    {
        case BTN_UP:
            *out_label = "button_up";
            return ESP_OK;
        case BTN_DOWN:
            *out_label = "button_down";
            return ESP_OK;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

/**
 * @brief Draws current Edge Impulse state in OLED.
 *
 * @param[in] initialized True when module is initialized.
 * @param[in] wifi_connected True when STA has IP.
 * @param[in] capture_running True when upload is in progress.
 * @param[in] status_text Status line text.
 * @return esp_err_t
 */
static esp_err_t menu_edge_impulse_draw_(bool initialized,
                                         bool wifi_connected,
                                         bool capture_running,
                                         const char* status_text)
{
    char line_init[22];
    char line_wifi[22];
    char line_send[22];
    char line_status[22];

    (void)snprintf(line_init, sizeof(line_init), "Init:%s", initialized ? "OK" : "ERR");
    (void)snprintf(line_wifi, sizeof(line_wifi), "WiFi:%s", wifi_connected ? "OK" : "WAIT");
    (void)snprintf(line_send, sizeof(line_send), "Upload:%s", capture_running ? "SENDING" : "IDLE");
    (void)snprintf(line_status, sizeof(line_status), "%.21s", (status_text != NULL) ? status_text : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(18, 2);
    (void)poom_arduboy_print(F("EDGE IMPULSE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(2, 14);
    (void)poom_arduboy_print(line_init);
    poom_arduboy_set_cursor(64, 14);
    (void)poom_arduboy_print(line_wifi);

    const int16_t y_upload = 28;
    poom_arduboy_set_cursor(2, y_upload);
    (void)poom_arduboy_print(line_send);
    if(capture_running)
    {
        poom_arduboy_fill_rect(0, (int16_t)(y_upload - 1), ARDUBOY_WIDTH, 9, INVERT);
    }

    poom_arduboy_set_cursor(2, 42);
    (void)poom_arduboy_print(line_status);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Background task that refreshes OLED status periodically.
 *
 * @param[in] task_arg Unused task argument.
 * @return void
 */
static void menu_edge_impulse_status_task_(void* task_arg)
{
    bool initialized = false;
    bool wifi_connected = false;
    bool capture_running = false;
    uint32_t result_hold_cycles = 0U;
    const char* transient_status = NULL;
    uint32_t transient_hold_cycles = 0U;
    esp_err_t trigger_status;

    (void)task_arg;

    while(s_menu_edge_impulse_active)
    {
        if(s_menu_edge_impulse_exit_requested)
        {
            (void)menu_edge_impulse_exit_();
            break;
        }

        if(poom_edge_impulse_get_state(&initialized, &wifi_connected, &capture_running) == ESP_OK)
        {
            if(s_menu_edge_impulse_capture_request_pending)
            {
                (void)menu_edge_impulse_draw_(initialized, wifi_connected, true, "Capturing...");
                trigger_status = poom_edge_impulse_trigger_capture(s_menu_edge_impulse_capture_request_label);
                s_menu_edge_impulse_capture_request_pending = false;

                if(trigger_status == ESP_OK)
                {
                    s_menu_edge_impulse_capture_ongoing = true;
                    s_menu_edge_impulse_capture_result_pending = false;
                    vTaskDelay(pdMS_TO_TICKS(MENU_EDGE_IMPULSE_REFRESH_MS));
                    continue;
                }

                s_menu_edge_impulse_capture_ongoing = false;
                transient_status = menu_edge_impulse_status_from_error_(trigger_status);
                transient_hold_cycles = MENU_EDGE_IMPULSE_RESULT_HOLD_CYCLES;
            }

            if(s_menu_edge_impulse_capture_ongoing)
            {
                if(!capture_running)
                {
                    s_menu_edge_impulse_capture_ongoing = false;
                    result_hold_cycles = MENU_EDGE_IMPULSE_RESULT_HOLD_CYCLES;
                }
                else
                {
                    vTaskDelay(pdMS_TO_TICKS(MENU_EDGE_IMPULSE_REFRESH_MS));
                    continue;
                }
            }

            if(s_menu_edge_impulse_capture_result_pending && (result_hold_cycles > 0U))
            {
                const char* result_text = (s_menu_edge_impulse_capture_result == ESP_OK) ? "Upload OK" : "Upload FAIL";
                (void)menu_edge_impulse_draw_(initialized, wifi_connected, capture_running, result_text);
                result_hold_cycles--;
                if(result_hold_cycles == 0U)
                {
                    s_menu_edge_impulse_capture_result_pending = false;
                }
            }
            else if((transient_status != NULL) && (transient_hold_cycles > 0U))
            {
                (void)menu_edge_impulse_draw_(initialized, wifi_connected, capture_running, transient_status);
                transient_hold_cycles--;
                if(transient_hold_cycles == 0U)
                {
                    transient_status = NULL;
                }
            }
            else
            {
                (void)menu_edge_impulse_draw_(initialized, wifi_connected, capture_running, "UP/DOWN: send");
            }
        }
        else
        {
            (void)menu_edge_impulse_draw_(false, false, false, "State read error");
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_EDGE_IMPULSE_REFRESH_MS));
    }

    s_menu_edge_impulse_status_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Receives capture/upload completion event from Edge Impulse module.
 *
 * @param[in] result Capture/upload result code.
 * @param[in] label Capture label.
 * @param[in] user_ctx User context pointer.
 * @return void
 */
static void menu_edge_impulse_capture_done_cb_(esp_err_t result, const char* label, void* user_ctx)
{
    (void)label;
    (void)user_ctx;
    s_menu_edge_impulse_capture_result = result;
    s_menu_edge_impulse_capture_result_pending = true;
}

/**
 * @brief Stops menu runtime and returns control to submenu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_edge_impulse_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_edge_impulse_active = false;
    s_menu_edge_impulse_exit_requested = false;
    s_menu_edge_impulse_capture_ongoing = false;
    s_menu_edge_impulse_capture_request_pending = false;
    s_menu_edge_impulse_capture_result_pending = false;

    if(s_menu_edge_impulse_status_task != NULL)
    {
        if(s_menu_edge_impulse_status_task != current_task)
        {
            TaskHandle_t status_task = s_menu_edge_impulse_status_task;
            s_menu_edge_impulse_status_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_edge_impulse_status_task = NULL;
        }
    }

    if(s_menu_edge_impulse_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_edge_impulse_button_cb_, s_menu_edge_impulse_sbus_user);
        s_menu_edge_impulse_buttons_subscribed = false;
    }

    (void)poom_edge_impulse_set_capture_done_cb(NULL, NULL);
    (void)poom_edge_impulse_stop();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);

    return ESP_OK;
}

/**
 * @brief Handles button events for Edge Impulse menu.
 *
 * @param[in] msg SBUS message pointer.
 * @param[in] user_ctx User context pointer.
 * @return void
 */
static void menu_edge_impulse_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_edge_impulse_button_msg_t button_msg;
    const char* capture_label;

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
        if(s_menu_edge_impulse_status_task == NULL)
        {
            (void)menu_edge_impulse_exit_();
        }
        else
        {
            s_menu_edge_impulse_exit_requested = true;
        }
        return;
    }

    if(menu_edge_impulse_label_from_button_(button_msg.button, &capture_label) != ESP_OK)
    {
        return;
    }

    if(s_menu_edge_impulse_capture_ongoing || s_menu_edge_impulse_capture_request_pending)
    {
        return;
    }

    (void)snprintf(s_menu_edge_impulse_capture_request_label,
                   sizeof(s_menu_edge_impulse_capture_request_label),
                   "%s",
                   capture_label);
    s_menu_edge_impulse_capture_request_pending = true;
    s_menu_edge_impulse_capture_result_pending = false;
    return;
}

void menu_edge_impulse_show(void)
{
    poom_edge_impulse_config_t config;
    esp_err_t status;

    (void)poom_edge_impulse_stop();
    s_menu_edge_impulse_active = true;
    s_menu_edge_impulse_exit_requested = false;
    s_menu_edge_impulse_capture_ongoing = false;
    s_menu_edge_impulse_capture_request_pending = false;
    s_menu_edge_impulse_capture_result_pending = false;
    s_menu_edge_impulse_capture_result = ESP_OK;

    if(!s_menu_edge_impulse_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_edge_impulse_button_cb_, s_menu_edge_impulse_sbus_user))
        {
            s_menu_edge_impulse_buttons_subscribed = true;
        }
        else
        {
            s_menu_edge_impulse_active = false;
            (void)menu_edge_impulse_draw_(false, false, false, "Button sub error");

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    status = poom_edge_impulse_get_default_config(&config);
    if(status != ESP_OK)
    {
        (void)menu_edge_impulse_draw_(false, false, false, "Config error");
        return;
    }

    (void)menu_edge_impulse_draw_(false, false, false, "Starting...");

    status = poom_edge_impulse_start(&config);
    if(status != ESP_OK)
    {
        (void)menu_edge_impulse_draw_(false, false, false, "Start failed");
        return;
    }

    (void)poom_edge_impulse_set_capture_done_cb(menu_edge_impulse_capture_done_cb_, NULL);

    if(s_menu_edge_impulse_status_task == NULL)
    {
        (void)xTaskCreate(menu_edge_impulse_status_task_,
                          "menu_ei_status",
                          MENU_EDGE_IMPULSE_STATUS_STACK,
                          NULL,
                          MENU_EDGE_IMPULSE_STATUS_PRIO,
                          &s_menu_edge_impulse_status_task);
    }
}
