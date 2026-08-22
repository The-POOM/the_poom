// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_dfu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "dfu.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_dfu_log.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_DFU_REFRESH_MS (180U)
#define MENU_DFU_STACK (3072U)
#define MENU_DFU_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (42)
#define TEXT_X (3)

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_DFU_STATE_STARTING = 0,
    MENU_DFU_STATE_READY,
    MENU_DFU_STATE_UPLOADING,
    MENU_DFU_STATE_SUCCESS,
    MENU_DFU_STATE_ERROR,
} menu_dfu_state_t;

#if CONFIG_POOM_DFU_ENABLE_LOG
static const char* POOM_DFU_MENU_TAG = "menu_dfu";
#endif
static bool s_menu_dfu_active = false;
static bool s_menu_dfu_buttons_subscribed = false;
static bool s_menu_dfu_backend_started = false;
static volatile bool s_menu_dfu_exit_requested = false;
static volatile bool s_menu_dfu_input_dirty = false;
static TaskHandle_t s_menu_dfu_task = NULL;
static char s_menu_dfu_sbus_user[] = "menu_dfu";
static menu_dfu_state_t s_menu_dfu_state = MENU_DFU_STATE_STARTING;
static uint8_t s_menu_dfu_progress = 0U;
static char s_menu_dfu_status[18] = "Starting";
static char s_menu_dfu_detail[22] = "Please wait";

static void menu_dfu_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_dfu_task_(void* arg);
static void menu_dfu_exit_(void);

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_dfu_request_render_(void)
{
    s_menu_dfu_input_dirty = true;
    if(s_menu_dfu_task != NULL) {
        (void)xTaskNotifyGive(s_menu_dfu_task);
    }
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return bool
 */
static bool menu_dfu_exit_locked_(void)
{
    return (s_menu_dfu_state == MENU_DFU_STATE_UPLOADING) ||
           (s_menu_dfu_state == MENU_DFU_STATE_SUCCESS);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_dfu_draw_(void)
{
    char line0[22] = {0};
    char line1[22] = {0};
    char line2[22] = {0};
    char line3[22] = {0};
    char line4[22] = {0};
    const char* ssid = poom_fw_update_get_wifi_ap_ssid();
    const char* pass = poom_fw_update_get_wifi_ap_password();
    const char* ip = poom_fw_update_get_wifi_ap_ip();

    if((ssid == NULL) || (ssid[0] == '\0')) {
        ssid = "N/A";
    }
    if((pass == NULL) || (pass[0] == '\0')) {
        pass = "-";
    }
    if((ip == NULL) || (ip[0] == '\0')) {
        ip = "-";
    }

    (void)snprintf(line0, sizeof(line0), "State: %.14s", s_menu_dfu_status);

    switch(s_menu_dfu_state)
    {
        case MENU_DFU_STATE_STARTING:
        case MENU_DFU_STATE_READY:
            (void)snprintf(line1, sizeof(line1), "SSID: %.15s", ssid);
            (void)snprintf(line2, sizeof(line2), "PASS: %.15s", pass);
            (void)snprintf(line3, sizeof(line3), "URL: poom.local");
            (void)snprintf(line4, sizeof(line4), "IP: %.15s", ip);
            break;
        case MENU_DFU_STATE_UPLOADING:
            (void)snprintf(line1, sizeof(line1), "Prog: %3u%%", (unsigned)s_menu_dfu_progress);
            (void)snprintf(line2, sizeof(line2), "URL: poom.local");
            (void)snprintf(line3, sizeof(line3), "IP: %.15s", ip);
            (void)snprintf(line4, sizeof(line4), "Wait upload");
            break;
        case MENU_DFU_STATE_SUCCESS:
            (void)snprintf(line1, sizeof(line1), "Rebooting...");
            (void)snprintf(line2, sizeof(line2), "URL: poom.local");
            (void)snprintf(line3, sizeof(line3), "IP: %.15s", ip);
            break;
        case MENU_DFU_STATE_ERROR:
        default:
            (void)snprintf(line1, sizeof(line1), "%.21s", s_menu_dfu_detail);
            if(s_menu_dfu_backend_started) {
                (void)snprintf(line2, sizeof(line2), "URL: poom.local");
                (void)snprintf(line3, sizeof(line3), "IP: %.15s", ip);
                (void)snprintf(line4, sizeof(line4), "Retry from web");
            }
            break;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(52, 2);
    (void)poom_arduboy_print(F("DFU"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, 14);
    (void)poom_arduboy_print(line0);
    poom_arduboy_set_cursor(TEXT_X, 22);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(TEXT_X, 30);
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(TEXT_X, 38);
    (void)poom_arduboy_print(line3);
    poom_arduboy_set_cursor(TEXT_X, 46);
    (void)poom_arduboy_print(line4);

    poom_arduboy_set_cursor(72, 56);
    if(menu_dfu_exit_locked_()) {
        (void)poom_arduboy_print(F("B:WAIT"));
    } else {
        (void)poom_arduboy_print(F("B:BACK"));
    }

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_dfu_set_error`.
 *
 * @param[in] status Parameter passed to the helper.
 * @param[in] detail Parameter passed to the helper.
 * @return void
 */
static void menu_dfu_set_error_(const char* status, const char* detail)
{
    (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "%.17s", status ? status : "Error");
    (void)snprintf(s_menu_dfu_detail, sizeof(s_menu_dfu_detail), "%.21s", detail ? detail : "");
    s_menu_dfu_state = MENU_DFU_STATE_ERROR;
}

/**
 * @brief Internal helper for `dfu_event`.
 *
 * @param[in] event Parameter passed to the helper.
 * @param[in] context Parameter passed to the helper.
 * @return void
 */
static void dfu_event_(uint8_t event, void* context)
{
    switch(event)
    {
        case POOM_FW_UPDATE_SHOW_START_EVENT:
            POOM_DFU_PRINTF_I(POOM_DFU_MENU_TAG, "Event start");
            s_menu_dfu_state = MENU_DFU_STATE_UPLOADING;
            s_menu_dfu_progress = 0U;
            (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "Uploading");
            (void)snprintf(s_menu_dfu_detail, sizeof(s_menu_dfu_detail), "Receiving file");
            break;
        case POOM_FW_UPDATE_SHOW_PROGRESS_EVENT:
            POOM_DFU_PRINTF_D(POOM_DFU_MENU_TAG, "Event progress");
            s_menu_dfu_state = MENU_DFU_STATE_UPLOADING;
            if(context != NULL) {
                s_menu_dfu_progress = *((uint8_t*)context);
            }
            (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "Uploading");
            break;
        case POOM_FW_UPDATE_SHOW_RESULT_EVENT:
            POOM_DFU_PRINTF_I(POOM_DFU_MENU_TAG, "Event result");
            if((context != NULL) && *((bool*)context)) {
                s_menu_dfu_state = MENU_DFU_STATE_SUCCESS;
                (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "Success");
                (void)snprintf(s_menu_dfu_detail, sizeof(s_menu_dfu_detail), "Rebooting");
            } else {
                menu_dfu_set_error_("Error", "Upload failed");
            }
            break;
        default:
            break;
    }

    menu_dfu_request_render_();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_dfu_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_dfu_active = false;
    s_menu_dfu_exit_requested = false;
    s_menu_dfu_input_dirty = false;

    (void)poom_fw_update_set_show_event_cb(NULL);

    if(s_menu_dfu_backend_started) {
        (void)poom_fw_update_deinit();
        s_menu_dfu_backend_started = false;
    }

    if(s_menu_dfu_task != NULL) {
        if(s_menu_dfu_task != current_task) {
            TaskHandle_t task = s_menu_dfu_task;
            s_menu_dfu_task = NULL;
            vTaskDelete(task);
        } else {
            s_menu_dfu_task = NULL;
        }
    }

    if(s_menu_dfu_buttons_subscribed) {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_dfu_button_cb_, s_menu_dfu_sbus_user);
        s_menu_dfu_buttons_subscribed = false;
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_dfu_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    button_event_msg_t ev;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_event_msg_t))) {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if(ev.event != BUTTON_SINGLE_CLICK) {
        return;
    }

    if(ev.button == BTN_B) {
        if(!menu_dfu_exit_locked_()) {
            s_menu_dfu_exit_requested = true;
        }
        menu_dfu_request_render_();
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_dfu_task_(void* arg)
{
    esp_err_t err;

    (void)arg;

    menu_dfu_draw_();
    s_menu_dfu_input_dirty = false;

    (void)poom_fw_update_set_show_event_cb(dfu_event_);

    err = poom_fw_update_init();
    if(err == ESP_OK) {
        s_menu_dfu_backend_started = true;
        s_menu_dfu_state = MENU_DFU_STATE_READY;
        (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "Ready");
        (void)snprintf(s_menu_dfu_detail, sizeof(s_menu_dfu_detail), "Open browser");
    } else {
        POOM_DFU_PRINTF_E(POOM_DFU_MENU_TAG, "poom_fw_update_init failed: %s", esp_err_to_name(err));
        menu_dfu_set_error_("Init failed", esp_err_to_name(err));
    }
    menu_dfu_request_render_();

    while(s_menu_dfu_active) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MENU_DFU_REFRESH_MS));

        if(s_menu_dfu_exit_requested) {
            menu_dfu_exit_();
            break;
        }

        if(s_menu_dfu_input_dirty) {
            menu_dfu_draw_();
            s_menu_dfu_input_dirty = false;
        }
    }

    s_menu_dfu_task = NULL;
    vTaskDelete(NULL);
}

void dfu_start_task(void)
{
    if(s_menu_dfu_task != NULL) {
        return;
    }

    s_menu_dfu_active = true;
    s_menu_dfu_backend_started = false;
    s_menu_dfu_exit_requested = false;
    s_menu_dfu_input_dirty = true;
    s_menu_dfu_state = MENU_DFU_STATE_STARTING;
    s_menu_dfu_progress = 0U;
    (void)snprintf(s_menu_dfu_status, sizeof(s_menu_dfu_status), "Starting");
    (void)snprintf(s_menu_dfu_detail, sizeof(s_menu_dfu_detail), "Please wait");

    if(!s_menu_dfu_buttons_subscribed) {
        if(!poom_sbus_subscribe_cb("input/button", menu_dfu_button_cb_, s_menu_dfu_sbus_user)) {
            s_menu_dfu_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
        s_menu_dfu_buttons_subscribed = true;
    }

    if(xTaskCreate(menu_dfu_task_, "menu_dfu", MENU_DFU_STACK, NULL, MENU_DFU_PRIO, &s_menu_dfu_task) != pdPASS) {
        s_menu_dfu_active = false;
        if(s_menu_dfu_buttons_subscribed) {
            (void)poom_sbus_unsubscribe_cb("input/button", menu_dfu_button_cb_, s_menu_dfu_sbus_user);
            s_menu_dfu_buttons_subscribed = false;
        }
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}
