// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_deauth.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_wifi_attacks.h"
#include "poom_wifi_scanner.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MAX_WIFI_ITEMS (20)
#define MAX_ATTACK_ITEMS (3)

#define VISIBLE_ROWS (4)
#define LIST_Y0 (14)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)

#define MENU_DEAUTH_REFRESH_MS (100U)
#define MENU_DEAUTH_SCAN_SETTLE_MS (2000U)
#define MENU_DEAUTH_STACK (4096U)
#define MENU_DEAUTH_PRIO (5U)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_DEAUTH_VIEW_IDLE = 0,
    MENU_DEAUTH_VIEW_SCANNING,
    MENU_DEAUTH_VIEW_WIFI_LIST,
    MENU_DEAUTH_VIEW_ATTACK_LIST,
    MENU_DEAUTH_VIEW_ATTACK_RUNNING,
} menu_deauth_view_t;

static const char *s_attack_names[MAX_ATTACK_ITEMS] = {"deauth", "Wi-Fi Clone", "Combine"};

static char s_ssid_buf[MAX_WIFI_ITEMS][33];
static int8_t s_rssi_buf[MAX_WIFI_ITEMS];
static uint8_t s_ch_buf[MAX_WIFI_ITEMS];
static int s_wifi_ap_map[MAX_WIFI_ITEMS];
static int s_wifi_count = 0;

static int s_selected_index = 0;
static int s_scroll = 0;
static int s_attack_selected = 0;
static int s_wifi_selected_ap = -1;

static menu_deauth_view_t s_view = MENU_DEAUTH_VIEW_IDLE;
static bool s_active = false;
static bool s_exit_requested = false;
static bool s_scan_requested = false;
static bool s_ui_dirty = true;
static bool s_buttons_subscribed = false;
static TaskHandle_t s_ui_task = NULL;

static void menu_deauth_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_deauth_ui_task_(void *arg);
static void menu_deauth_exit_(void);

/**
 * @brief Draws the menu header.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_draw_header_(const char *title)
{
    poom_arduboy_set_cursor(46, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_deauth_draw_idle_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_deauth_draw_header_(F("DEAUTH"));

    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(12, 22);
    (void)poom_arduboy_print(F("Press A to"));

    poom_arduboy_set_cursor(12, 34);
    (void)poom_arduboy_print(F("scan Wi-Fi"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SCAN"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_deauth_draw_scanning_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_deauth_draw_header_(F("DEAUTH"));

    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(28, 30);
    (void)poom_arduboy_print(F("Scanning..."));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_deauth_render_wifi_list_(void)
{
    if (s_selected_index < 0)
    {
        s_selected_index = 0;
    }
    if (s_selected_index > (s_wifi_count - 1))
    {
        s_selected_index = (s_wifi_count - 1);
    }

    if (s_selected_index < s_scroll)
    {
        s_scroll = s_selected_index;
    }
    if (s_selected_index >= (s_scroll + VISIBLE_ROWS))
    {
        s_scroll = s_selected_index - VISIBLE_ROWS + 1;
    }

    int max_scroll = s_wifi_count - VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_scroll < 0)
    {
        s_scroll = 0;
    }
    if (s_scroll > max_scroll)
    {
        s_scroll = max_scroll;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_deauth_draw_header_(F("DEAUTH"));

    poom_arduboy_set_cursor(98, 2);
    char count_buf[10];
    (void)snprintf(count_buf, sizeof(count_buf), "%d", s_wifi_count);
    (void)poom_arduboy_print(count_buf);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SELECT"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_scroll + row;
        if (idx >= s_wifi_count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);

        char ssid_line[16];
        (void)snprintf(ssid_line, sizeof(ssid_line), "%.14s", s_ssid_buf[idx]);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(ssid_line);

        char rssi_small[10];
        (void)snprintf(rssi_small, sizeof(rssi_small), "%dd", (int)s_rssi_buf[idx]);
        poom_arduboy_set_cursor(86, y);
        (void)poom_arduboy_print(rssi_small);

        char ch_small[6];
        (void)snprintf(ch_small, sizeof(ch_small), "C%u", (unsigned)s_ch_buf[idx]);
        poom_arduboy_set_cursor(110, y);
        (void)poom_arduboy_print(ch_small);

        if (idx == s_selected_index)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    if (s_scroll > 0)
    {
        poom_arduboy_fill_triangle(124, (int16_t)(LIST_Y0 - 3), 120, (int16_t)(LIST_Y0 + 1), 127, (int16_t)(LIST_Y0 + 1), WHITE);
    }
    if (s_scroll < max_scroll)
    {
        const int16_t y = (int16_t)(LIST_Y0 + (VISIBLE_ROWS - 1) * ROW_STEP + 6);
        poom_arduboy_fill_triangle(120, y, 127, y, 124, (int16_t)(y + 4), WHITE);
    }

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @param[in] running Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_render_attack_list_(bool running)
{
    const char *ssid = "(none)";
    if (s_wifi_selected_ap >= 0)
    {
        wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record((unsigned)s_wifi_selected_ap);
        if ((ap != NULL) && ap->ssid[0])
        {
            ssid = (const char *)ap->ssid;
        }
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_deauth_draw_header_(F("ATTACK"));

    char ssid_line[15];
    (void)snprintf(ssid_line, sizeof(ssid_line), "%.14s", ssid);
    poom_arduboy_set_cursor(0, 14);
    (void)poom_arduboy_print(ssid_line);

    if (running)
    {
        poom_arduboy_set_cursor(97, 14);
        (void)poom_arduboy_print(F("RUN"));
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(running ? F("A:PAUSE") : F("A:START"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    for (int i = 0; i < MAX_ATTACK_ITEMS; i++)
    {
        const int16_t y = (int16_t)(26 + i * 10);

        char name_line[16];
        (void)snprintf(name_line, sizeof(name_line), "%.14s", s_attack_names[i]);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(name_line);

        if (i == s_attack_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_deauth_build_wifi_list`.
 *
 * @return void
 */
static void menu_deauth_build_wifi_list_(void)
{
    s_wifi_count = 0;

    (void)poom_wifi_scanner_clear_ap_records();
    (void)poom_wifi_scanner_scan();
    vTaskDelay(pdMS_TO_TICKS(MENU_DEAUTH_SCAN_SETTLE_MS));

    poom_wifi_scanner_ap_records_t *r = poom_wifi_scanner_get_ap_records();
    if ((r == NULL) || (r->count == 0))
    {
        ESP_LOGW("menu_deauth", "No networks found");
        return;
    }

    const int count = (r->count > MAX_WIFI_ITEMS) ? MAX_WIFI_ITEMS : (int)r->count;
    for (int i = 0; i < count; i++)
    {
        wifi_ap_record_t *ap = &r->records[i];

        (void)snprintf(s_ssid_buf[i], sizeof(s_ssid_buf[i]), "%.32s", (const char *)ap->ssid);
        s_rssi_buf[i] = (int8_t)ap->rssi;
        s_ch_buf[i] = (uint8_t)ap->primary;
        s_wifi_ap_map[i] = i;
    }

    s_wifi_count = count;
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] attack_type Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_start_attack_(int attack_type)
{
    wifi_ap_record_t *target = NULL;

    if (s_wifi_selected_ap >= 0)
    {
        target = poom_wifi_scanner_get_ap_record((unsigned)s_wifi_selected_ap);
    }

    if (target == NULL)
    {
        return;
    }

    poom_wifi_attacks_stop_all();

    printf("\n[LAB] Attack type %d on SSID=%s\n", attack_type, (char *)target->ssid);
    poom_wifi_attacks_handle((poom_wifi_attacks_type_t)attack_type, target);

    s_view = MENU_DEAUTH_VIEW_ATTACK_RUNNING;
    s_ui_dirty = true;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_deauth_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_active = false;
    s_exit_requested = false;
    s_scan_requested = false;
    s_ui_dirty = false;

    poom_wifi_attacks_stop_all();

    if (s_ui_task != NULL)
    {
        if (s_ui_task != current_task)
        {
            TaskHandle_t ui_task = s_ui_task;
            s_ui_task = NULL;
            vTaskDelete(ui_task);
        }
        else
        {
            s_ui_task = NULL;
        }
    }

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_deauth_button_cb_, "menu_deauth");
        s_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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
        if (s_view == MENU_DEAUTH_VIEW_ATTACK_LIST || s_view == MENU_DEAUTH_VIEW_ATTACK_RUNNING)
        {
            poom_wifi_attacks_stop_all();
            s_view = (s_wifi_count > 0) ? MENU_DEAUTH_VIEW_WIFI_LIST : MENU_DEAUTH_VIEW_IDLE;
            s_ui_dirty = true;
            return;
        }

        s_exit_requested = true;
        return;
    }

    if (s_view == MENU_DEAUTH_VIEW_IDLE)
    {
        if (ev.button == BUTTON_A)
        {
            s_scan_requested = true;
        }
        return;
    }

    if (s_view == MENU_DEAUTH_VIEW_SCANNING)
    {
        return;
    }

    if (s_view == MENU_DEAUTH_VIEW_WIFI_LIST)
    {
        if (s_wifi_count == 0)
        {
            if (ev.button == BUTTON_A)
            {
                s_scan_requested = true;
            }
            return;
        }

        if ((ev.button == BUTTON_UP) && (s_selected_index > 0))
        {
            s_selected_index--;
            s_ui_dirty = true;
        }
        else if ((ev.button == BUTTON_DOWN) && (s_selected_index < (s_wifi_count - 1)))
        {
            s_selected_index++;
            s_ui_dirty = true;
        }
        else if (ev.button == BUTTON_A)
        {
            s_wifi_selected_ap = s_wifi_ap_map[s_selected_index];
            s_attack_selected = 0;
            s_view = MENU_DEAUTH_VIEW_ATTACK_LIST;
            s_ui_dirty = true;
        }

        return;
    }

    if (s_view == MENU_DEAUTH_VIEW_ATTACK_LIST || s_view == MENU_DEAUTH_VIEW_ATTACK_RUNNING)
    {
        if ((ev.button == BUTTON_UP) && (s_attack_selected > 0))
        {
            s_attack_selected--;
            s_ui_dirty = true;
        }
        else if ((ev.button == BUTTON_DOWN) && (s_attack_selected < (MAX_ATTACK_ITEMS - 1)))
        {
            s_attack_selected++;
            s_ui_dirty = true;
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_view == MENU_DEAUTH_VIEW_ATTACK_RUNNING)
            {
                poom_wifi_attacks_stop_all();
                s_view = MENU_DEAUTH_VIEW_ATTACK_LIST;
                s_ui_dirty = true;
            }
            else
            {
                menu_deauth_start_attack_(s_attack_selected);
            }
        }

        return;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_ui_task_(void *arg)
{
    (void)arg;

    while (s_active)
    {
        if (s_exit_requested)
        {
            menu_deauth_exit_();
            break;
        }

        if (s_scan_requested)
        {
            s_scan_requested = false;
            s_view = MENU_DEAUTH_VIEW_SCANNING;
            menu_deauth_draw_scanning_();

            menu_deauth_build_wifi_list_();

            s_selected_index = 0;
            s_scroll = 0;
            s_wifi_selected_ap = -1;
            s_attack_selected = 0;

            if (s_wifi_count > 0)
            {
                s_view = MENU_DEAUTH_VIEW_WIFI_LIST;
            }
            else
            {
                s_view = MENU_DEAUTH_VIEW_IDLE;
            }

            s_ui_dirty = true;
        }

        if (s_ui_dirty)
        {
            s_ui_dirty = false;

            switch (s_view)
            {
                case MENU_DEAUTH_VIEW_IDLE:
                    menu_deauth_draw_idle_();
                    break;
                case MENU_DEAUTH_VIEW_SCANNING:
                    menu_deauth_draw_scanning_();
                    break;
                case MENU_DEAUTH_VIEW_WIFI_LIST:
                    if (s_wifi_count > 0)
                    {
                        menu_deauth_render_wifi_list_();
                    }
                    else
                    {
                        menu_deauth_draw_idle_();
                    }
                    break;
                case MENU_DEAUTH_VIEW_ATTACK_LIST:
                    menu_deauth_render_attack_list_(false);
                    break;
                case MENU_DEAUTH_VIEW_ATTACK_RUNNING:
                    menu_deauth_render_attack_list_(true);
                    break;
                default:
                    menu_deauth_draw_idle_();
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_DEAUTH_REFRESH_MS));
    }

    if (s_ui_task != NULL)
    {
        s_ui_task = NULL;
    }
    vTaskDelete(NULL);
}

void app_deauth(void)
{
    memset(s_ssid_buf, 0, sizeof(s_ssid_buf));
    memset(s_rssi_buf, 0, sizeof(s_rssi_buf));
    memset(s_ch_buf, 0, sizeof(s_ch_buf));
    memset(s_wifi_ap_map, 0, sizeof(s_wifi_ap_map));

    s_wifi_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_attack_selected = 0;
    s_wifi_selected_ap = -1;

    s_view = MENU_DEAUTH_VIEW_IDLE;
    s_active = true;
    s_exit_requested = false;
    s_scan_requested = false;
    s_ui_dirty = true;

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_deauth_button_cb_, "menu_deauth");
        s_buttons_subscribed = true;
    }

    if (s_ui_task == NULL)
    {
        (void)xTaskCreate(menu_deauth_ui_task_, "menu_deauth_ui", MENU_DEAUTH_STACK, NULL, MENU_DEAUTH_PRIO, &s_ui_task);
    }
}
