// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_droneid.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_drone.h"
#include "poom_sbus.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_DRONEID_REFRESH_MS (200U)
#define MENU_DRONEID_STACK (3584U)
#define MENU_DRONEID_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (42)
#define LIST_Y0 (14)
#define ROW_STEP (8)
#define ROW_HILITE_H (8)

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

typedef enum
{
    MENU_DRONEID_ITEM_RUN = 0,
    MENU_DRONEID_ITEM_WIFI,
    MENU_DRONEID_ITEM_BLE,
    MENU_DRONEID_ITEM_CLI,
    MENU_DRONEID_ITEM_PCAP,
    MENU_DRONEID_ITEM_COUNT,
} menu_droneid_item_t;

typedef enum
{
    MENU_DRONEID_SCREEN_CONFIG = 0,
    MENU_DRONEID_SCREEN_RUNNING,
} menu_droneid_screen_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
} menu_droneid_btn_msg_t;

static bool s_menu_active = false;
static bool s_buttons_subscribed = false;
static TaskHandle_t s_menu_task = NULL;
static QueueHandle_t s_btn_q = NULL;
static char s_sbus_user[] = "menu_poom_droneid";

static int s_selected = 0;
static char s_status[18] = "READY";
static menu_droneid_screen_t s_screen = MENU_DRONEID_SCREEN_CONFIG;

static poom_drone_config_t s_cfg;
static bool s_sd_available = false;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_report_count = 0;
static int8_t s_last_rssi = 0;
static uint16_t s_last_channel = 0;
static uint8_t s_last_mac[6] = {0};
static char s_last_id[ODID_ID_SIZE + 1] = {0};
static bool s_has_last = false;

static void menu_btn_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_task_(void *arg);
static void menu_exit_(void);

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] report Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_report_cb_(const poom_drone_uav_data_t *report, void *user_ctx)
{
    (void)user_ctx;
    if (report == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);
    s_report_count++;
    s_last_rssi = report->rssi;
    s_last_channel = report->channel;
    memcpy(s_last_mac, report->mac, sizeof(s_last_mac));
    snprintf(s_last_id, sizeof(s_last_id), "%s", report->uav_id);
    s_has_last = true;
    portEXIT_CRITICAL(&s_stats_mux);
}

/**
 * @brief Internal helper for `menu_sd_probe`.
 *
 * @return bool
 */
static bool menu_sd_probe_(void)
{
    if (sd_card_is_mounted())
    {
        return true;
    }
    return (sd_card_mount() == ESP_OK);
}

/**
 * @brief Internal helper for `cfg_wifi`.
 *
 * @param[in] cfg Parameter passed to the helper.
 * @return bool
 */
static bool cfg_wifi_(const poom_drone_config_t *cfg)
{
    return (cfg != NULL) && ((cfg->scan_mask & POOM_DRONE_SCAN_WIFI) != 0U);
}

/**
 * @brief Internal helper for `cfg_ble`.
 *
 * @param[in] cfg Parameter passed to the helper.
 * @return bool
 */
static bool cfg_ble_(const poom_drone_config_t *cfg)
{
    return (cfg != NULL) && ((cfg->scan_mask & POOM_DRONE_SCAN_BLE) != 0U);
}

/**
 * @brief Internal helper for `cfg_set_wifi`.
 *
 * @param[in] cfg Parameter passed to the helper.
 * @param[in] on Parameter passed to the helper.
 * @return void
 */
static void cfg_set_wifi_(poom_drone_config_t *cfg, bool on)
{
    if (cfg == NULL)
    {
        return;
    }
    if (on)
    {
        cfg->scan_mask |= POOM_DRONE_SCAN_WIFI;
    }
    else
    {
        cfg->scan_mask &= ~POOM_DRONE_SCAN_WIFI;
    }
    if (cfg->scan_mask == 0U)
    {
        cfg->scan_mask = POOM_DRONE_SCAN_WIFI;
    }
}

/**
 * @brief Internal helper for `cfg_set_ble`.
 *
 * @param[in] cfg Parameter passed to the helper.
 * @param[in] on Parameter passed to the helper.
 * @return void
 */
static void cfg_set_ble_(poom_drone_config_t *cfg, bool on)
{
    if (cfg == NULL)
    {
        return;
    }
    if (on)
    {
        cfg->scan_mask |= POOM_DRONE_SCAN_BLE;
    }
    else
    {
        cfg->scan_mask &= ~POOM_DRONE_SCAN_BLE;
    }
    if (cfg->scan_mask == 0U)
    {
        cfg->scan_mask = POOM_DRONE_SCAN_WIFI;
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_draw_frame_(const char *title)
{
    char st_short[10];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(30, 2);
    (void)poom_arduboy_print(title ? title : "DRONE ID");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_set_cursor(92, 2);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_config_(void)
{
    const bool running = poom_drone_is_running();

    menu_draw_frame_("DRONE ID");

    const char *labels[MENU_DRONEID_ITEM_COUNT] = {"RUN", "WIFI", "BLE", "CLI", "PCAP"};
    for (int i = 0; i < (int)MENU_DRONEID_ITEM_COUNT; i++)
    {
        const int16_t y = (int16_t)(LIST_Y0 + i * ROW_STEP);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(labels[i]);

        poom_arduboy_set_cursor(46, y);
        switch ((menu_droneid_item_t)i)
        {
        case MENU_DRONEID_ITEM_RUN:
            (void)poom_arduboy_print(running ? "STOP" : "START");
            break;
        case MENU_DRONEID_ITEM_WIFI:
            (void)poom_arduboy_print(cfg_wifi_(&s_cfg) ? "ON" : "OFF");
            break;
        case MENU_DRONEID_ITEM_BLE:
            (void)poom_arduboy_print(cfg_ble_(&s_cfg) ? "ON" : "OFF");
            break;
        case MENU_DRONEID_ITEM_CLI:
            (void)poom_arduboy_print(s_cfg.enable_cli_print ? "ON" : "OFF");
            break;
        case MENU_DRONEID_ITEM_PCAP:
            if (!s_sd_available)
            {
                (void)poom_arduboy_print("NO SD");
            }
            else
            {
                (void)poom_arduboy_print(s_cfg.enable_pcap_to_sd ? "ON" : "OFF");
            }
            break;
        default:
            break;
        }

        if (i == s_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    if (s_selected == (int)MENU_DRONEID_ITEM_RUN)
    {
        (void)poom_arduboy_print(running ? F("A:STOP") : F("A:START"));
    }
    else
    {
        (void)poom_arduboy_print(F("A:TOGGLE"));
    }
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_running_(void)
{
    uint32_t count = 0;
    int8_t rssi = 0;
    uint16_t ch = 0;
    uint8_t mac[6] = {0};
    char id[ODID_ID_SIZE + 1] = {0};
    bool has = false;

    portENTER_CRITICAL(&s_stats_mux);
    count = s_report_count;
    rssi = s_last_rssi;
    ch = s_last_channel;
    memcpy(mac, s_last_mac, sizeof(mac));
    snprintf(id, sizeof(id), "%s", s_last_id);
    has = s_has_last;
    portEXIT_CRITICAL(&s_stats_mux);

    menu_draw_frame_("DRONE ID");

    poom_arduboy_set_cursor(4, 14);
    (void)poom_arduboy_print(F("State:RUN"));

    poom_arduboy_set_cursor(4, 22);
    char line_reports[22];
    (void)snprintf(line_reports, sizeof(line_reports), "Reports:%lu", (unsigned long)count);
    (void)poom_arduboy_print(line_reports);

    poom_arduboy_set_cursor(4, 30);
    char line_modes[22];
    (void)snprintf(line_modes,
                   sizeof(line_modes),
                   "W:%s B:%s",
                   cfg_wifi_(&s_cfg) ? "ON" : "OFF",
                   cfg_ble_(&s_cfg) ? "ON" : "OFF");
    (void)poom_arduboy_print(line_modes);

    poom_arduboy_set_cursor(4, 38);
    char line_out[22];
    if (!s_sd_available)
    {
        (void)snprintf(line_out, sizeof(line_out), "PCAP:NO SD");
    }
    else
    {
        (void)snprintf(line_out,
                       sizeof(line_out),
                       "PCAP:%s CLI:%s",
                       s_cfg.enable_pcap_to_sd ? "ON" : "OFF",
                       s_cfg.enable_cli_print ? "ON" : "OFF");
    }
    (void)poom_arduboy_print(line_out);

    poom_arduboy_set_cursor(4, 46);
    if (has && (id[0] != '\0'))
    {
        char line_id[22];
        (void)snprintf(line_id, sizeof(line_id), "ID:%.17s", id);
        (void)poom_arduboy_print(line_id);
    }
    else
    {
        char line_last[28];
        if (has)
        {
            (void)snprintf(line_last, sizeof(line_last), "%02X:%02X %ddBm CH%u",
                           mac[4],
                           mac[5],
                           (int)rssi,
                           (unsigned)ch);
        }
        else
        {
            (void)snprintf(line_last, sizeof(line_last), "CH:%u", (unsigned)ch);
        }
        (void)poom_arduboy_print(line_last);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:STOP"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_(void)
{
    if (s_screen == MENU_DRONEID_SCREEN_RUNNING)
    {
        menu_render_running_();
    }
    else
    {
        menu_render_config_();
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_handle_button_(uint8_t button)
{
    const bool running = poom_drone_is_running();

    if (s_screen == MENU_DRONEID_SCREEN_RUNNING)
    {
        if (button == BTN_A)
        {
            (void)poom_drone_stop();
            (void)snprintf(s_status, sizeof(s_status), "READY");
            s_screen = MENU_DRONEID_SCREEN_CONFIG;
        }
        else if (button == BTN_B)
        {
            menu_exit_();
        }
        return;
    }

    if (button == BTN_UP)
    {
        s_selected--;
        if (s_selected < 0)
        {
            s_selected = (int)MENU_DRONEID_ITEM_COUNT - 1;
        }
        return;
    }
    if (button == BTN_DOWN)
    {
        s_selected++;
        if (s_selected >= (int)MENU_DRONEID_ITEM_COUNT)
        {
            s_selected = 0;
        }
        return;
    }
    if (button == BTN_B)
    {
        menu_exit_();
        return;
    }
    if (button != BTN_A)
    {
        return;
    }

    if (s_selected < 0)
    {
        s_selected = 0;
    }
    if (s_selected >= (int)MENU_DRONEID_ITEM_COUNT)
    {
        s_selected = (int)MENU_DRONEID_ITEM_COUNT - 1;
    }

    if ((menu_droneid_item_t)s_selected == MENU_DRONEID_ITEM_RUN)
    {
        if (running)
        {
            (void)poom_drone_stop();
            (void)snprintf(s_status, sizeof(s_status), "READY");
            return;
        }

        portENTER_CRITICAL(&s_stats_mux);
        s_report_count = 0;
        s_has_last = false;
        s_last_id[0] = '\0';
        portEXIT_CRITICAL(&s_stats_mux);

        esp_err_t err = poom_drone_start_ex(&s_cfg);
        if (err == ESP_OK)
        {
            (void)snprintf(s_status, sizeof(s_status), "RUN");
            s_screen = MENU_DRONEID_SCREEN_RUNNING;
        }
        else
        {
            (void)snprintf(s_status, sizeof(s_status), "ERR %d", (int)err);
        }
        return;
    }

    if (running)
    {
        (void)snprintf(s_status, sizeof(s_status), "STOP1ST");
        return;
    }

    switch ((menu_droneid_item_t)s_selected)
    {
    case MENU_DRONEID_ITEM_WIFI:
        cfg_set_wifi_(&s_cfg, !cfg_wifi_(&s_cfg));
        break;
    case MENU_DRONEID_ITEM_BLE:
        cfg_set_ble_(&s_cfg, !cfg_ble_(&s_cfg));
        break;
    case MENU_DRONEID_ITEM_CLI:
        s_cfg.enable_cli_print = !s_cfg.enable_cli_print;
        break;
    case MENU_DRONEID_ITEM_PCAP:
        if (s_sd_available)
        {
            s_cfg.enable_pcap_to_sd = !s_cfg.enable_pcap_to_sd;
        }
        break;
    default:
        break;
    }

    (void)snprintf(s_status, sizeof(s_status), "READY");
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_btn_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;
    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }
    if (!s_menu_active || (s_btn_q == NULL))
    {
        return;
    }

    button_event_msg_t ev = {0};
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    menu_droneid_btn_msg_t out = {.button = ev.button, .event = ev.event};
    (void)xQueueSend(s_btn_q, &out, 0U);
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_task_(void *arg)
{
    (void)arg;

    while (s_menu_active)
    {
        menu_droneid_btn_msg_t btn = {0};
        if ((s_btn_q != NULL) && (xQueueReceive(s_btn_q, &btn, pdMS_TO_TICKS(MENU_DRONEID_REFRESH_MS)) == pdPASS))
        {
            menu_handle_button_(btn.button);
            if (!s_menu_active)
            {
                break;
            }
        }

        menu_render_();
    }

    s_menu_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_active = false;
    (void)poom_drone_stop();

    if (s_menu_task != NULL)
    {
        if (s_menu_task != current_task)
        {
            TaskHandle_t t = s_menu_task;
            s_menu_task = NULL;
            vTaskDelete(t);
        }
        else
        {
            s_menu_task = NULL;
        }
    }

    if (s_btn_q != NULL)
    {
        QueueHandle_t q = s_btn_q;
        s_btn_q = NULL;
        vQueueDelete(q);
    }

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_btn_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

void menu_poom_droneid_show(void)
{
    if (s_menu_active)
    {
        return;
    }

    poom_drone_config_default(&s_cfg);
    poom_drone_register_report_cb(menu_report_cb_, NULL);

    s_sd_available = menu_sd_probe_();
    s_cfg.enable_pcap_to_sd = false;

    s_selected = 0;
    s_screen = MENU_DRONEID_SCREEN_CONFIG;
    snprintf(s_status, sizeof(s_status), "READY");

    portENTER_CRITICAL(&s_stats_mux);
    s_report_count = 0;
    s_has_last = false;
    s_last_id[0] = '\0';
    portEXIT_CRITICAL(&s_stats_mux);

    s_btn_q = xQueueCreate(8, sizeof(menu_droneid_btn_msg_t));
    if (s_btn_q == NULL)
    {
        return;
    }

    s_menu_active = true;

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_btn_cb_, s_sbus_user);
        s_buttons_subscribed = true;
    }

    if (xTaskCreate(menu_task_, "menu_poom_droneid", MENU_DRONEID_STACK, NULL, MENU_DRONEID_PRIO, &s_menu_task) != pdPASS)
    {
        s_menu_active = false;
        menu_exit_();
        return;
    }
}
