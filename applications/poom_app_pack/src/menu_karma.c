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
#include "poom_wifi_captive.h"
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
} menu_karma_button_msg_t;

typedef enum
{
    MENU_KARMA_VIEW_STATUS = 0,
    MENU_KARMA_VIEW_CLIENTS,
    MENU_KARMA_VIEW_CLIENT_INFO,
} menu_karma_view_t;

static bool s_menu_karma_active = false;
static bool s_menu_karma_buttons_subscribed = false;
static bool s_menu_karma_exit_requested = false;
static bool s_menu_karma_restart_requested = false;
static TaskHandle_t s_menu_karma_task = NULL;
static char s_menu_karma_sbus_user[] = "menu_karma";
static char s_menu_karma_status[22] = "Starting...";
static char s_menu_karma_active_ssid[33] = {0};
static menu_karma_view_t s_menu_karma_view = MENU_KARMA_VIEW_STATUS;
static unsigned s_menu_karma_client_selected = 0U;
static unsigned s_menu_karma_client_scroll = 0U;

static esp_err_t menu_karma_exit_(void);
static void menu_karma_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_karma_sync_runtime_status_(void);
static esp_err_t menu_karma_draw_status_(void);
static esp_err_t menu_karma_draw_clients_(void);
static esp_err_t menu_karma_draw_client_info_(void);

static const char *menu_karma_portal_label_(const char *path)
{
    const char *slash;
    if((path == NULL) || (path[0] == '\0'))
    {
        return CAPTIVE_PORTAL_DEFAULT_NAME;
    }
    slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

static size_t menu_karma_get_clients_(poom_wifi_captive_client_t *clients, size_t capacity)
{
    size_t count = 0U;
    if((clients == NULL) || (capacity == 0U))
    {
        return 0U;
    }
    if(poom_wifi_captive_get_clients(clients, capacity, &count) != ESP_OK)
    {
        return 0U;
    }
    return count;
}

static void menu_karma_format_client_label_(char *out, size_t out_len, const poom_wifi_captive_client_t *client)
{
    const char *label;
    const char *last;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if(client == NULL)
    {
        return;
    }

    label = poom_wifi_captive_client_label(client);
    last = (client->ip[0] != '\0') ? strrchr(client->ip, '.') : NULL;
    if((client->device_type[0] != '\0') && (last != NULL) && (last[1] != '\0'))
    {
        const char *p = last - 1;
        while((p > client->ip) && (*p != '.'))
        {
            --p;
        }
        (void)snprintf(out, out_len, "%.11s %s", client->device_type, (*p == '.') ? (p + 1) : client->ip);
    }
    else
    {
        (void)snprintf(out, out_len, "%.17s", label);
    }
}

static void menu_karma_draw_header_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor((int16_t)((ARDUBOY_WIDTH - ((int)strlen(title) * 6)) / 2), 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

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
static esp_err_t menu_karma_draw_status_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];
    char portal[CAPTIVE_PORTAL_MAX_PATH_LEN];
    char ssid_text[MENU_KARMA_TEXT_MAX_CHARS + 1U];
    char status_text[MENU_KARMA_STATUS_MAX_CHARS + 1U];

    if(!poom_wifi_karma_get_active_ssid(s_menu_karma_active_ssid, sizeof(s_menu_karma_active_ssid)))
    {
        s_menu_karma_active_ssid[0] = '\0';
    }

    menu_karma_sync_runtime_status_();
    menu_karma_format_ssid_line_(ssid_text, sizeof(ssid_text), s_menu_karma_active_ssid);
    menu_karma_format_status_line_(status_text, sizeof(status_text), s_menu_karma_status);

    if(poom_wifi_captive_load_saved_portal(portal, sizeof(portal)) != ESP_OK)
    {
        (void)snprintf(portal, sizeof(portal), "%s", CAPTIVE_PORTAL_DEFAULT_NAME);
    }

    (void)snprintf(line1, sizeof(line1), "SSID: %s", ssid_text);
    (void)snprintf(line2, sizeof(line2), "STATUS: %s", status_text);
    (void)snprintf(line3, sizeof(line3), "Portal:%.13s", menu_karma_portal_label_(portal));

    menu_karma_draw_header_("POOM KARMA");
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(4, 14);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, 26);
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(4, 38);
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((poom_wifi_karma_get_state() == POOM_WIFI_KARMA_STATE_CAPTIVE) ? F("A:CLIENTS") : F("A:SCAN"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

static esp_err_t menu_karma_draw_clients_(void)
{
    poom_wifi_captive_client_t clients[POOM_WIFI_CAPTIVE_MAX_CLIENTS];
    size_t count = menu_karma_get_clients_(clients, POOM_WIFI_CAPTIVE_MAX_CLIENTS);
    char line[22];

    if(s_menu_karma_client_selected >= count)
    {
        s_menu_karma_client_selected = (count > 0U) ? (unsigned)(count - 1U) : 0U;
    }
    if(s_menu_karma_client_selected < s_menu_karma_client_scroll)
    {
        s_menu_karma_client_scroll = s_menu_karma_client_selected;
    }
    if(s_menu_karma_client_selected >= (s_menu_karma_client_scroll + 4U))
    {
        s_menu_karma_client_scroll = s_menu_karma_client_selected - 3U;
    }
    if((count <= 4U) || (s_menu_karma_client_scroll > (unsigned)(count - 4U)))
    {
        s_menu_karma_client_scroll = (count > 4U) ? (unsigned)(count - 4U) : 0U;
    }

    (void)snprintf(line, sizeof(line), "CLIENTS %u", (unsigned)count);
    menu_karma_draw_header_(line);
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    if(count == 0U)
    {
        poom_arduboy_set_cursor(18, 30);
        (void)poom_arduboy_print(F("NO CLIENTS"));
    }
    else
    {
        for(unsigned row = 0U; row < 4U; row++)
        {
            unsigned idx = s_menu_karma_client_scroll + row;
            int16_t y = (int16_t)(14 + (int)row * 10);
            if(idx >= count)
            {
                break;
            }
            menu_karma_format_client_label_(line, sizeof(line), &clients[idx]);
            if(clients[idx].online)
            {
                poom_arduboy_fill_rect(4, y + 2, 3, 3, WHITE);
            }
            poom_arduboy_set_cursor(12, y);
            (void)poom_arduboy_print(line);
            if(idx == s_menu_karma_client_selected)
            {
                poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
            }
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((count > 0U) ? F("A:INFO") : F(""));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
    return ESP_OK;
}

static esp_err_t menu_karma_draw_client_info_(void)
{
    poom_wifi_captive_client_t clients[POOM_WIFI_CAPTIVE_MAX_CLIENTS];
    size_t count = menu_karma_get_clients_(clients, POOM_WIFI_CAPTIVE_MAX_CLIENTS);
    poom_wifi_captive_client_t *client = (s_menu_karma_client_selected < count) ? &clients[s_menu_karma_client_selected] : NULL;
    char line[22];

    menu_karma_draw_header_("CLIENT INFO");

    if(client == NULL)
    {
        poom_arduboy_set_cursor(18, 30);
        (void)poom_arduboy_print(F("CLIENT GONE"));
    }
    else
    {
        (void)snprintf(line, sizeof(line), "%.21s", poom_wifi_captive_client_label(client));
        poom_arduboy_set_cursor(0, 14);
        (void)poom_arduboy_print(line);
        (void)snprintf(line, sizeof(line), "IP:%s", client->ip[0] ? client->ip : "---");
        poom_arduboy_set_cursor(0, 22);
        (void)poom_arduboy_print(line);
        (void)snprintf(line, sizeof(line), "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                       client->mac[0], client->mac[1], client->mac[2], client->mac[3], client->mac[4], client->mac[5]);
        poom_arduboy_set_cursor(0, 30);
        (void)poom_arduboy_print(line);
        if(client->rssi_known)
        {
            (void)snprintf(line, sizeof(line), "RSSI:%d dBm", (int)client->rssi);
        }
        else
        {
            (void)snprintf(line, sizeof(line), "RSSI:---");
        }
        poom_arduboy_set_cursor(0, 38);
        (void)poom_arduboy_print(line);
        (void)snprintf(line, sizeof(line), "%.10s%s%s", client->device_type,
                       client->device_type[0] ? " " : "",
                       client->online ? "ONLINE" : "OFFLINE");
        poom_arduboy_set_cursor(0, 46);
        (void)poom_arduboy_print(line);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
    return ESP_OK;
}

static esp_err_t menu_karma_draw_(void)
{
    if(s_menu_karma_view == MENU_KARMA_VIEW_CLIENTS)
    {
        return menu_karma_draw_clients_();
    }
    if(s_menu_karma_view == MENU_KARMA_VIEW_CLIENT_INFO)
    {
        return menu_karma_draw_client_info_();
    }
    return menu_karma_draw_status_();
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
            s_menu_karma_view = MENU_KARMA_VIEW_STATUS;
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
    s_menu_karma_view = MENU_KARMA_VIEW_STATUS;
    s_menu_karma_client_selected = 0U;
    s_menu_karma_client_scroll = 0U;

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
        if(s_menu_karma_view == MENU_KARMA_VIEW_CLIENT_INFO)
        {
            s_menu_karma_view = MENU_KARMA_VIEW_CLIENTS;
            return;
        }
        if(s_menu_karma_view == MENU_KARMA_VIEW_CLIENTS)
        {
            s_menu_karma_view = MENU_KARMA_VIEW_STATUS;
            return;
        }
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

    if(s_menu_karma_view == MENU_KARMA_VIEW_CLIENTS)
    {
        poom_wifi_captive_client_t clients[POOM_WIFI_CAPTIVE_MAX_CLIENTS];
        size_t count = menu_karma_get_clients_(clients, POOM_WIFI_CAPTIVE_MAX_CLIENTS);
        if((button_msg.button == BTN_UP) && (s_menu_karma_client_selected > 0U))
        {
            s_menu_karma_client_selected--;
        }
        else if((button_msg.button == BTN_DOWN) && ((s_menu_karma_client_selected + 1U) < count))
        {
            s_menu_karma_client_selected++;
        }
        else if((button_msg.button == BTN_A) && (count > 0U))
        {
            s_menu_karma_view = MENU_KARMA_VIEW_CLIENT_INFO;
        }
        return;
    }

    if(button_msg.button == BTN_A)
    {
        if(poom_wifi_karma_get_state() == POOM_WIFI_KARMA_STATE_CAPTIVE)
        {
            s_menu_karma_client_selected = 0U;
            s_menu_karma_client_scroll = 0U;
            s_menu_karma_view = MENU_KARMA_VIEW_CLIENTS;
        }
        else
        {
            s_menu_karma_restart_requested = true;
        }
    }
}

void menu_karma_init(void)
{
    s_menu_karma_active = true;
    s_menu_karma_exit_requested = false;
    s_menu_karma_restart_requested = false;
    s_menu_karma_active_ssid[0] = '\0';
    s_menu_karma_view = MENU_KARMA_VIEW_STATUS;
    s_menu_karma_client_selected = 0U;
    s_menu_karma_client_scroll = 0U;

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
