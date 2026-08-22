// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_drone_emul.h"

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
#include "poom_drone.h"
#include "poom_drone_emul.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_DRONE_EMUL_REFRESH_MS (250U)
#define MENU_DRONE_EMUL_STACK (3072U)
#define MENU_DRONE_EMUL_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (8)

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

static bool s_menu_active = false;
static bool s_buttons_subscribed = false;
static bool s_exit_requested = false;
static bool s_action_requested = false;
static bool s_input_dirty = false;
static TaskHandle_t s_menu_task = NULL;
static char s_sbus_user[] = "menu_poom_drone_emul";
static char s_status[22] = "Press A to run";
static uint8_t s_selected = 0;

typedef enum
{
    MENU_ITEM_STATE = 0,
    MENU_ITEM_QTY,
    MENU_ITEM_BLE,
    MENU_ITEM_LOC,
    MENU_ITEM_COUNT,
} menu_item_t;

static void menu_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_task_(void *arg);
static void menu_exit_(void);

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_request_render_from_input_(void)
{
    s_input_dirty = true;
    if (s_menu_task != NULL)
    {
        (void)xTaskNotifyGive(s_menu_task);
    }
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_draw_header_(void)
{
    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("DRONE EMUL"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] running Parameter passed to the helper.
 * @param[in] status_text Parameter passed to the helper.
 * @return void
 */
static void menu_draw_(bool running, const char *status_text)
{
    double lat = 0;
    double lon = 0;
    (void)poom_drone_emul_get_location(&lat, &lon);
    uint8_t count = 0;
    (void)poom_drone_emul_get_count(&count);
    bool ble = false;
    (void)poom_drone_emul_get_ble_enabled(&ble);

    char line_state[22];
    char line_qty[22];
    char line_ble[22];
    char line_loc[22];
    char line_hint[22];

    (void)snprintf(line_state, sizeof(line_state), "State: %s", running ? "RUN" : "OFF");
    (void)snprintf(line_qty, sizeof(line_qty), "Qty: %u", (unsigned)count);
    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", ble ? "ON" : "OFF");
    (void)snprintf(line_loc, sizeof(line_loc), "Loc: %.2f, %.2f", lat, lon);
    (void)snprintf(line_hint, sizeof(line_hint), "%.18s", (status_text != NULL) ? status_text : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_draw_header_();

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    const int16_t y_items[MENU_ITEM_COUNT] = {
        ROW0_Y,
        (int16_t)(ROW0_Y + ROW_STEP),
        (int16_t)(ROW0_Y + 2 * ROW_STEP),
        (int16_t)(ROW0_Y + 3 * ROW_STEP),
    };

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(line_state);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line_qty);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line_ble);

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(line_loc);

    if (s_selected < MENU_ITEM_COUNT)
    {
        poom_arduboy_fill_rect(1, (int16_t)(y_items[s_selected] - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
    }

    (void)line_hint;

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("LR:SET"));
    poom_arduboy_set_cursor(42, 56);
    (void)poom_arduboy_print(F("UD:SEL"));
    poom_arduboy_set_cursor(85, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_apply_lr`.
 *
 * @param[in] delta Parameter passed to the helper.
 * @return void
 */
static void menu_apply_lr_(int delta)
{
    if (delta == 0)
    {
        return;
    }

    if (s_selected == MENU_ITEM_STATE)
    {
        if (poom_drone_emul_is_running())
        {
            esp_err_t err = poom_drone_emul_stop();
            (void)snprintf(s_status, sizeof(s_status), (err == ESP_OK) ? "Stopped" : "Stop failed");
            return;
        }

        if (poom_drone_is_running())
        {
            (void)snprintf(s_status, sizeof(s_status), "Stop scan first");
            return;
        }

        poom_drone_emul_config_t cfg;
        poom_drone_emul_config_default(&cfg);
        esp_err_t err = poom_drone_emul_start(&cfg);
        (void)snprintf(s_status, sizeof(s_status), (err == ESP_OK) ? "Running" : "Start failed");
        return;
    }

    if (s_selected == MENU_ITEM_QTY)
    {
        uint8_t count = 0;
        (void)poom_drone_emul_get_count(&count);
        int next = (int)count + delta;
        if (next < 1)
        {
            next = 1;
        }
        if (next > 16)
        {
            next = 16;
        }
        (void)poom_drone_emul_set_count((uint8_t)next);
        (void)snprintf(s_status, sizeof(s_status), "Qty=%u", (unsigned)next);
        return;
    }

    if (s_selected == MENU_ITEM_BLE)
    {
        bool ble = false;
        (void)poom_drone_emul_get_ble_enabled(&ble);
        ble = !ble;
        (void)poom_drone_emul_set_ble_enabled(ble);
        (void)snprintf(s_status, sizeof(s_status), "BLE=%s", ble ? "ON" : "OFF");
        return;
    }

    (void)snprintf(s_status, sizeof(s_status), "Read-only");
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
    s_exit_requested = false;

    (void)poom_drone_emul_stop();

    if (s_menu_task != NULL)
    {
        if (s_menu_task != current_task)
        {
            TaskHandle_t task = s_menu_task;
            s_menu_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_task = NULL;
        }
    }

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_button_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
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

    bool last_draw_running = false;
    char last_draw_status[22] = {0};

    menu_request_render_from_input_();

    while (s_menu_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MENU_DRONE_EMUL_REFRESH_MS));

        if (s_exit_requested)
        {
            menu_exit_();
            break;
        }

        if (s_action_requested)
        {
            s_action_requested = false;
            s_input_dirty = true;
        }

        const bool running = poom_drone_emul_is_running();
        if (s_input_dirty || (running != last_draw_running) ||
            (strncmp(last_draw_status, s_status, sizeof(last_draw_status)) != 0))
        {
            menu_draw_(running, s_status);
            s_input_dirty = false;
            last_draw_running = running;
            (void)snprintf(last_draw_status, sizeof(last_draw_status), "%s", s_status);
        }
    }

    s_menu_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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

    if (ev.button == BTN_B)
    {
        s_exit_requested = true;
        menu_request_render_from_input_();
        return;
    }
    if (ev.button == BTN_UP)
    {
        if (s_selected > 0)
        {
            s_selected--;
        }
        menu_request_render_from_input_();
        return;
    }
    if (ev.button == BTN_DOWN)
    {
        if (s_selected < (MENU_ITEM_COUNT - 1))
        {
            s_selected++;
        }
        menu_request_render_from_input_();
        return;
    }
    if (ev.button == BTN_LEFT)
    {
        menu_apply_lr_(-1);
        menu_request_render_from_input_();
        return;
    }
    if (ev.button == BTN_RIGHT)
    {
        menu_apply_lr_(+1);
        menu_request_render_from_input_();
        return;
    }
}

void menu_poom_drone_emul_show(void)
{
    s_menu_active = true;
    s_exit_requested = false;
    s_action_requested = false;
    s_input_dirty = false;
    s_selected = MENU_ITEM_STATE;
    (void)snprintf(s_status, sizeof(s_status), poom_drone_emul_is_running() ? "Running" : "UD:SEL LR:SET");

    if (!s_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_button_cb_, s_sbus_user))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            s_menu_active = false;
            menu_draw_(false, "Button sub err");

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if (s_menu_task == NULL)
    {
        (void)xTaskCreate(menu_task_, "menu_drone_emul", MENU_DRONE_EMUL_STACK, NULL, MENU_DRONE_EMUL_PRIO, &s_menu_task);
    }
}
