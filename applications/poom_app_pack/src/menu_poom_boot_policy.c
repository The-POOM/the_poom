// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file menu_poom_boot_policy.c
 * @brief Visual selector used to launch the current game or load a new binary from the SD card.
 */

#include "menu_poom_boot_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_boot_policy.h"
#include "poom_sbus.h"
#include "poom_sd_browser.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_POOM_BOOT_POLICY_REFRESH_MS (120U)
#define MENU_POOM_BOOT_POLICY_STACK      (4096U)
#define MENU_POOM_BOOT_POLICY_PRIO       (4U)

#define HEADER_H           (11)
#define BOX_Y              (12)
#define BOX_H              (40)
#define MAIN_LIST_Y0       (16)
#define MAIN_ROW_STEP      (11)
#define MAIN_ROW_HILITE_H  (9)
#define STATUS_LINE_LEN    (22U)
#define RESULT_LINE_LEN    (96U)

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

/**
 * @brief Button event payload received from the internal event bus.
 */
typedef struct
{
    /**< Button identifier such as `BTN_A` or `BTN_B`. */
    uint8_t button;
    /**< Button event type, for example `BUTTON_SINGLE_CLICK`. */
    uint8_t event;
    /**< Timestamp in milliseconds associated with the event. */
    uint32_t ts_ms;
} menu_poom_boot_policy_button_msg_t;

/**
 * @brief UI states used by the game selector flow.
 */
typedef enum
{
    /**< Main selector screen with the available actions. */
    MENU_POOM_BOOT_POLICY_STATE_MAIN = 0,
    /**< Confirmation screen shown before flashing a new game. */
    MENU_POOM_BOOT_POLICY_STATE_CONFIRM,
    /**< Busy screen shown while the binary is being installed. */
    MENU_POOM_BOOT_POLICY_STATE_BUSY,
    /**< Result screen shown after an error or a completed operation. */
    MENU_POOM_BOOT_POLICY_STATE_RESULT,
} menu_poom_boot_policy_state_t;

/**
 * @brief Action entries available in the main selector menu.
 */
typedef enum
{
    /**< Boot the game currently installed in `ota_1`. */
    MENU_POOM_BOOT_POLICY_OPT_PLAY = 0,
    /**< Browse the SD card and install a new game binary. */
    MENU_POOM_BOOT_POLICY_OPT_LOAD,
    /**< Total number of main menu options. */
    MENU_POOM_BOOT_POLICY_OPT_COUNT,
} menu_poom_boot_policy_opt_t;

/**
 * @brief Heap-backed working buffers used only while the menu is active.
 */
typedef struct
{
    /**< Absolute path of the game binary selected from the SD browser. */
    char selected_game_path[256];
    /**< First short line shown in the result screen. */
    char result_line0[STATUS_LINE_LEN];
    /**< Second short line shown in the result screen. */
    char result_line1[STATUS_LINE_LEN];
    /**< Detail line shown in the result screen. */
    char result_detail[RESULT_LINE_LEN];
} menu_poom_boot_policy_buffers_t;

/** @brief Indicates whether the selector task is currently active. */
static bool s_active = false;
/** @brief Tracks whether the menu is currently subscribed to button events. */
static bool s_buttons_subscribed = false;
/** @brief Requests that the selector task exits and returns control to the caller menu. */
static volatile bool s_exit_requested = false;
/** @brief Requests execution of the primary action for the current UI state. */
static volatile bool s_primary_requested = false;
/** @brief Requests handing control to the SD browser. */
static volatile bool s_open_browser_requested = false;
/** @brief FreeRTOS task handle for the selector UI loop. */
static TaskHandle_t s_task = NULL;
/** @brief Stable user tag used when subscribing to the internal event bus. */
static char s_sbus_user[] = "menu_poom_boot_policy";

/** @brief Current UI state shown by the selector. */
static menu_poom_boot_policy_state_t s_state = MENU_POOM_BOOT_POLICY_STATE_MAIN;
/** @brief Currently highlighted option in the main menu. */
static menu_poom_boot_policy_opt_t s_opt = MENU_POOM_BOOT_POLICY_OPT_PLAY;
/** @brief Heap-allocated runtime buffers owned by the selector while it is open. */
static menu_poom_boot_policy_buffers_t* s_buffers = NULL;

static void menu_poom_boot_policy_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_poom_boot_policy_task_(void* arg);
static void menu_poom_boot_policy_sd_exit_cb_(void* user_ctx);
static void menu_poom_boot_policy_sd_selected_cb_(const char* abs_path, void* user_ctx);
static bool menu_poom_boot_policy_alloc_buffers_(void);
static void menu_poom_boot_policy_free_buffers_(void);

/**
 * @brief Allocates the heap-backed buffers required by this menu.
 *
 * @return `true` if the buffers are already available or were allocated successfully.
 * @return `false` if heap allocation failed.
 */
static bool menu_poom_boot_policy_alloc_buffers_(void)
{
    if(s_buffers != NULL)
    {
        return true;
    }

    s_buffers = (menu_poom_boot_policy_buffers_t*)calloc(1U, sizeof(*s_buffers));
    return (s_buffers != NULL);
}

/**
 * @brief Releases the heap-backed buffers used by this menu.
 */
static void menu_poom_boot_policy_free_buffers_(void)
{
    if(s_buffers != NULL)
    {
        free(s_buffers);
        s_buffers = NULL;
    }
}

/**
 * @brief Publishes a resume event so the parent menu can continue rendering.
 */
static void menu_poom_boot_policy_publish_resume_(void)
{
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

/**
 * @brief Stops the selector task, unsubscribes inputs, frees buffers, and returns to the caller.
 */
static void menu_poom_boot_policy_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_active = false;
    s_exit_requested = false;
    s_primary_requested = false;
    s_open_browser_requested = false;

    if(s_task != NULL)
    {
        if(s_task != current_task)
        {
            TaskHandle_t task = s_task;
            s_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_task = NULL;
        }
    }

    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_poom_boot_policy_button_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }

    menu_poom_boot_policy_free_buffers_();
    menu_poom_boot_policy_publish_resume_();
}

/**
 * @brief Draws the common frame and centered title used by all selector screens.
 *
 * @param[in] title Title to display in the header bar. If null, a default title is used.
 */
static void menu_poom_boot_policy_draw_frame_(const char* title)
{
    size_t width = 0U;
    int16_t x = 0;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    if(title != NULL)
    {
        width = strlen(title) * 6U;
    }
    if(width < (size_t)ARDUBOY_WIDTH)
    {
        x = (int16_t)((ARDUBOY_WIDTH - (int16_t)width) / 2);
    }

    poom_arduboy_set_cursor(x, 2);
    (void)poom_arduboy_print(title ? title : "GAME SLOT");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Fills the result buffers with user-facing status text.
 *
 * @param[in] line0 First result line to show to the user.
 * @param[in] line1 Second result line to show to the user.
 * @param[in] err Error code used to choose the short detail message.
 */
static void menu_poom_boot_policy_set_result_(const char* line0, const char* line1, esp_err_t err)
{
    const char* detail = "TRY AGAIN";
    char* result_line0;
    char* result_line1;
    char* result_detail;

    if(s_buffers == NULL)
    {
        return;
    }

    result_line0 = s_buffers->result_line0;
    result_line1 = s_buffers->result_line1;
    result_detail = s_buffers->result_detail;

    if(err == ESP_ERR_NOT_FOUND)
    {
        detail = "NO GAME";
    }
    else if(err == ESP_ERR_INVALID_SIZE)
    {
        detail = "BAD FILE";
    }
    else if(err == ESP_ERR_NO_MEM)
    {
        detail = "NO MEMORY";
    }
    else if(err == ESP_OK)
    {
        detail = "OK";
    }

    (void)snprintf(result_line0, STATUS_LINE_LEN, "%.21s", line0 ? line0 : "");
    (void)snprintf(result_line1, STATUS_LINE_LEN, "%.21s", line1 ? line1 : "");
    (void)snprintf(result_detail, RESULT_LINE_LEN, "%.95s", detail);
}

/**
 * @brief Builds a user-facing result screen from an operation context and an error code.
 *
 * @param[in] context Short label describing the operation that failed.
 * @param[in] err Error code returned by the failed operation.
 */
static void menu_poom_boot_policy_result_from_error_(const char* context, esp_err_t err)
{
    char line0[STATUS_LINE_LEN];

    (void)snprintf(line0, sizeof(line0), "%.21s", context ? context : "Error");
    menu_poom_boot_policy_set_result_(line0, "Operation failed", err);
}

/**
 * @brief Filters SD browser entries so only directories and `.bin` files are selectable.
 *
 * @param[in] name Entry name reported by the browser.
 * @param[in] is_directory `true` if the entry is a directory.
 * @param[in] user_ctx Unused caller-provided context pointer.
 *
 * @return `true` if the entry should be shown.
 * @return `false` if the entry should be hidden.
 */
static bool menu_poom_boot_policy_filter_bin_(const char* name, bool is_directory, void* user_ctx)
{
    const char* dot;

    (void)user_ctx;

    if((name == NULL) || (name[0] == '\0') || (name[0] == '.'))
    {
        return false;
    }

    if(is_directory)
    {
        return true;
    }

    dot = strrchr(name, '.');
    return (dot != NULL) && (strcasecmp(dot, ".bin") == 0);
}

/**
 * @brief Draws the main selector screen with the available actions.
 */
static void menu_poom_boot_policy_draw_main_(void)
{
    static const char* const items[MENU_POOM_BOOT_POLICY_OPT_COUNT] = {
        "PLAY CURRENT GAME",
        "LOAD NEW BIN",
    };

    menu_poom_boot_policy_draw_frame_("THE GAMER");

    for(int row = 0; row < (int)MENU_POOM_BOOT_POLICY_OPT_COUNT; row++)
    {
        const int16_t y = (int16_t)(MAIN_LIST_Y0 + row * MAIN_ROW_STEP);
        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(items[row]);
        if(row == (int)s_opt)
        {
            poom_arduboy_fill_rect(0, y - 1, ARDUBOY_WIDTH, MAIN_ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));
    poom_arduboy_display();
}

/**
 * @brief Draws the confirmation screen shown before flashing the selected game.
 */
static void menu_poom_boot_policy_draw_confirm_(void)
{
    menu_poom_boot_policy_draw_frame_("LOAD GAME");

    poom_arduboy_set_cursor(12, 24);
    (void)poom_arduboy_print(F("LOAD SELECTED"));
    poom_arduboy_set_cursor(30, 36);
    (void)poom_arduboy_print(F("GAME?"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:FLASH"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Draws the busy screen shown while the game binary is being installed.
 */
static void menu_poom_boot_policy_draw_busy_(void)
{
    menu_poom_boot_policy_draw_frame_("FLASH GAME");

    poom_arduboy_set_cursor(10, 20);
    (void)poom_arduboy_print(F("INSTALLING..."));
    poom_arduboy_set_cursor(10, 30);
    (void)poom_arduboy_print(F("PLEASE WAIT"));
    poom_arduboy_set_cursor(10, 40);
    (void)poom_arduboy_print(F("LOADING GAME"));

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("WORKING"));
    poom_arduboy_display();
}

/**
 * @brief Draws the result screen using the current buffered status text.
 */
static void menu_poom_boot_policy_draw_result_(void)
{
    menu_poom_boot_policy_draw_frame_("GAME SLOT");

    poom_arduboy_set_cursor(4, 18);
    (void)poom_arduboy_print((s_buffers != NULL) ? s_buffers->result_line0 : "");
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print((s_buffers != NULL) ? s_buffers->result_line1 : "");
    poom_arduboy_set_cursor(4, 38);
    (void)poom_arduboy_print((s_buffers != NULL) ? s_buffers->result_detail : "");

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Draws the screen that matches the current selector state.
 */
static void menu_poom_boot_policy_draw_(void)
{
    switch(s_state)
    {
        case MENU_POOM_BOOT_POLICY_STATE_CONFIRM:
            menu_poom_boot_policy_draw_confirm_();
            break;
        case MENU_POOM_BOOT_POLICY_STATE_BUSY:
            menu_poom_boot_policy_draw_busy_();
            break;
        case MENU_POOM_BOOT_POLICY_STATE_RESULT:
            menu_poom_boot_policy_draw_result_();
            break;
        case MENU_POOM_BOOT_POLICY_STATE_MAIN:
        default:
            menu_poom_boot_policy_draw_main_();
            break;
    }
}

/**
 * @brief Launches the SD browser configured for game binaries.
 */
static void menu_poom_boot_policy_launch_browser_(void)
{
    poom_sd_browser_config_t cfg = {
        .start_dir = "/sdcard/apps",
        .header = "GAME BIN",
        .filter = menu_poom_boot_policy_filter_bin_,
        .filter_ctx = NULL,
        .on_file_selected = menu_poom_boot_policy_sd_selected_cb_,
        .on_file_selected_ctx = NULL,
    };

    (void)poom_sd_browser_set_exit_callback(menu_poom_boot_policy_sd_exit_cb_, NULL);
    (void)poom_sd_browser_start_ex(&cfg);
}

/**
 * @brief Executes the primary action for the current state.
 *
 * @details In the main screen this either boots the current game or opens the SD browser.
 *          In the confirmation screen it flashes the selected binary. In the result screen
 *          it returns to the main selector.
 */
static void menu_poom_boot_policy_run_primary_(void)
{
    esp_err_t err;

    if(s_state == MENU_POOM_BOOT_POLICY_STATE_MAIN)
    {
        if(s_opt == MENU_POOM_BOOT_POLICY_OPT_PLAY)
        {
            err = poom_boot_policy_boot_game();
            if(err != ESP_OK)
            {
                menu_poom_boot_policy_result_from_error_("Play game", err);
                s_state = MENU_POOM_BOOT_POLICY_STATE_RESULT;
            }
            else
            {
                esp_restart();
            }
            return;
        }

        err = poom_boot_policy_prepare_apps_dir();
        if(err != ESP_OK)
        {
            menu_poom_boot_policy_result_from_error_("Open /apps", err);
            s_state = MENU_POOM_BOOT_POLICY_STATE_RESULT;
            return;
        }

        s_open_browser_requested = true;
        return;
    }

    if(s_state == MENU_POOM_BOOT_POLICY_STATE_CONFIRM)
    {
        s_state = MENU_POOM_BOOT_POLICY_STATE_BUSY;
        menu_poom_boot_policy_draw_();
        vTaskDelay(pdMS_TO_TICKS(20U));

        if((s_buffers == NULL) || (s_buffers->selected_game_path[0] == '\0'))
        {
            err = ESP_ERR_NOT_FOUND;
        }
        else
        {
            err = poom_boot_policy_install_and_boot(s_buffers->selected_game_path);
        }

        if(err != ESP_OK)
        {
            menu_poom_boot_policy_result_from_error_("Flash game", err);
            s_state = MENU_POOM_BOOT_POLICY_STATE_RESULT;
        }
        return;
    }

    if(s_state == MENU_POOM_BOOT_POLICY_STATE_RESULT)
    {
        s_state = MENU_POOM_BOOT_POLICY_STATE_MAIN;
    }
}

/**
 * @brief Handles button events received from the internal event bus.
 *
 * @param[in] msg Incoming event-bus message that may contain a button event payload.
 * @param[in] user_ctx Unused subscription context pointer.
 */
static void menu_poom_boot_policy_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_poom_boot_policy_button_msg_t ev;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));
    if(ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(ev.button == BTN_B)
    {
        if(s_state == MENU_POOM_BOOT_POLICY_STATE_CONFIRM)
        {
            s_state = MENU_POOM_BOOT_POLICY_STATE_MAIN;
            return;
        }

        if(s_state == MENU_POOM_BOOT_POLICY_STATE_RESULT)
        {
            s_state = MENU_POOM_BOOT_POLICY_STATE_MAIN;
            return;
        }

        if(s_state == MENU_POOM_BOOT_POLICY_STATE_MAIN)
        {
            s_exit_requested = true;
        }
        return;
    }

    if(s_state == MENU_POOM_BOOT_POLICY_STATE_MAIN)
    {
        if(ev.button == BTN_UP)
        {
            if(s_opt > 0)
            {
                s_opt = (menu_poom_boot_policy_opt_t)((int)s_opt - 1);
            }
        }
        else if(ev.button == BTN_DOWN)
        {
            if(((int)s_opt + 1) < (int)MENU_POOM_BOOT_POLICY_OPT_COUNT)
            {
                s_opt = (menu_poom_boot_policy_opt_t)((int)s_opt + 1);
            }
        }
        else if(ev.button == BTN_A)
        {
            s_primary_requested = true;
        }
        return;
    }

    if(((s_state == MENU_POOM_BOOT_POLICY_STATE_CONFIRM) || (s_state == MENU_POOM_BOOT_POLICY_STATE_RESULT)) &&
       (ev.button == BTN_A))
    {
        s_primary_requested = true;
    }
}

/**
 * @brief SD browser exit callback that reopens the selector without changing the selection.
 *
 * @param[in] user_ctx Unused callback context pointer.
 */
static void menu_poom_boot_policy_sd_exit_cb_(void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    menu_poom_boot_policy_show();
}

/**
 * @brief SD browser selection callback that stores the chosen path and opens confirmation.
 *
 * @param[in] abs_path Absolute path selected in the SD browser.
 * @param[in] user_ctx Unused callback context pointer.
 */
static void menu_poom_boot_policy_sd_selected_cb_(const char* abs_path, void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);

    menu_poom_boot_policy_show();

    if((abs_path != NULL) && (abs_path[0] != '\0'))
    {
        if(s_buffers != NULL)
        {
            (void)snprintf(s_buffers->selected_game_path, sizeof(s_buffers->selected_game_path), "%s", abs_path);
        }
    }

    s_state = MENU_POOM_BOOT_POLICY_STATE_CONFIRM;
}

/**
 * @brief Main UI task for the selector.
 *
 * @param[in] arg Unused FreeRTOS task argument.
 */
static void menu_poom_boot_policy_task_(void* arg)
{
    (void)arg;

    while(s_active)
    {
        if(s_exit_requested)
        {
            menu_poom_boot_policy_exit_();
            break;
        }

        if(s_open_browser_requested)
        {
            s_open_browser_requested = false;

            if(s_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", menu_poom_boot_policy_button_cb_, s_sbus_user);
                s_buttons_subscribed = false;
            }

            s_active = false;
            s_task = NULL;
            menu_poom_boot_policy_launch_browser_();
            vTaskDelete(NULL);
        }

        if(s_primary_requested)
        {
            s_primary_requested = false;
            menu_poom_boot_policy_run_primary_();
        }

        menu_poom_boot_policy_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_POOM_BOOT_POLICY_REFRESH_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void menu_poom_boot_policy_show(void)
{
    if(s_task != NULL)
    {
        return;
    }

    if(!menu_poom_boot_policy_alloc_buffers_())
    {
        menu_poom_boot_policy_publish_resume_();
        return;
    }

    s_active = true;
    s_exit_requested = false;
    s_primary_requested = false;
    s_open_browser_requested = false;
    s_state = MENU_POOM_BOOT_POLICY_STATE_MAIN;
    s_opt = MENU_POOM_BOOT_POLICY_OPT_PLAY;

    if(!s_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_poom_boot_policy_button_cb_, s_sbus_user))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            s_active = false;
            menu_poom_boot_policy_publish_resume_();
            return;
        }
    }

    (void)xTaskCreate(menu_poom_boot_policy_task_,
                      "menu_boot_policy",
                      MENU_POOM_BOOT_POLICY_STACK,
                      NULL,
                      MENU_POOM_BOOT_POLICY_PRIO,
                      &s_task);
}
