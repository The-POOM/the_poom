// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_captive.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "sd_card.h"
#include "poom_ui_keyboard.h"
#include "poom_wifi_captive.h"
#include "poom_wifi_scanner.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

typedef enum
{
    MENU_CAPTIVE_VIEW_SELECT = 0,
    MENU_CAPTIVE_VIEW_MSG,
    MENU_CAPTIVE_VIEW_KEYBOARD,
    MENU_CAPTIVE_VIEW_SCAN_LIST,
    MENU_CAPTIVE_VIEW_STATUS,
} menu_captive_view_t;

typedef enum
{
    MENU_CAPTIVE_OPT_SD = 0,
    MENU_CAPTIVE_OPT_KEYBOARD,
    MENU_CAPTIVE_OPT_SCAN,
    MENU_CAPTIVE_OPT_COFFEE,
    MENU_CAPTIVE_OPT_COUNT,
} menu_captive_option_t;

static bool s_menu_captive_running = false;
static bool s_menu_captive_buttons_subscribed = false;
static char s_menu_captive_sbus_user[] = "menu_captive";
static menu_captive_view_t s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
static uint8_t s_menu_captive_selected_opt = 0U;
static uint16_t s_menu_captive_scan_selected = 0U;
static uint16_t s_menu_captive_scan_window_start = 0U;
static char s_menu_captive_status[22] = "";
static char s_menu_captive_ap_ssid[33] = {0};
static char s_menu_captive_keyboard_ssid[33] = {0};
static poom_ui_keyboard_t s_menu_captive_keyboard;

static const char *const s_menu_captive_opt_labels[MENU_CAPTIVE_OPT_COUNT] = {
    "SD",
    "Type SSID",
    "Scan SSID",
    "Coffee",
};

static void menu_captive_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_captive_draw_header_(void)
{
    poom_arduboy_set_cursor(40, 2);
    (void)poom_arduboy_print(F("CAPTIVE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_captive_draw_select_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_captive_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    for (uint8_t i = 0; i < (uint8_t)MENU_CAPTIVE_OPT_COUNT; i++)
    {
        const int16_t y = (int16_t)(ROW0_Y + (int16_t)i * ROW_STEP);
        poom_arduboy_set_cursor(TEXT_X, y);
        (void)poom_arduboy_print(s_menu_captive_opt_labels[i]);

        if (i == s_menu_captive_selected_opt)
        {
            poom_arduboy_fill_rect(1, (int16_t)(y - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_captive_draw_msg_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_captive_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(s_menu_captive_status);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_captive_show_msg`.
 *
 * @param[in] msg Parameter passed to the helper.
 * @return void
 */
static void menu_captive_show_msg_(const char *msg)
{
    (void)snprintf(s_menu_captive_status, sizeof(s_menu_captive_status), "%.21s", (msg != NULL) ? msg : "");
    s_menu_captive_view = MENU_CAPTIVE_VIEW_MSG;
    menu_captive_draw_msg_();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_captive_draw_scan_list_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
    const uint16_t count = (records != NULL) ? records->count : 0U;
    const uint8_t visible = 4U;

    if (count == 0U)
    {
        menu_captive_show_msg_("No networks");
        return;
    }

    if (s_menu_captive_scan_selected >= count)
    {
        s_menu_captive_scan_selected = (uint16_t)(count - 1U);
    }

    if (s_menu_captive_scan_selected < s_menu_captive_scan_window_start)
    {
        s_menu_captive_scan_window_start = s_menu_captive_scan_selected;
    }
    if (s_menu_captive_scan_selected >= (uint16_t)(s_menu_captive_scan_window_start + visible))
    {
        s_menu_captive_scan_window_start = (uint16_t)(s_menu_captive_scan_selected - visible + 1U);
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(34, 2);
    (void)poom_arduboy_print(F("SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    for (uint8_t i = 0; i < visible; i++)
    {
        const uint16_t idx = (uint16_t)(s_menu_captive_scan_window_start + i);
        const int16_t y = (int16_t)(ROW0_Y + (int16_t)i * ROW_STEP);
        char ssid_line[22];

        ssid_line[0] = '\0';
        if (idx < count)
        {
            wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record((unsigned)idx);
            if ((ap != NULL) && (ap->ssid[0] != 0))
            {
                (void)snprintf(ssid_line, sizeof(ssid_line), "%.18s", (const char *)ap->ssid);
            }
            else
            {
                (void)snprintf(ssid_line, sizeof(ssid_line), "<hidden>");
            }
        }

        poom_arduboy_set_cursor(TEXT_X, y);
        (void)poom_arduboy_print(ssid_line);

        if (idx == s_menu_captive_scan_selected)
        {
            poom_arduboy_fill_rect(1, (int16_t)(y - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_captive_draw_status_(void)
{
    char line_state[22];
    char line_ssid[22];

    (void)snprintf(line_state, sizeof(line_state), "State: %s", s_menu_captive_running ? "RUNNING" : "STOPPED");
    (void)snprintf(line_ssid, sizeof(line_ssid), "SSID: %.15s", (s_menu_captive_ap_ssid[0] != '\0') ? s_menu_captive_ap_ssid : "<default>");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_captive_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_state);

    if (s_menu_captive_running)
    {
        poom_arduboy_fill_rect(1, (int16_t)(ROW0_Y - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
    }

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line_ssid);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(F("Saves: /captive_data"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(F("Portal: /portals"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:STOP"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_captive_draw_scanning_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(34, 2);
    (void)poom_arduboy_print(F("SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(F("Scanning..."));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_captive_exit_(void)
{
    if (s_menu_captive_running)
    {
        poom_wifi_captive_stop();
        s_menu_captive_running = false;
    }

    if (s_menu_captive_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_captive_button_cb_, s_menu_captive_sbus_user);
        s_menu_captive_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Stops the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_captive_stop_to_select_(void)
{
    poom_wifi_captive_stop();
    s_menu_captive_running = false;
    s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
    menu_captive_draw_select_();
}

/**
 * @brief Internal helper for `menu_captive_sd_ssid_available`.
 *
 * @param[in] out_ssid Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return bool
 */
static bool menu_captive_sd_ssid_available_(char *out_ssid, size_t out_len)
{
    esp_err_t status;
    char path[128];
    FILE *f;

    if ((out_ssid != NULL) && (out_len > 0U))
    {
        out_ssid[0] = '\0';
    }

    status = sd_card_mount();
    if (status != ESP_OK)
    {
        return false;
    }

    (void)snprintf(path, sizeof(path), "%s/%s", SD_CARD_PATH, SSID_DATA_PATH);
    path[sizeof(path) - 1U] = '\0';

    f = fopen(path, "r");
    if (f == NULL)
    {
        return false;
    }

    if ((out_ssid != NULL) && (out_len > 0U))
    {
        char line[128];
        if (fgets(line, sizeof(line), f) != NULL)
        {
            char *newline = strpbrk(line, "\r\n");
            if (newline != NULL)
            {
                *newline = '\0';
            }
            char *comma = strchr(line, ',');
            if (comma != NULL)
            {
                *comma = '\0';
            }
            (void)snprintf(out_ssid, out_len, "%.32s", line);
        }
    }

    fclose(f);
    return true;
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] ap_ssid_override Parameter passed to the helper.
 * @return void
 */
static void menu_captive_start_(const char *ap_ssid_override)
{
    if ((ap_ssid_override != NULL) && (ap_ssid_override[0] != '\0'))
    {
        poom_wifi_captive_set_ap_clone(ap_ssid_override, true);
        (void)snprintf(s_menu_captive_ap_ssid, sizeof(s_menu_captive_ap_ssid), "%.32s", ap_ssid_override);
    }
    else
    {
        poom_wifi_captive_set_ap_clone(NULL, false);
    }

    sd_card_begin();
    poom_wifi_captive_start();
    s_menu_captive_running = true;
    s_menu_captive_view = MENU_CAPTIVE_VIEW_STATUS;
    menu_captive_draw_status_();
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_captive_scan_start_(void)
{
    esp_err_t status;
    poom_wifi_scanner_ap_records_t *records;

    menu_captive_draw_scanning_();

    (void)poom_wifi_scanner_clear_ap_records();
    status = poom_wifi_scanner_scan();
    records = poom_wifi_scanner_get_ap_records();
    if ((status != ESP_OK) || (records == NULL) || (records->count == 0U))
    {
        menu_captive_show_msg_((status == ESP_OK) ? "No networks" : "Scan failed");
        return;
    }

    s_menu_captive_scan_selected = 0U;
    s_menu_captive_scan_window_start = 0U;
    s_menu_captive_view = MENU_CAPTIVE_VIEW_SCAN_LIST;
    menu_captive_draw_scan_list_();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_captive_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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
        if (s_menu_captive_view == MENU_CAPTIVE_VIEW_MSG)
        {
            s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
            s_menu_captive_status[0] = '\0';
            menu_captive_draw_select_();
            return;
        }
        if (s_menu_captive_view == MENU_CAPTIVE_VIEW_KEYBOARD)
        {
            s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
            menu_captive_draw_select_();
            return;
        }
        if (s_menu_captive_view == MENU_CAPTIVE_VIEW_SCAN_LIST)
        {
            s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
            menu_captive_draw_select_();
            return;
        }
        if (s_menu_captive_view == MENU_CAPTIVE_VIEW_STATUS)
        {
            poom_wifi_captive_stop();
            s_menu_captive_running = false;
        }
        menu_captive_exit_();
        return;
    }

    if (s_menu_captive_view == MENU_CAPTIVE_VIEW_MSG)
    {
        s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
        s_menu_captive_status[0] = '\0';
        menu_captive_draw_select_();
        return;
    }

    if (s_menu_captive_view == MENU_CAPTIVE_VIEW_KEYBOARD)
    {
        poom_ui_keyboard_action_t action = poom_ui_keyboard_handle_button(&s_menu_captive_keyboard, ev.button);
        if (action == POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            if (s_menu_captive_keyboard.text_len == 0U)
            {
                menu_captive_show_msg_("SSID empty");
                return;
            }

            (void)snprintf(s_menu_captive_ap_ssid,
                           sizeof(s_menu_captive_ap_ssid),
                           "%.32s",
                           s_menu_captive_keyboard_ssid);
            menu_captive_start_(s_menu_captive_ap_ssid);
            return;
        }

        poom_ui_keyboard_draw(&s_menu_captive_keyboard);
        return;
    }

    if (ev.button == BUTTON_A)
    {
        if (s_menu_captive_view == MENU_CAPTIVE_VIEW_SELECT)
        {
            if (s_menu_captive_selected_opt >= (uint8_t)MENU_CAPTIVE_OPT_COUNT)
            {
                s_menu_captive_selected_opt = 0U;
            }

            if (s_menu_captive_selected_opt == (uint8_t)MENU_CAPTIVE_OPT_SD)
            {
                char sd_ssid[33] = {0};
                if (!menu_captive_sd_ssid_available_(sd_ssid, sizeof(sd_ssid)))
                {
                    menu_captive_show_msg_("SD not available");
                    return;
                }

                if (sd_ssid[0] != '\0')
                {
                    (void)snprintf(s_menu_captive_ap_ssid, sizeof(s_menu_captive_ap_ssid), "%.32s", sd_ssid);
                }
                else
                {
                    s_menu_captive_ap_ssid[0] = '\0';
                }

                menu_captive_start_(NULL);
                return;
            }

            if (s_menu_captive_selected_opt == (uint8_t)MENU_CAPTIVE_OPT_KEYBOARD)
            {
                (void)memset(s_menu_captive_keyboard_ssid, 0, sizeof(s_menu_captive_keyboard_ssid));
                poom_ui_keyboard_init(&s_menu_captive_keyboard,
                                      s_menu_captive_keyboard_ssid,
                                      sizeof(s_menu_captive_keyboard_ssid),
                                      "SSID");
                s_menu_captive_view = MENU_CAPTIVE_VIEW_KEYBOARD;
                poom_ui_keyboard_draw(&s_menu_captive_keyboard);
                return;
            }

            if (s_menu_captive_selected_opt == (uint8_t)MENU_CAPTIVE_OPT_SCAN)
            {
                menu_captive_scan_start_();
                return;
            }

            if (s_menu_captive_selected_opt == (uint8_t)MENU_CAPTIVE_OPT_COFFEE)
            {
                menu_captive_start_("coffee");
                return;
            }
        }
        else if (s_menu_captive_view == MENU_CAPTIVE_VIEW_STATUS)
        {
            menu_captive_stop_to_select_();
            return;
        }
        else if (s_menu_captive_view == MENU_CAPTIVE_VIEW_SCAN_LIST)
        {
            wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record((unsigned)s_menu_captive_scan_selected);
            if ((ap == NULL) || (ap->ssid[0] == 0))
            {
                menu_captive_show_msg_("Hidden SSID");
                return;
            }

            (void)snprintf(s_menu_captive_ap_ssid, sizeof(s_menu_captive_ap_ssid), "%.32s", (const char *)ap->ssid);
            menu_captive_start_(s_menu_captive_ap_ssid);
            return;
        }
    }

    if (s_menu_captive_view == MENU_CAPTIVE_VIEW_SELECT)
    {
        if ((ev.button == BUTTON_UP) || (ev.button == BUTTON_LEFT))
        {
            if (s_menu_captive_selected_opt > 0U)
            {
                s_menu_captive_selected_opt--;
            }
            menu_captive_draw_select_();
        }
        else if ((ev.button == BUTTON_DOWN) || (ev.button == BUTTON_RIGHT))
        {
            if (s_menu_captive_selected_opt < (uint8_t)(MENU_CAPTIVE_OPT_COUNT - 1U))
            {
                s_menu_captive_selected_opt++;
            }
            menu_captive_draw_select_();
        }
        return;
    }

    if (s_menu_captive_view == MENU_CAPTIVE_VIEW_SCAN_LIST)
    {
        poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
        const uint16_t count = (records != NULL) ? records->count : 0U;

        if ((ev.button == BUTTON_UP) || (ev.button == BUTTON_LEFT))
        {
            if (s_menu_captive_scan_selected > 0U)
            {
                s_menu_captive_scan_selected--;
                menu_captive_draw_scan_list_();
            }
        }
        else if ((ev.button == BUTTON_DOWN) || (ev.button == BUTTON_RIGHT))
        {
            if ((count > 0U) && (s_menu_captive_scan_selected < (uint16_t)(count - 1U)))
            {
                s_menu_captive_scan_selected++;
                menu_captive_draw_scan_list_();
            }
        }
        return;
    }

    if (s_menu_captive_view == MENU_CAPTIVE_VIEW_STATUS)
    {
        menu_captive_draw_status_();
        return;
    }
}

void menu_captive_display(void)
{
    s_menu_captive_running = false;
    s_menu_captive_view = MENU_CAPTIVE_VIEW_SELECT;
    s_menu_captive_selected_opt = 0U;
    s_menu_captive_scan_selected = 0U;
    s_menu_captive_scan_window_start = 0U;
    s_menu_captive_status[0] = '\0';
    s_menu_captive_ap_ssid[0] = '\0';

    if (!s_menu_captive_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_captive_button_cb_, s_menu_captive_sbus_user))
        {
            s_menu_captive_buttons_subscribed = true;
        }
        else
        {
            menu_captive_draw_select_();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    menu_captive_draw_select_();
}
