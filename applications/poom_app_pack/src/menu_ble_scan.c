// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ble_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_ble_scan.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_BLE_SCAN_REFRESH_MS (250U)
#define MENU_BLE_SCAN_STACK (3072U)
#define MENU_BLE_SCAN_PRIO (4U)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_ble_scan_button_msg_t;

typedef struct
{
    uint32_t adv_total;
    int rssi;
    uint8_t mac[6];
    uint8_t addr_type;
    bool has_packet;
} menu_ble_scan_stats_t;

typedef enum
{
    MENU_BLE_SCAN_SCREEN_SELECT_MODE = 0,
    MENU_BLE_SCAN_SCREEN_RUNNING,
} menu_ble_scan_screen_t;

static bool s_menu_ble_scan_active = false;
static bool s_menu_ble_scan_buttons_subscribed = false;
static bool s_menu_ble_scan_exit_requested = false;
static TaskHandle_t s_menu_ble_scan_task = NULL;
static char s_menu_ble_scan_sbus_user[] = "menu_ble_scan";
static char s_menu_ble_scan_status[22] = "A:Stop B:Exit";
static menu_ble_scan_stats_t s_menu_ble_scan_stats;
static esp_ble_scan_type_t s_menu_ble_scan_mode = BLE_SCAN_TYPE_ACTIVE;
static menu_ble_scan_screen_t s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_SELECT_MODE;

static esp_err_t menu_ble_scan_exit_(void);
static esp_err_t menu_ble_scan_start_runtime_(void);
static void menu_ble_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Returns the display label for the current state.
 *
 * @return const char *
 */
static const char *menu_ble_scan_mode_label_(void)
{
    return (s_menu_ble_scan_mode == BLE_SCAN_TYPE_PASSIVE) ? "PASSIVE" : "ACTIVE";
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_ble_scan_draw_header_(void)
{
    poom_arduboy_set_cursor(40, 2);
    (void)poom_arduboy_print(F("BLE SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_ble_scan_draw_select_mode_(void)
{
    char line_mode[22];

    (void)snprintf(line_mode, sizeof(line_mode), "Mode: %s", menu_ble_scan_mode_label_());

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_ble_scan_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(F("Select scan mode"));
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line_mode);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(F("Up/Down: change"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:START"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Receives BLE advertising reports and updates runtime stats.
 * @param[in] scan_result BLE GAP scan result payload.
 * @return void
 */
static void menu_ble_scan_result_cb_(const esp_ble_gap_cb_param_t *scan_result)
{
    if((scan_result == NULL) || !s_menu_ble_scan_active)
    {
        return;
    }

    s_menu_ble_scan_stats.adv_total++;
    s_menu_ble_scan_stats.rssi = scan_result->scan_rst.rssi;
    s_menu_ble_scan_stats.addr_type = scan_result->scan_rst.ble_addr_type;
    (void)memcpy(s_menu_ble_scan_stats.mac, scan_result->scan_rst.bda, sizeof(s_menu_ble_scan_stats.mac));
    s_menu_ble_scan_stats.has_packet = true;
}

/**
 * @brief Draws BLE scan dashboard on OLED.
 * @return esp_err_t
 */
static esp_err_t menu_ble_scan_draw_(void)
{
    char line_state[22];
    char line_info[22];
    char line_hint[22];

    if(s_menu_ble_scan_screen == MENU_BLE_SCAN_SCREEN_SELECT_MODE)
    {
        menu_ble_scan_draw_select_mode_();
        return ESP_OK;
    }

    (void)snprintf(line_state,
                   sizeof(line_state),
                   "State: %s",
                   poom_ble_scan_is_active() ? "RUNNING" : "STOPPED");

    (void)snprintf(line_info,
                   sizeof(line_info),
                   "%s scan active",
                   poom_ble_scan_is_active() ? "BLE" : "BLE");
    (void)snprintf(line_hint,
                   sizeof(line_hint),
                   "Streaming to host");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ble_scan_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_state);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line_info);
    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line_hint);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:STOP"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Starts/stops BLE scanner runtime from button action.
 * @return void
 */
static void menu_ble_scan_toggle_(void)
{
    esp_err_t ret;

    if(poom_ble_scan_is_active())
    {
        ret = poom_ble_scan_stop();
        if(ret == ESP_OK)
        {
            s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_SELECT_MODE;
            (void)snprintf(s_menu_ble_scan_status, sizeof(s_menu_ble_scan_status), "A:Start B:Exit");
        }
        else
        {
            (void)snprintf(s_menu_ble_scan_status, sizeof(s_menu_ble_scan_status), "Stop err");
        }
        return;
    }

    ret = menu_ble_scan_start_runtime_();
    if(ret != ESP_OK)
    {
        (void)snprintf(s_menu_ble_scan_status, sizeof(s_menu_ble_scan_status), "Start err");
        s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_SELECT_MODE;
        return;
    }

    s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_RUNNING;
}

/**
 * @brief Starts BLE scan runtime and host forwarding.
 * @return esp_err_t
 */
static esp_err_t menu_ble_scan_start_runtime_(void)
{
    esp_err_t ret;

    (void)memset(&s_menu_ble_scan_stats, 0, sizeof(s_menu_ble_scan_stats));
    poom_ble_scan_set_uart_forward_enabled(true);
    poom_ble_scan_set_scan_type(s_menu_ble_scan_mode);
    poom_ble_scan_set_filter_type(BLE_SCAN_FILTER_ALLOW_ALL);
    poom_ble_scan_register_cb(menu_ble_scan_result_cb_);

    ret = poom_ble_scan_start();
    if(ret != ESP_OK)
    {
        poom_ble_scan_register_cb(NULL);
        poom_ble_scan_set_uart_forward_enabled(false);
        return ret;
    }

    (void)snprintf(s_menu_ble_scan_status, sizeof(s_menu_ble_scan_status), "A:Stop B:Exit");
    return ESP_OK;
}

/**
 * @brief BLE scan menu task loop.
 * @param[in,out] task_arg Unused task argument.
 * @return void
 */
static void menu_ble_scan_task_(void *task_arg)
{
    (void)task_arg;

    while(s_menu_ble_scan_active)
    {
        if(s_menu_ble_scan_exit_requested)
        {
            (void)menu_ble_scan_exit_();
            break;
        }

        (void)menu_ble_scan_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_BLE_SCAN_REFRESH_MS));
    }

    s_menu_ble_scan_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Exits BLE scan menu and restores submenu context.
 * @return esp_err_t
 */
static esp_err_t menu_ble_scan_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_ble_scan_active = false;
    s_menu_ble_scan_exit_requested = false;
    s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_SELECT_MODE;

    if(s_menu_ble_scan_task != NULL)
    {
        if(s_menu_ble_scan_task != current_task)
        {
            TaskHandle_t status_task = s_menu_ble_scan_task;
            s_menu_ble_scan_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_ble_scan_task = NULL;
        }
    }

    if(s_menu_ble_scan_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ble_scan_button_cb_, s_menu_ble_scan_sbus_user);
        s_menu_ble_scan_buttons_subscribed = false;
    }

    poom_ble_scan_register_cb(NULL);
    (void)poom_ble_scan_stop();
    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);

    return ESP_OK;
}

/**
 * @brief Handles BLE scan menu button events.
 * @param[in] msg SBUS payload.
 * @param[in] user_ctx User context (unused).
 * @return void
 */
static void menu_ble_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    menu_ble_scan_button_msg_t button_msg;

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

    if(button_msg.button == BUTTON_B)
    {
        if(s_menu_ble_scan_task == NULL)
        {
            (void)menu_ble_scan_exit_();
        }
        else
        {
            s_menu_ble_scan_exit_requested = true;
        }
        return;
    }

    if(button_msg.button == BUTTON_A)
    {
        menu_ble_scan_toggle_();
        return;
    }

    if((button_msg.button == BUTTON_UP) || (button_msg.button == BUTTON_DOWN))
    {
        if(s_menu_ble_scan_screen == MENU_BLE_SCAN_SCREEN_SELECT_MODE)
        {
            s_menu_ble_scan_mode = (s_menu_ble_scan_mode == BLE_SCAN_TYPE_ACTIVE)
                                       ? BLE_SCAN_TYPE_PASSIVE
                                       : BLE_SCAN_TYPE_ACTIVE;
        }
    }
}

/**
 * @brief Opens BLE scan monitor menu.
 * @return void
 */
void app_ble_scan(void)
{
    s_menu_ble_scan_active = true;
    s_menu_ble_scan_exit_requested = false;
    s_menu_ble_scan_mode = BLE_SCAN_TYPE_ACTIVE;
    s_menu_ble_scan_screen = MENU_BLE_SCAN_SCREEN_SELECT_MODE;
    (void)memset(&s_menu_ble_scan_stats, 0, sizeof(s_menu_ble_scan_stats));
    (void)snprintf(s_menu_ble_scan_status, sizeof(s_menu_ble_scan_status), "A:Start B:Exit");

    if(!s_menu_ble_scan_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_ble_scan_button_cb_, s_menu_ble_scan_sbus_user))
        {
            s_menu_ble_scan_buttons_subscribed = true;
        }
        else
        {
            s_menu_ble_scan_active = false;
            (void)menu_ble_scan_draw_();
            (void)poom_ble_scan_register_cb(NULL);
            (void)poom_ble_scan_stop();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)menu_ble_scan_draw_();

    if(s_menu_ble_scan_task == NULL)
    {
        (void)xTaskCreate(menu_ble_scan_task_,
                          "menu_ble_scan",
                          MENU_BLE_SCAN_STACK,
                          NULL,
                          MENU_BLE_SCAN_PRIO,
                          &s_menu_ble_scan_task);
    }
}
