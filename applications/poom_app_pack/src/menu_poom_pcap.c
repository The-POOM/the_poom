// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_pcap.h"

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
#include "poom_pcap_manager.h"
#include "poom_sbus.h"
#include "poom_wifi_ctrl.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_POOM_PCAP_REFRESH_MS (250U)
#define MENU_POOM_PCAP_STACK (3584U)
#define MENU_POOM_PCAP_PRIO (4U)

#define MENU_POOM_PCAP_ZB_CH_MIN (11U)
#define MENU_POOM_PCAP_ZB_CH_MAX (26U)
#define MENU_POOM_PCAP_ZB_CH_DEFAULT (11U)
#define MENU_POOM_PCAP_ZB_CH_HOP (0U)
#define MENU_POOM_PCAP_WIFI_CH_MIN (POOM_WIFI_CTRL_WIFI_CHANNEL_MIN)
#define MENU_POOM_PCAP_WIFI_CH_MAX (POOM_WIFI_CTRL_WIFI_CHANNEL_MAX)
#define MENU_POOM_PCAP_WIFI_CH_DEFAULT (6U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)
#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)
#define LIST_Y0 (16)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_POOM_PCAP_SCREEN_SELECT = 0,
    MENU_POOM_PCAP_SCREEN_ZIGBEE_CH,
    MENU_POOM_PCAP_SCREEN_WIFI_TYPE,
    MENU_POOM_PCAP_SCREEN_WIFI_CH,
    MENU_POOM_PCAP_SCREEN_RUNNING,
} menu_poom_pcap_screen_t;

typedef enum
{
    MENU_POOM_PCAP_MODE_ZIGBEE = 0,
    MENU_POOM_PCAP_MODE_WIFI,
    MENU_POOM_PCAP_MODE_BLE,
    MENU_POOM_PCAP_MODE_COUNT,
} menu_poom_pcap_mode_t;

static bool s_menu_poom_pcap_active = false;
static bool s_menu_poom_pcap_running = false;
static bool s_menu_poom_pcap_buttons_subscribed = false;
static bool s_menu_poom_pcap_exit_requested = false;
static bool s_menu_poom_pcap_input_dirty = false;
static TaskHandle_t s_menu_poom_pcap_task = NULL;
static char s_menu_poom_pcap_sbus_user[] = "menu_poom_pcap";

static menu_poom_pcap_screen_t s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_SELECT;
static menu_poom_pcap_mode_t s_menu_poom_pcap_mode = MENU_POOM_PCAP_MODE_ZIGBEE;
static uint8_t s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_DEFAULT;
static uint8_t s_menu_poom_pcap_wifi_channel = MENU_POOM_PCAP_WIFI_CH_DEFAULT;
static poom_pcap_wifi_capture_t s_menu_poom_pcap_wifi_capture = POOM_PCAP_WIFI_CAPTURE_PROBE;

static void menu_poom_pcap_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static esp_err_t menu_poom_pcap_exit_(void);
static void menu_poom_pcap_request_render_from_input_(void);

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_poom_pcap_request_render_from_input_(void)
{
    s_menu_poom_pcap_input_dirty = true;
    if (s_menu_poom_pcap_task != NULL)
    {
        (void)xTaskNotifyGive(s_menu_poom_pcap_task);
    }
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_poom_pcap_draw_header_(void)
{
    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("PCAP SNIFFER"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] mode Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_poom_pcap_mode_label_(menu_poom_pcap_mode_t mode)
{
    switch (mode)
    {
        case MENU_POOM_PCAP_MODE_ZIGBEE:
            return "ZIGBEE";
        case MENU_POOM_PCAP_MODE_WIFI:
            return "WIFI";
        case MENU_POOM_PCAP_MODE_BLE:
            return "BLE";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Returns the display label for the current state.
 *
 * @return const char *
 */
static const char *menu_poom_pcap_output_label_(void)
{
    const char *path = poom_pcap_manager_get_file_path();
    return (path != NULL) ? "SD" : "UART";
}

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] capture Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_poom_pcap_wifi_capture_label_(poom_pcap_wifi_capture_t capture)
{
    switch (capture)
    {
        case POOM_PCAP_WIFI_CAPTURE_PROBE:
            return "PROBE";
        case POOM_PCAP_WIFI_CAPTURE_DEAUTH:
            return "DEAUTH";
        case POOM_PCAP_WIFI_CAPTURE_BEACON:
            return "BEACON";
        case POOM_PCAP_WIFI_CAPTURE_RAW:
            return "RAW";
        case POOM_PCAP_WIFI_CAPTURE_EAPOL:
            return "EAPOL";
        case POOM_PCAP_WIFI_CAPTURE_WPS:
            return "WPS";
        default:
            return "RAW";
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_pcap_draw_select_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_pcap_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    for (int i = 0; i < (int)MENU_POOM_PCAP_MODE_COUNT; i++)
    {
        const int16_t y = (int16_t)(LIST_Y0 + i * ROW_STEP);
        const char *label = menu_poom_pcap_mode_label_((menu_poom_pcap_mode_t)i);

        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(label);

        if (i == (int)s_menu_poom_pcap_mode)
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
static void menu_poom_pcap_draw_wifi_type_(void)
{
    static const poom_pcap_wifi_capture_t order[] = {
        POOM_PCAP_WIFI_CAPTURE_PROBE,
        POOM_PCAP_WIFI_CAPTURE_DEAUTH,
        POOM_PCAP_WIFI_CAPTURE_BEACON,
        POOM_PCAP_WIFI_CAPTURE_RAW,
        POOM_PCAP_WIFI_CAPTURE_EAPOL,
        POOM_PCAP_WIFI_CAPTURE_WPS,
    };

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_pcap_draw_header_();
    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    int sel_idx = 0;
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++)
    {
        if (order[i] == s_menu_poom_pcap_wifi_capture)
        {
            sel_idx = i;
            break;
        }
    }

    const int item_count = (int)(sizeof(order) / sizeof(order[0]));
    const int visible_rows = 3;
    const int max_scroll = (item_count > visible_rows) ? (item_count - visible_rows) : 0;

    int first_idx = 0;
    if (sel_idx >= visible_rows)
    {
        first_idx = sel_idx - (visible_rows - 1);
    }
    if (first_idx > max_scroll)
    {
        first_idx = max_scroll;
    }
    if (first_idx < 0)
    {
        first_idx = 0;
    }

    for (int row = 0; row < visible_rows; row++)
    {
        const int i = first_idx + row;
        if (i >= item_count)
        {
            break;
        }

        const int16_t x = 4;
        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        const char *label = menu_poom_pcap_wifi_capture_label_(order[i]);

        poom_arduboy_set_cursor(x, y);
        (void)poom_arduboy_print(label);

        if (i == sel_idx)
        {
            poom_arduboy_fill_rect(1, (int16_t)(y - 1), ARDUBOY_WIDTH - 2, ROW_HILITE_H, INVERT);
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
static void menu_poom_pcap_draw_zigbee_ch_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];

    if (s_menu_poom_pcap_zigbee_channel == MENU_POOM_PCAP_ZB_CH_HOP)
    {
        (void)snprintf(line1, sizeof(line1), "Zigbee CH: HOP");
    }
    else
    {
        (void)snprintf(line1, sizeof(line1), "Zigbee CH: %u", (unsigned)s_menu_poom_pcap_zigbee_channel);
    }
    (void)snprintf(line2, sizeof(line2), "Up/Down: ch/hop");
    (void)snprintf(line3, sizeof(line3), "A:Start B:Back");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_pcap_draw_header_();

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
static void menu_poom_pcap_draw_wifi_ch_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];

    (void)snprintf(line1, sizeof(line1), "WiFi %.6s CH:%u",
                   menu_poom_pcap_wifi_capture_label_(s_menu_poom_pcap_wifi_capture),
                   (unsigned)s_menu_poom_pcap_wifi_channel);
    (void)snprintf(line2, sizeof(line2), "Up/Down: ch");
    (void)snprintf(line3, sizeof(line3), "A:Start B:Back");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_pcap_draw_header_();

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
static void menu_poom_pcap_draw_running_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];
    const char *path = poom_pcap_manager_get_file_path();

    (void)snprintf(line1, sizeof(line1), "Mode: %.10s", menu_poom_pcap_mode_label_(s_menu_poom_pcap_mode));
    if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_ZIGBEE)
    {
        if (s_menu_poom_pcap_zigbee_channel == MENU_POOM_PCAP_ZB_CH_HOP)
        {
            (void)snprintf(line2, sizeof(line2), "Out: %.4s CH:HOP", menu_poom_pcap_output_label_());
        }
        else
        {
            (void)snprintf(line2, sizeof(line2), "Out: %.4s CH:%u", menu_poom_pcap_output_label_(), (unsigned)s_menu_poom_pcap_zigbee_channel);
        }
    }
    else if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_WIFI)
    {
        char ch[4];
        (void)snprintf(ch, sizeof(ch), "%hhu", s_menu_poom_pcap_wifi_channel);
        (void)snprintf(line2, sizeof(line2), "O:%.4s %.6s C:%.3s",
                       menu_poom_pcap_output_label_(),
                       menu_poom_pcap_wifi_capture_label_(s_menu_poom_pcap_wifi_capture),
                       ch);
    }
    else
    {
        (void)snprintf(line2, sizeof(line2), "Out: %.4s", menu_poom_pcap_output_label_());
    }

    if (path != NULL)
    {
        size_t n = strlen(path);
        const char *tail = (n > 18U) ? &path[n - 18U] : path;
        (void)snprintf(line3, sizeof(line3), "%.18s", tail);
    }
    else
    {
        (void)snprintf(line3, sizeof(line3), "UART stream");
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_pcap_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line1);

    const int16_t y_state = (int16_t)(ROW0_Y + ROW_STEP);
    poom_arduboy_set_cursor(TEXT_X, y_state);
    (void)poom_arduboy_print(line2);
    poom_arduboy_fill_rect(1, (int16_t)(y_state - 1), ARDUBOY_WIDTH - 2, 9, INVERT);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("B:STOP"));
    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_poom_pcap_render_(void)
{
    if (!s_menu_poom_pcap_active)
    {
        return;
    }

    if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_SELECT)
    {
        menu_poom_pcap_draw_select_();
    }
    else if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_ZIGBEE_CH)
    {
        menu_poom_pcap_draw_zigbee_ch_();
    }
    else if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_WIFI_TYPE)
    {
        menu_poom_pcap_draw_wifi_type_();
    }
    else if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_WIFI_CH)
    {
        menu_poom_pcap_draw_wifi_ch_();
    }
    else
    {
        menu_poom_pcap_draw_running_();
    }
}

/**
 * @brief Stops the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_poom_pcap_stop_capture_(void)
{
    if (!s_menu_poom_pcap_running)
    {
        return;
    }

    (void)poom_pcap_manager_sniffer_stop();

    s_menu_poom_pcap_running = false;
    menu_poom_pcap_request_render_from_input_();
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_pcap_start_capture_(void)
{
    esp_err_t ret;

    if (s_menu_poom_pcap_running)
    {
        return ESP_OK;
    }

    if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_ZIGBEE)
    {
        const bool hop = (s_menu_poom_pcap_zigbee_channel == MENU_POOM_PCAP_ZB_CH_HOP);
        ret = poom_pcap_manager_sniffer_start_zigbee(s_menu_poom_pcap_zigbee_channel, hop, 0U);
    }
    else if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_WIFI)
    {
        uint32_t filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;

        if ((s_menu_poom_pcap_wifi_capture == POOM_PCAP_WIFI_CAPTURE_PROBE) ||
            (s_menu_poom_pcap_wifi_capture == POOM_PCAP_WIFI_CAPTURE_DEAUTH) ||
            (s_menu_poom_pcap_wifi_capture == POOM_PCAP_WIFI_CAPTURE_BEACON))
        {
            filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        }
        else if ((s_menu_poom_pcap_wifi_capture == POOM_PCAP_WIFI_CAPTURE_EAPOL) ||
                 (s_menu_poom_pcap_wifi_capture == POOM_PCAP_WIFI_CAPTURE_WPS))
        {
            filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
        }

        if ((s_menu_poom_pcap_wifi_channel < MENU_POOM_PCAP_WIFI_CH_MIN) || (s_menu_poom_pcap_wifi_channel > MENU_POOM_PCAP_WIFI_CH_MAX))
        {
            s_menu_poom_pcap_wifi_channel = MENU_POOM_PCAP_WIFI_CH_DEFAULT;
        }
        ret = poom_pcap_manager_sniffer_start_wifi_capture(s_menu_poom_pcap_wifi_channel, filter_mask, s_menu_poom_pcap_wifi_capture);
    }
    else
    {
        ret = poom_pcap_manager_sniffer_start_ble();
    }

    if (ret != ESP_OK)
    {
        return ret;
    }

    s_menu_poom_pcap_running = true;
    s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_RUNNING;
    menu_poom_pcap_request_render_from_input_();
    return ESP_OK;
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_pcap_task_(void *task_arg)
{
    (void)task_arg;

    while (s_menu_poom_pcap_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MENU_POOM_PCAP_REFRESH_MS));

        if (s_menu_poom_pcap_exit_requested)
        {
            (void)menu_poom_pcap_exit_();
            break;
        }

        if (s_menu_poom_pcap_input_dirty)
        {
            menu_poom_pcap_render_();
            s_menu_poom_pcap_input_dirty = false;
        }
    }

    s_menu_poom_pcap_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_pcap_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_poom_pcap_active = false;
    s_menu_poom_pcap_exit_requested = false;

    menu_poom_pcap_stop_capture_();

    if (s_menu_poom_pcap_task != NULL)
    {
        if (s_menu_poom_pcap_task != current_task)
        {
            TaskHandle_t ui_task = s_menu_poom_pcap_task;
            s_menu_poom_pcap_task = NULL;
            vTaskDelete(ui_task);
        }
        else
        {
            s_menu_poom_pcap_task = NULL;
        }
    }

    if (s_menu_poom_pcap_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_poom_pcap_button_cb_, s_menu_poom_pcap_sbus_user);
        s_menu_poom_pcap_buttons_subscribed = false;
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
static void menu_poom_pcap_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t button_msg;
    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if (button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_RUNNING)
    {
        if (button_msg.button == BUTTON_B)
        {
            menu_poom_pcap_stop_capture_();
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_SELECT;
            menu_poom_pcap_request_render_from_input_();
        }
        return;
    }

    if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_ZIGBEE_CH)
    {
        if (button_msg.button == BUTTON_B)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_SELECT;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_UP)
        {
            if (s_menu_poom_pcap_zigbee_channel == MENU_POOM_PCAP_ZB_CH_HOP)
            {
                s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_MAX;
            }
            else if (s_menu_poom_pcap_zigbee_channel <= MENU_POOM_PCAP_ZB_CH_MIN)
            {
                s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_HOP;
            }
            else
            {
                s_menu_poom_pcap_zigbee_channel--;
            }

            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_DOWN)
        {
            if (s_menu_poom_pcap_zigbee_channel == MENU_POOM_PCAP_ZB_CH_HOP)
            {
                s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_MIN;
            }
            else if (s_menu_poom_pcap_zigbee_channel >= MENU_POOM_PCAP_ZB_CH_MAX)
            {
                s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_HOP;
            }
            else
            {
                s_menu_poom_pcap_zigbee_channel++;
            }
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_A)
        {
            (void)menu_poom_pcap_start_capture_();
            return;
        }
        return;
    }

    if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_WIFI_CH)
    {
        if (button_msg.button == BUTTON_B)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_WIFI_TYPE;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_UP)
        {
            if (s_menu_poom_pcap_wifi_channel <= MENU_POOM_PCAP_WIFI_CH_MIN)
            {
                s_menu_poom_pcap_wifi_channel = MENU_POOM_PCAP_WIFI_CH_MAX;
            }
            else
            {
                s_menu_poom_pcap_wifi_channel--;
            }
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_DOWN)
        {
            if (s_menu_poom_pcap_wifi_channel >= MENU_POOM_PCAP_WIFI_CH_MAX)
            {
                s_menu_poom_pcap_wifi_channel = MENU_POOM_PCAP_WIFI_CH_MIN;
            }
            else
            {
                s_menu_poom_pcap_wifi_channel++;
            }
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_A)
        {
            (void)menu_poom_pcap_start_capture_();
            return;
        }
        return;
    }

    if (s_menu_poom_pcap_screen == MENU_POOM_PCAP_SCREEN_WIFI_TYPE)
    {
        static const poom_pcap_wifi_capture_t order[] = {
            POOM_PCAP_WIFI_CAPTURE_PROBE,
            POOM_PCAP_WIFI_CAPTURE_DEAUTH,
            POOM_PCAP_WIFI_CAPTURE_BEACON,
            POOM_PCAP_WIFI_CAPTURE_RAW,
            POOM_PCAP_WIFI_CAPTURE_EAPOL,
            POOM_PCAP_WIFI_CAPTURE_WPS,
        };

        int sel_idx = 0;
        for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++)
        {
            if (order[i] == s_menu_poom_pcap_wifi_capture)
            {
                sel_idx = i;
                break;
            }
        }

        if (button_msg.button == BUTTON_B)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_SELECT;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_UP)
        {
            sel_idx = (sel_idx == 0) ? ((int)(sizeof(order) / sizeof(order[0])) - 1) : (sel_idx - 1);
            s_menu_poom_pcap_wifi_capture = order[sel_idx];
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_DOWN)
        {
            sel_idx = (sel_idx + 1) % (int)(sizeof(order) / sizeof(order[0]));
            s_menu_poom_pcap_wifi_capture = order[sel_idx];
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (button_msg.button == BUTTON_A)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_WIFI_CH;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        return;
    }

    if (button_msg.button == BUTTON_B)
    {
        if (s_menu_poom_pcap_task == NULL)
        {
            (void)menu_poom_pcap_exit_();
        }
        else
        {
            s_menu_poom_pcap_exit_requested = true;
            (void)xTaskNotifyGive(s_menu_poom_pcap_task);
        }
        return;
    }
    if (button_msg.button == BUTTON_UP)
    {
        if (s_menu_poom_pcap_mode == 0)
        {
            s_menu_poom_pcap_mode = (menu_poom_pcap_mode_t)(MENU_POOM_PCAP_MODE_COUNT - 1);
        }
        else
        {
            s_menu_poom_pcap_mode = (menu_poom_pcap_mode_t)((int)s_menu_poom_pcap_mode - 1);
        }
        menu_poom_pcap_request_render_from_input_();
        return;
    }
    if (button_msg.button == BUTTON_DOWN)
    {
        s_menu_poom_pcap_mode = (menu_poom_pcap_mode_t)(((int)s_menu_poom_pcap_mode + 1) % (int)MENU_POOM_PCAP_MODE_COUNT);
        menu_poom_pcap_request_render_from_input_();
        return;
    }
    if (button_msg.button == BUTTON_A)
    {
        if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_ZIGBEE)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_ZIGBEE_CH;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        if (s_menu_poom_pcap_mode == MENU_POOM_PCAP_MODE_WIFI)
        {
            s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_WIFI_TYPE;
            menu_poom_pcap_request_render_from_input_();
            return;
        }
        (void)menu_poom_pcap_start_capture_();
        return;
    }
}

void menu_poom_pcap_show(void)
{
    s_menu_poom_pcap_active = true;
    s_menu_poom_pcap_exit_requested = false;
    s_menu_poom_pcap_running = false;
    s_menu_poom_pcap_screen = MENU_POOM_PCAP_SCREEN_SELECT;
    s_menu_poom_pcap_input_dirty = false;

    s_menu_poom_pcap_mode = MENU_POOM_PCAP_MODE_ZIGBEE;
    s_menu_poom_pcap_zigbee_channel = MENU_POOM_PCAP_ZB_CH_DEFAULT;
    s_menu_poom_pcap_wifi_channel = MENU_POOM_PCAP_WIFI_CH_DEFAULT;
    s_menu_poom_pcap_wifi_capture = POOM_PCAP_WIFI_CAPTURE_PROBE;

    if (!s_menu_poom_pcap_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_poom_pcap_button_cb_, s_menu_poom_pcap_sbus_user))
        {
            s_menu_poom_pcap_buttons_subscribed = true;
        }
        else
        {
            s_menu_poom_pcap_active = false;
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if (s_menu_poom_pcap_task == NULL)
    {
        (void)xTaskCreate(menu_poom_pcap_task_,
                          "menu_pcap",
                          MENU_POOM_PCAP_STACK,
                          NULL,
                          MENU_POOM_PCAP_PRIO,
                          &s_menu_poom_pcap_task);
    }

    menu_poom_pcap_request_render_from_input_();
}
