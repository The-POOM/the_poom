// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_wifi_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_scanner.h"
#include "poom_ui_keyboard.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_POOM_WIFI_SCAN_OLED_COL (6)
#define MENU_POOM_WIFI_SCAN_BOX_Y (10)
#define MENU_POOM_WIFI_SCAN_BOX_H (52)
#define MENU_POOM_WIFI_SCAN_REFRESH_MS (250U)
#define MENU_POOM_WIFI_SCAN_STATUS_STACK (3584U)
#define MENU_POOM_WIFI_SCAN_STATUS_PRIO (4U)
#define MENU_POOM_WIFI_SCAN_CONNECT_TIMEOUT_MS (15000U)
#define MENU_POOM_WIFI_SCAN_INFO_HOLD_CYCLES (6U)
#define MENU_POOM_WIFI_SCAN_PASS_MAX_LEN (63U)
#define MENU_POOM_WIFI_SCAN_VISIBLE_AP_LINES (3U)

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

#ifndef BUTTON_LONG_PRESS_START
#define BUTTON_LONG_PRESS_START (6U)
#endif

#ifndef BUTTON_LONG_PRESS_HOLD
#define BUTTON_LONG_PRESS_HOLD (7U)
#endif

#ifndef BUTTON_LONG_PRESS_UP
#define BUTTON_LONG_PRESS_UP (8U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_poom_wifi_scan_button_msg_t;

typedef enum
{
    MENU_POOM_WIFI_SCAN_STATE_SCANNING = 0,
    MENU_POOM_WIFI_SCAN_STATE_SELECT_AP,
    MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD,
    MENU_POOM_WIFI_SCAN_STATE_CONNECTING,
    MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE,
    MENU_POOM_WIFI_SCAN_STATE_INFO,
} menu_poom_wifi_scan_state_t;

static bool s_menu_poom_wifi_scan_active = false;
static bool s_menu_poom_wifi_scan_buttons_subscribed = false;
static bool s_menu_poom_wifi_scan_exit_requested = false;
static bool s_menu_poom_wifi_scan_scan_request_pending = false;
static bool s_menu_poom_wifi_scan_connect_request_pending = false;
static TaskHandle_t s_menu_poom_wifi_scan_status_task = NULL;
static char s_menu_poom_wifi_scan_sbus_user[] = "menu_poom_wifi_scan";
static menu_poom_wifi_scan_state_t s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SCANNING;
static uint16_t s_menu_poom_wifi_scan_selected_ap = 0U;
static uint16_t s_menu_poom_wifi_scan_ap_window_start = 0U;
static char s_menu_poom_wifi_scan_selected_ssid[33] = {0};
static char s_menu_poom_wifi_scan_password[MENU_POOM_WIFI_SCAN_PASS_MAX_LEN + 1U] = {0};
static size_t s_menu_poom_wifi_scan_password_len = 0U;
static poom_ui_keyboard_t s_menu_poom_wifi_scan_keyboard;
static bool s_menu_poom_wifi_scan_save_yes_selected = true;
static uint32_t s_menu_poom_wifi_scan_connect_start_ms = 0U;
static uint8_t s_menu_poom_wifi_scan_info_hold_cycles = 0U;
static char s_menu_poom_wifi_scan_status[22] = "Initializing";

static esp_err_t menu_poom_wifi_scan_exit_(void);
static void menu_poom_wifi_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_poom_wifi_scan_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx);

/**
 * @brief Internal helper for `menu_poom_wifi_scan_is_long_press_event`.
 *
 * @param[in] event Parameter passed to the helper.
 * @return bool
 */
static bool menu_poom_wifi_scan_is_long_press_event_(uint8_t event)
{
    return (event == BUTTON_LONG_PRESS_START) ||
           (event == BUTTON_LONG_PRESS_HOLD) ||
           (event == BUTTON_LONG_PRESS_UP);
}

/**
 * @brief Copies internal data into the destination buffer.
 *
 * @param[in] out_ssid Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @param[in] raw_ssid Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_scan_copy_ssid_(char *out_ssid, size_t out_len, const uint8_t *raw_ssid)
{
    size_t ssid_len;

    if((out_ssid == NULL) || (out_len == 0U))
    {
        return;
    }

    out_ssid[0] = '\0';
    if(raw_ssid == NULL)
    {
        return;
    }

    ssid_len = strnlen((const char *)raw_ssid, 32U);
    if(ssid_len >= out_len)
    {
        ssid_len = out_len - 1U;
    }

    if(ssid_len > 0U)
    {
        (void)memcpy(out_ssid, raw_ssid, ssid_len);
    }
    out_ssid[ssid_len] = '\0';
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_now_ms`.
 *
 * @return uint32_t
 */
static uint32_t menu_poom_wifi_scan_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief Returns the display label for the current state.
 *
 * @return const char *
 */
static const char *menu_poom_wifi_scan_selected_ssid_label_(void)
{
    if(s_menu_poom_wifi_scan_selected_ssid[0] == '\0')
    {
        return "<hidden>";
    }

    return s_menu_poom_wifi_scan_selected_ssid;
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_get_ap_count`.
 *
 * @return uint16_t
 */
static uint16_t menu_poom_wifi_scan_get_ap_count_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();

    if(records == NULL)
    {
        return 0U;
    }

    return records->count;
}

/**
 * @brief Clears the internal state used by this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_scan_clear_password_(void)
{
    (void)memset(s_menu_poom_wifi_scan_password, 0, sizeof(s_menu_poom_wifi_scan_password));
    s_menu_poom_wifi_scan_password_len = 0U;
    poom_ui_keyboard_init(&s_menu_poom_wifi_scan_keyboard,
                          s_menu_poom_wifi_scan_password,
                          sizeof(s_menu_poom_wifi_scan_password),
                          s_menu_poom_wifi_scan_selected_ssid);
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_enter_password_state`.
 *
 * @return void
 */
static void menu_poom_wifi_scan_enter_password_state_(void)
{
    wifi_ap_record_t *ap_record = poom_wifi_scanner_get_ap_record(s_menu_poom_wifi_scan_selected_ap);

    if(ap_record == NULL)
    {
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Invalid AP");
        return;
    }

    menu_poom_wifi_scan_copy_ssid_(s_menu_poom_wifi_scan_selected_ssid,
                                   sizeof(s_menu_poom_wifi_scan_selected_ssid),
                                   ap_record->ssid);

    menu_poom_wifi_scan_clear_password_();
    poom_ui_keyboard_set_target_label(&s_menu_poom_wifi_scan_keyboard, s_menu_poom_wifi_scan_selected_ssid);
    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD;
    (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Enter password");
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_store_credentials`.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_wifi_scan_store_credentials_(void)
{
    esp_err_t status;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        return status;
    }

    status = poom_secrets_set_wifi_ssid(s_menu_poom_wifi_scan_selected_ssid);
    if(status != ESP_OK)
    {
        return status;
    }

    return poom_secrets_set_wifi_pass(s_menu_poom_wifi_scan_password);
}

/**
 * @brief Draws the current menu state.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_wifi_scan_draw_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];

    (void)memset(line1, 0, sizeof(line1));
    (void)memset(line2, 0, sizeof(line2));
    (void)memset(line3, 0, sizeof(line3));

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("WIFI SCAN"));

    switch(s_menu_poom_wifi_scan_state)
    {
        case MENU_POOM_WIFI_SCAN_STATE_SELECT_AP:
        {
            poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
            uint16_t count = (records != NULL) ? records->count : 0U;

            char count_buf[10];
            (void)snprintf(count_buf, sizeof(count_buf), "AP:%u", (unsigned)count);
            poom_arduboy_set_cursor(90, 2);
            (void)poom_arduboy_print(count_buf);
            break;
        }

        case MENU_POOM_WIFI_SCAN_STATE_CONNECTING:
            poom_arduboy_set_cursor(88, 2);
            (void)poom_arduboy_print(F("..."));
            break;

        default:
            break;
    }

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    switch(s_menu_poom_wifi_scan_state)
    {
        case MENU_POOM_WIFI_SCAN_STATE_SCANNING:
            poom_arduboy_set_cursor(22, 26);
            (void)poom_arduboy_print(F("Scanning..."));
            poom_arduboy_set_cursor(22, 36);
            (void)poom_arduboy_print(F("Please wait"));
            poom_arduboy_set_cursor(72, 56);
            (void)poom_arduboy_print(F("B:EXIT"));
            break;

        case MENU_POOM_WIFI_SCAN_STATE_SELECT_AP:
        {
            poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
            uint16_t count = (records != NULL) ? records->count : 0U;

            if(count == 0U)
            {
                poom_arduboy_set_cursor(22, 26);
                (void)poom_arduboy_print(F("No AP found"));
                poom_arduboy_set_cursor(28, 40);
                (void)poom_arduboy_print(F("A:RESCAN"));
                poom_arduboy_set_cursor(72, 56);
                (void)poom_arduboy_print(F("B:EXIT"));
                break;
            }

            const uint8_t visible = MENU_POOM_WIFI_SCAN_VISIBLE_AP_LINES;
            for(uint8_t row = 0; row < visible; row++)
            {
                uint16_t index = (uint16_t)(s_menu_poom_wifi_scan_ap_window_start + row);
                if(index >= count)
                {
                    break;
                }

                char ssid[33];
                menu_poom_wifi_scan_copy_ssid_(ssid, sizeof(ssid), records->records[index].ssid);
                if(ssid[0] == '\0')
                {
                    (void)snprintf(ssid, sizeof(ssid), "%s", "<hidden>");
                }

                char ap_line[16];
                (void)snprintf(ap_line, sizeof(ap_line), "%.14s", ssid);

                const int16_t y = (int16_t)(14 + (int16_t)row * 10);
                poom_arduboy_set_cursor(2, y);
                (void)poom_arduboy_print(ap_line);

                if(index == s_menu_poom_wifi_scan_selected_ap)
                {
                    poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
                }
            }

            if(s_menu_poom_wifi_scan_ap_window_start > 0U)
            {
                poom_arduboy_fill_triangle(124, 12, 120, 16, 127, 16, WHITE);
            }
            if((uint16_t)(s_menu_poom_wifi_scan_ap_window_start + MENU_POOM_WIFI_SCAN_VISIBLE_AP_LINES) < count)
            {
                poom_arduboy_fill_triangle(120, 48, 127, 48, 124, 52, WHITE);
            }

            poom_arduboy_set_cursor(0, 56);
            (void)poom_arduboy_print(F("A:EDIT"));
            poom_arduboy_set_cursor(72, 56);
            (void)poom_arduboy_print(F("B:EXIT"));
            break;
        }

        case MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD:
            poom_ui_keyboard_draw(&s_menu_poom_wifi_scan_keyboard);
            return ESP_OK;

        case MENU_POOM_WIFI_SCAN_STATE_CONNECTING:
            poom_arduboy_set_cursor(22, 26);
            (void)poom_arduboy_print(F("Connecting..."));
            (void)snprintf(line1, sizeof(line1), "SSID:%.14s", menu_poom_wifi_scan_selected_ssid_label_());
            poom_arduboy_set_cursor(2, 38);
            (void)poom_arduboy_print(line1);
            poom_arduboy_set_cursor(72, 56);
            (void)poom_arduboy_print(F("B:CANCEL"));
            break;

        case MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE:
        {
            poom_arduboy_set_cursor(28, 18);
            (void)poom_arduboy_print(F("Connected"));
            poom_arduboy_set_cursor(10, 30);
            (void)poom_arduboy_print(F("Save credential?"));

            const int16_t y = 42;
            poom_arduboy_set_cursor(32, y);
            (void)poom_arduboy_print(F("YES"));
            poom_arduboy_set_cursor(80, y);
            (void)poom_arduboy_print(F("NO"));

            if(s_menu_poom_wifi_scan_save_yes_selected)
            {
                poom_arduboy_fill_rect(28, (int16_t)(y - 1), 24, 9, INVERT);
            }
            else
            {
                poom_arduboy_fill_rect(76, (int16_t)(y - 1), 18, 9, INVERT);
            }

            poom_arduboy_set_cursor(0, 56);
            (void)poom_arduboy_print(F("A:OK"));
            poom_arduboy_set_cursor(72, 56);
            (void)poom_arduboy_print(F("B:NO"));
            break;
        }

        case MENU_POOM_WIFI_SCAN_STATE_INFO:
        default:
            (void)snprintf(line1, sizeof(line1), "%.20s", s_menu_poom_wifi_scan_status);
            poom_arduboy_set_cursor(6, 30);
            (void)poom_arduboy_print(line1);
            poom_arduboy_set_cursor(28, 44);
            (void)poom_arduboy_print(F("Returning..."));
            break;
    }

    poom_arduboy_display();
    return ESP_OK;
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_run_scan`.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_wifi_scan_run_scan_(void)
{
    esp_err_t status;
    uint16_t count;

    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SCANNING;
    (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Scanning...");
    (void)menu_poom_wifi_scan_draw_();

    (void)poom_wifi_scanner_clear_ap_records();
    status = poom_wifi_scanner_scan();
    if(status != ESP_OK)
    {
        s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SELECT_AP;
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Scan failed");
        return status;
    }

    count = menu_poom_wifi_scan_get_ap_count_();
    s_menu_poom_wifi_scan_selected_ap = 0U;
    s_menu_poom_wifi_scan_ap_window_start = 0U;
    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SELECT_AP;

    if(count == 0U)
    {
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "No APs");
    }
    else
    {
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Select AP");
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `menu_poom_wifi_scan_update_connect_state`.
 *
 * @return void
 */
static void menu_poom_wifi_scan_update_connect_state_(void)
{
    uint32_t now_ms = menu_poom_wifi_scan_now_ms_();

    if(poom_wifi_ctrl_sta_has_ip())
    {
        s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE;
        s_menu_poom_wifi_scan_save_yes_selected = true;
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Connected");
        return;
    }

    if((now_ms - s_menu_poom_wifi_scan_connect_start_ms) >= MENU_POOM_WIFI_SCAN_CONNECT_TIMEOUT_MS)
    {
        s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD;
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Connect timeout");
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_scan_handle_select_ap_button_(uint8_t button)
{
    uint16_t count = menu_poom_wifi_scan_get_ap_count_();

    if(count == 0U)
    {
        if(button == BTN_A)
        {
            s_menu_poom_wifi_scan_scan_request_pending = true;
        }
        return;
    }

    if(button == BTN_UP)
    {
        if(s_menu_poom_wifi_scan_selected_ap > 0U)
        {
            s_menu_poom_wifi_scan_selected_ap--;
        }
    }
    else if(button == BTN_DOWN)
    {
        if(s_menu_poom_wifi_scan_selected_ap < (uint16_t)(count - 1U))
        {
            s_menu_poom_wifi_scan_selected_ap++;
        }
    }
    else if((button == BTN_A) || (button == BTN_RIGHT))
    {
        menu_poom_wifi_scan_enter_password_state_();
        return;
    }

    if(s_menu_poom_wifi_scan_selected_ap < s_menu_poom_wifi_scan_ap_window_start)
    {
        s_menu_poom_wifi_scan_ap_window_start = s_menu_poom_wifi_scan_selected_ap;
    }

    if(s_menu_poom_wifi_scan_selected_ap >= (uint16_t)(s_menu_poom_wifi_scan_ap_window_start +
                                                        MENU_POOM_WIFI_SCAN_VISIBLE_AP_LINES))
    {
        s_menu_poom_wifi_scan_ap_window_start =
            (uint16_t)(s_menu_poom_wifi_scan_selected_ap - MENU_POOM_WIFI_SCAN_VISIBLE_AP_LINES + 1U);
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return poom_ui_keyboard_action_t
 */
static poom_ui_keyboard_action_t menu_poom_wifi_scan_handle_edit_password_button_(uint8_t button)
{
    poom_ui_keyboard_action_t action =
        poom_ui_keyboard_handle_button(&s_menu_poom_wifi_scan_keyboard, button);
    s_menu_poom_wifi_scan_password_len = s_menu_poom_wifi_scan_keyboard.text_len;
    return action;
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_scan_handle_save_confirm_(void)
{
    esp_err_t status = ESP_OK;

    if(s_menu_poom_wifi_scan_save_yes_selected)
    {
        status = menu_poom_wifi_scan_store_credentials_();
        if(status == ESP_OK)
        {
            (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Saved");
        }
        else
        {
            (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Save failed");
        }
    }
    else
    {
        (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Not saved");
    }

    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_INFO;
    s_menu_poom_wifi_scan_info_hold_cycles = MENU_POOM_WIFI_SCAN_INFO_HOLD_CYCLES;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_wifi_scan_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_poom_wifi_scan_active = false;
    s_menu_poom_wifi_scan_exit_requested = false;
    s_menu_poom_wifi_scan_scan_request_pending = false;
    s_menu_poom_wifi_scan_connect_request_pending = false;

    if(s_menu_poom_wifi_scan_status_task != NULL)
    {
        if(s_menu_poom_wifi_scan_status_task != current_task)
        {
            TaskHandle_t status_task = s_menu_poom_wifi_scan_status_task;
            s_menu_poom_wifi_scan_status_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_poom_wifi_scan_status_task = NULL;
        }
    }

    if(s_menu_poom_wifi_scan_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_poom_wifi_scan_button_cb_, s_menu_poom_wifi_scan_sbus_user);
        s_menu_poom_wifi_scan_buttons_subscribed = false;
    }

    (void)poom_wifi_ctrl_unregister_cb();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] info Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_scan_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if(info == NULL)
    {
        return;
    }

    if(info->evt == POOM_WIFI_CTRL_EVT_STA_CONNECTED)
    {
        printf("[I] [menu_poom_wifi_scan] wifi evt: STA_CONNECTED\n");
    }
    else if(info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)
    {
        printf("[I] [menu_poom_wifi_scan] wifi evt: GOT_IP=%u.%u.%u.%u\n",
               (unsigned)(info->ip.addr & 0xFFU),
               (unsigned)((info->ip.addr >> 8) & 0xFFU),
               (unsigned)((info->ip.addr >> 16) & 0xFFU),
               (unsigned)((info->ip.addr >> 24) & 0xFFU));
    }
    else if(info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        printf("[W] [menu_poom_wifi_scan] wifi evt: STA_DISCONNECTED reason=%ld\n", (long)info->reason);
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_scan_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    menu_poom_wifi_scan_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));

    if(button_msg.event == BUTTON_SINGLE_CLICK)
    {
        if(button_msg.button == BTN_B)
        {
            if(s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD)
            {
                s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SELECT_AP;
                (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Select AP");
            }
            else if(s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE)
            {
                s_menu_poom_wifi_scan_save_yes_selected = false;
                menu_poom_wifi_scan_handle_save_confirm_();
            }
            else if(s_menu_poom_wifi_scan_status_task == NULL)
            {
                (void)menu_poom_wifi_scan_exit_();
            }
            else
            {
                s_menu_poom_wifi_scan_exit_requested = true;
            }
            return;
        }

        switch(s_menu_poom_wifi_scan_state)
        {
            case MENU_POOM_WIFI_SCAN_STATE_SELECT_AP:
                menu_poom_wifi_scan_handle_select_ap_button_(button_msg.button);
                break;

            case MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD:
            {
                poom_ui_keyboard_action_t action = menu_poom_wifi_scan_handle_edit_password_button_(button_msg.button);
                if((action == POOM_UI_KEYBOARD_ACTION_ACCEPT) && !s_menu_poom_wifi_scan_connect_request_pending)
                {
                    s_menu_poom_wifi_scan_connect_request_pending = true;
                    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_CONNECTING;
                    (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Connecting...");
                }
                break;
            }

            case MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE:
                if((button_msg.button == BTN_LEFT) || (button_msg.button == BTN_UP))
                {
                    s_menu_poom_wifi_scan_save_yes_selected = true;
                }
                else if((button_msg.button == BTN_RIGHT) || (button_msg.button == BTN_DOWN))
                {
                    s_menu_poom_wifi_scan_save_yes_selected = false;
                }
                else if(button_msg.button == BTN_A)
                {
                    menu_poom_wifi_scan_handle_save_confirm_();
                }
                break;

            default:
                break;
        }
    }
    else if((button_msg.button == BTN_A) &&
            menu_poom_wifi_scan_is_long_press_event_(button_msg.event))
    {
        if(s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_SELECT_AP)
        {
            menu_poom_wifi_scan_enter_password_state_();
            return;
        }

        if(s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_ASK_SAVE)
        {
            menu_poom_wifi_scan_handle_save_confirm_();
            return;
        }

        if((s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD) &&
           !s_menu_poom_wifi_scan_connect_request_pending)
        {
            s_menu_poom_wifi_scan_password_len = s_menu_poom_wifi_scan_keyboard.text_len;
            s_menu_poom_wifi_scan_connect_request_pending = true;
            s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_CONNECTING;
            (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Connecting...");
        }
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_scan_status_task_(void *task_arg)
{
    (void)task_arg;

    while(s_menu_poom_wifi_scan_active)
    {
        if(s_menu_poom_wifi_scan_exit_requested)
        {
            (void)menu_poom_wifi_scan_exit_();
            break;
        }

        if(s_menu_poom_wifi_scan_scan_request_pending)
        {
            s_menu_poom_wifi_scan_scan_request_pending = false;
            (void)menu_poom_wifi_scan_run_scan_();
        }

        if(s_menu_poom_wifi_scan_connect_request_pending)
        {
            esp_err_t status;

            s_menu_poom_wifi_scan_connect_request_pending = false;
            printf("[I] [menu_poom_wifi_scan] connect ssid='%s' pw='%s'\n",
                   s_menu_poom_wifi_scan_selected_ssid,
                   (s_menu_poom_wifi_scan_password_len > 0U) ? s_menu_poom_wifi_scan_password : "");
            status = poom_wifi_ctrl_sta_connect(
                s_menu_poom_wifi_scan_selected_ssid,
                (s_menu_poom_wifi_scan_password_len > 0U) ? s_menu_poom_wifi_scan_password : NULL);
            printf("[I] [menu_poom_wifi_scan] connect status=%s (%d)\n",
                   esp_err_to_name(status),
                   (int)status);
            if(status != ESP_OK)
            {
                s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_EDIT_PASSWORD;
                (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Connect failed");
            }
            else
            {
                s_menu_poom_wifi_scan_connect_start_ms = menu_poom_wifi_scan_now_ms_();
                s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_CONNECTING;
                (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Waiting IP...");
            }
        }

        if(s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_CONNECTING)
        {
            menu_poom_wifi_scan_update_connect_state_();
        }
        else if((s_menu_poom_wifi_scan_state == MENU_POOM_WIFI_SCAN_STATE_INFO) &&
                (s_menu_poom_wifi_scan_info_hold_cycles > 0U))
        {
            s_menu_poom_wifi_scan_info_hold_cycles--;
            if(s_menu_poom_wifi_scan_info_hold_cycles == 0U)
            {
                s_menu_poom_wifi_scan_exit_requested = true;
            }
        }

        (void)menu_poom_wifi_scan_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_POOM_WIFI_SCAN_REFRESH_MS));
    }

    s_menu_poom_wifi_scan_status_task = NULL;
    vTaskDelete(NULL);
}

void menu_poom_wifi_scan_show(void)
{
    s_menu_poom_wifi_scan_active = true;
    s_menu_poom_wifi_scan_exit_requested = false;
    s_menu_poom_wifi_scan_scan_request_pending = true;
    s_menu_poom_wifi_scan_connect_request_pending = false;
    s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_SCANNING;
    s_menu_poom_wifi_scan_selected_ap = 0U;
    s_menu_poom_wifi_scan_ap_window_start = 0U;
    s_menu_poom_wifi_scan_save_yes_selected = true;
    s_menu_poom_wifi_scan_info_hold_cycles = 0U;
    (void)memset(s_menu_poom_wifi_scan_selected_ssid, 0, sizeof(s_menu_poom_wifi_scan_selected_ssid));
    menu_poom_wifi_scan_clear_password_();
    (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Scanning...");
    (void)poom_wifi_ctrl_register_cb(menu_poom_wifi_scan_wifi_evt_cb_, NULL);

    if(!s_menu_poom_wifi_scan_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_poom_wifi_scan_button_cb_, s_menu_poom_wifi_scan_sbus_user))
        {
            s_menu_poom_wifi_scan_buttons_subscribed = true;
        }
        else
        {
            s_menu_poom_wifi_scan_active = false;
            s_menu_poom_wifi_scan_state = MENU_POOM_WIFI_SCAN_STATE_INFO;
            (void)snprintf(s_menu_poom_wifi_scan_status, sizeof(s_menu_poom_wifi_scan_status), "Button sub error");
            (void)menu_poom_wifi_scan_draw_();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_poom_wifi_scan_status_task == NULL)
    {
        (void)xTaskCreate(menu_poom_wifi_scan_status_task_,
                          "menu_wifi_scan",
                          MENU_POOM_WIFI_SCAN_STATUS_STACK,
                          NULL,
                          MENU_POOM_WIFI_SCAN_STATUS_PRIO,
                          &s_menu_poom_wifi_scan_status_task);
    }
}
