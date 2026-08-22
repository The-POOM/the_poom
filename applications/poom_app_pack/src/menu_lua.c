// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_lua.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#include "poom_lua.h"
#include "poom_sd_browser.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_LUA_REFRESH_MS (180U)
#define MENU_LUA_STACK (4096U)
#define MENU_LUA_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

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
} menu_lua_button_msg_t;

typedef enum
{
    MENU_LUA_OPT_RUN_MAIN = 0,
    MENU_LUA_OPT_BROWSE,
    MENU_LUA_OPT_COUNT,
} menu_lua_opt_t;

typedef enum
{
    MENU_LUA_STATE_MAIN = 0,
    MENU_LUA_STATE_RUNNING,
    MENU_LUA_STATE_RESULT,
} menu_lua_state_t;

#define MENU_LUA_RESULT_HOLD_CYCLES (8U)

static bool s_menu_lua_active = false;
static bool s_menu_lua_buttons_subscribed = false;
static volatile bool s_menu_lua_exit_requested = false;
static volatile bool s_menu_lua_run_requested = false;
static volatile bool s_menu_lua_browse_requested = false;
static TaskHandle_t s_menu_lua_task = NULL;
static char s_menu_lua_sbus_user[] = "menu_lua";

static menu_lua_state_t s_state = MENU_LUA_STATE_MAIN;
static menu_lua_opt_t s_opt = MENU_LUA_OPT_RUN_MAIN;

static char s_selected_path[128] = "/sdcard/main.lua";
static volatile bool s_lua_done = false;
static esp_err_t s_lua_result = ESP_OK;
static char s_lua_err[96] = "";
static uint8_t s_result_hold_cycles = 0U;

static void menu_lua_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_lua_task_(void* arg);

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_lua_draw_frame_(const char* title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(46, 2);
    (void)poom_arduboy_print(title ? title : "LUA");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_lua_draw_main_(void)
{
    menu_lua_draw_frame_("LUA");

    poom_arduboy_set_cursor(4, 18);
    (void)poom_arduboy_print(F("RUN MAIN.LUA"));
    if(s_opt == MENU_LUA_OPT_RUN_MAIN)
    {
        poom_arduboy_fill_rect(0, 17, ARDUBOY_WIDTH, 9, INVERT);
    }

    poom_arduboy_set_cursor(4, 30);
    (void)poom_arduboy_print(F("BROWSE SD"));
    if(s_opt == MENU_LUA_OPT_BROWSE)
    {
        poom_arduboy_fill_rect(0, 29, ARDUBOY_WIDTH, 9, INVERT);
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
static void menu_lua_draw_result_(void)
{
    menu_lua_draw_frame_("LUA");

    poom_arduboy_set_cursor(4, 20);
    if(s_lua_result == ESP_OK)
    {
        (void)poom_arduboy_print(F("OK"));
    }
    else
    {
        (void)poom_arduboy_print(F("ERROR"));
    }

    poom_arduboy_set_cursor(4, 32);
    if(s_lua_err[0] != '\0')
    {
        char line[22];
        (void)snprintf(line, sizeof(line), "%.21s", s_lua_err);
        (void)poom_arduboy_print(line);
    }
    else
    {
        (void)poom_arduboy_print(F(" "));
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:AGAIN"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_lua_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_lua_active = false;
    s_menu_lua_exit_requested = false;
    s_menu_lua_run_requested = false;
    s_menu_lua_browse_requested = false;

    if(s_menu_lua_task != NULL)
    {
        if(s_menu_lua_task != current_task)
        {
            TaskHandle_t task = s_menu_lua_task;
            s_menu_lua_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_lua_task = NULL;
        }
    }

    if(s_menu_lua_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_lua_button_cb_, s_menu_lua_sbus_user);
        s_menu_lua_buttons_subscribed = false;
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Internal helper for `menu_lua_filter_lua`.
 *
 * @param[in] name Parameter passed to the helper.
 * @param[in] is_directory Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return bool
 */
static bool menu_lua_filter_lua_(const char* name, bool is_directory, void* user_ctx)
{
    (void)user_ctx;
    if(is_directory)
    {
        return true;
    }
    if((name == NULL) || (name[0] == '\0'))
    {
        return false;
    }
    const char* dot = strrchr(name, '.');
    if(dot == NULL)
    {
        return false;
    }
    return (strcasecmp(dot, ".lua") == 0);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_lua_sd_exit_cb_(void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    menu_lua_show();
}

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_lua_sd_file_selected_cb_(const char* abs_path, void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);

    if((abs_path != NULL) && (abs_path[0] != '\0'))
    {
        (void)snprintf(s_selected_path, sizeof(s_selected_path), "%s", abs_path);
    }

    menu_lua_show();
    s_menu_lua_run_requested = true;
}

/**
 * @brief Internal helper for `menu_lua_launch_browser`.
 *
 * @return void
 */
static void menu_lua_launch_browser_(void)
{
    poom_sd_browser_config_t cfg = {
        .start_dir = "/sdcard",
        .header = "LUA",
        .filter = menu_lua_filter_lua_,
        .filter_ctx = NULL,
        .on_file_selected = menu_lua_sd_file_selected_cb_,
        .on_file_selected_ctx = NULL,
    };

    (void)poom_sd_browser_set_exit_callback(menu_lua_sd_exit_cb_, NULL);
    (void)poom_sd_browser_start_ex(&cfg);
}

/**
 * @brief Internal helper for `menu_lua_on_done`.
 *
 * @param[in] status Parameter passed to the helper.
 * @param[in] error_msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_lua_on_done_(esp_err_t status, const char* error_msg, void* user_ctx)
{
    (void)user_ctx;

    s_lua_result = status;
    (void)snprintf(s_lua_err, sizeof(s_lua_err), "%.95s", (error_msg != NULL) ? error_msg : "");
    s_lua_done = true;
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_lua_start_run_(void)
{
    s_lua_done = false;
    s_lua_err[0] = '\0';
    s_lua_result = ESP_FAIL;

    const char* path = (s_selected_path[0] != '\0') ? s_selected_path : "/sdcard/main.lua";
    esp_err_t err = poom_lua_run_file_async(path, menu_lua_on_done_, NULL);
    if(err != ESP_OK)
    {
        s_lua_result = err;
        (void)snprintf(s_lua_err, sizeof(s_lua_err), "start:%d", (int)err);
        s_lua_done = true;
        s_state = MENU_LUA_STATE_RESULT;
        return;
    }

    s_state = MENU_LUA_STATE_RUNNING;
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_lua_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(menu_lua_button_msg_t)))
    {
        return;
    }

    menu_lua_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if(ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(ev.button == BTN_B)
    {
        if(poom_lua_is_running())
        {
            (void)poom_lua_request_stop();
            return;
        }
        s_menu_lua_exit_requested = true;
        return;
    }

    if(s_state == MENU_LUA_STATE_MAIN)
    {
        if(ev.button == BTN_UP)
        {
            if(s_opt > 0)
            {
                s_opt = (menu_lua_opt_t)((int)s_opt - 1);
            }
        }
        else if(ev.button == BTN_DOWN)
        {
            if(((int)s_opt + 1) < (int)MENU_LUA_OPT_COUNT)
            {
                s_opt = (menu_lua_opt_t)((int)s_opt + 1);
            }
        }
        else if(ev.button == BTN_A)
        {
            if(s_opt == MENU_LUA_OPT_RUN_MAIN)
            {
                (void)snprintf(s_selected_path, sizeof(s_selected_path), "%s", "/sdcard/main.lua");
                s_menu_lua_run_requested = true;
            }
            else
            {
                s_menu_lua_browse_requested = true;
            }
        }
        return;
    }

    if(s_state == MENU_LUA_STATE_RESULT)
    {
        if(ev.button == BTN_A)
        {
            s_menu_lua_run_requested = true;
        }
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_lua_task_(void* arg)
{
    (void)arg;

    while(s_menu_lua_active)
    {
        if(s_menu_lua_exit_requested)
        {
            menu_lua_exit_();
            break;
        }

        if((s_state != MENU_LUA_STATE_RUNNING) && s_menu_lua_browse_requested)
        {
            s_menu_lua_browse_requested = false;

            if(s_menu_lua_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", menu_lua_button_cb_, s_menu_lua_sbus_user);
                s_menu_lua_buttons_subscribed = false;
            }

            s_menu_lua_active = false;
            s_menu_lua_task = NULL;
            menu_lua_launch_browser_();
            vTaskDelete(NULL);
        }

        if((s_state != MENU_LUA_STATE_RUNNING) && s_menu_lua_run_requested)
        {
            s_menu_lua_run_requested = false;
            menu_lua_start_run_();
        }

        if(s_state == MENU_LUA_STATE_RUNNING)
        {
            if(s_lua_done)
            {
                s_lua_done = false;
                if((s_lua_result == ESP_OK) || (strcmp(s_lua_err, "run:stopped") == 0))
                {
                    menu_lua_exit_();
                    break;
                }

                s_state = MENU_LUA_STATE_RESULT;
                s_result_hold_cycles = MENU_LUA_RESULT_HOLD_CYCLES;
                menu_lua_draw_result_();
            }

            vTaskDelay(pdMS_TO_TICKS(MENU_LUA_REFRESH_MS));
            continue;
        }
        else if(s_state == MENU_LUA_STATE_RESULT)
        {
            menu_lua_draw_result_();
            if(s_result_hold_cycles > 0U)
            {
                s_result_hold_cycles--;
            }
            else
            {
                menu_lua_exit_();
                break;
            }
        }
        else
        {
            s_state = MENU_LUA_STATE_MAIN;
            menu_lua_draw_main_();
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_LUA_REFRESH_MS));
    }

    s_menu_lua_task = NULL;
    vTaskDelete(NULL);
}

void menu_lua_show(void)
{
    if(s_menu_lua_task != NULL)
    {
        return;
    }

    s_menu_lua_active = true;
    s_menu_lua_exit_requested = false;
    s_menu_lua_run_requested = false;
    s_menu_lua_browse_requested = false;

    s_state = MENU_LUA_STATE_MAIN;
    s_opt = MENU_LUA_OPT_RUN_MAIN;

    if(!s_menu_lua_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_lua_button_cb_, s_menu_lua_sbus_user))
        {
            s_menu_lua_buttons_subscribed = true;
        }
        else
        {
            s_menu_lua_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_lua_task_, "menu_lua", MENU_LUA_STACK, NULL, MENU_LUA_PRIO, &s_menu_lua_task);
}
