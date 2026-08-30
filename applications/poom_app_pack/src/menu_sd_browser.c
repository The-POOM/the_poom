// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_sd_browser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_sd_browser.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define MENU_SD_SBUS_USER      "menu_sd"

#define MENU_SD_REFRESH_MS (140U)
#define MENU_SD_STACK      (2048U)
#define MENU_SD_PRIO       (4U)

#define MENU_SD_HEADER_H        (11)
#define MENU_SD_BOX_Y           (12)
#define MENU_SD_BOX_H           (40)
#define MENU_SD_ROW_Y0          (18)
#define MENU_SD_ROW_STEP        (10)
#define MENU_SD_ROW_HILITE_H    (9)
#define MENU_SD_STATUS_LINE_MAX (22U)
#define MENU_SD_VISIBLE_ROWS    (3U)

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

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_sd_button_msg_t;

typedef enum
{
    MENU_SD_OPT_BROWSE = 0,
    MENU_SD_OPT_CHECK,
    MENU_SD_OPT_INFO,
    MENU_SD_OPT_FORMAT,
    MENU_SD_OPT_COUNT,
} menu_sd_opt_t;

typedef enum
{
    MENU_SD_STATE_MAIN = 0,
    MENU_SD_STATE_INFO,
    MENU_SD_STATE_CONFIRM,
    MENU_SD_STATE_RESULT,
} menu_sd_state_t;

typedef struct
{
    bool active;
    bool buttons_subscribed;
    volatile bool exit_requested;
    volatile bool browse_requested;
    volatile bool check_requested;
    volatile bool info_requested;
    volatile bool format_requested;
    TaskHandle_t task;
    menu_sd_state_t state;
    menu_sd_opt_t opt;
    bool confirm_yes;
    char status_line0[MENU_SD_STATUS_LINE_MAX];
    char status_line1[MENU_SD_STATUS_LINE_MAX];
} menu_sd_ctx_t;

static menu_sd_ctx_t* s_menu_sd = NULL;

static void menu_sd_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_sd_task_(void* arg);
static void menu_sd_browser_exit_cb_(void* user_ctx);

static menu_sd_ctx_t* menu_sd_ctx_(void)
{
    return s_menu_sd;
}

static bool menu_sd_alloc_ctx_(void)
{
    if(s_menu_sd != NULL)
    {
        return true;
    }

    s_menu_sd = (menu_sd_ctx_t*)calloc(1U, sizeof(*s_menu_sd));
    if(s_menu_sd == NULL)
    {
        return false;
    }

    s_menu_sd->state = MENU_SD_STATE_MAIN;
    s_menu_sd->opt = MENU_SD_OPT_BROWSE;
    return true;
}

static void menu_sd_publish_resume_(void)
{
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

static void menu_sd_destroy_ctx_(bool publish_resume)
{
    menu_sd_ctx_t* ctx = s_menu_sd;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    if(ctx == NULL)
    {
        if(publish_resume)
        {
            menu_sd_publish_resume_();
        }
        return;
    }

    s_menu_sd = NULL;

    if(ctx->buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_sd_button_cb_, MENU_SD_SBUS_USER);
        ctx->buttons_subscribed = false;
    }

    if((ctx->task != NULL) && (ctx->task != current_task))
    {
        vTaskDelete(ctx->task);
    }

    free(ctx);

    if(publish_resume)
    {
        menu_sd_publish_resume_();
    }
}

static void menu_sd_draw_frame_(const char* title)
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
    (void)poom_arduboy_print((title != NULL) ? title : "SD");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_SD_HEADER_H, INVERT);
    poom_arduboy_draw_rect(0, MENU_SD_BOX_Y, ARDUBOY_WIDTH, MENU_SD_BOX_H, WHITE);
}

static const char* menu_sd_opt_label_(menu_sd_opt_t opt)
{
    switch(opt)
    {
        case MENU_SD_OPT_BROWSE: return "BROWSE FILES";
        case MENU_SD_OPT_CHECK:  return "CHECK CARD";
        case MENU_SD_OPT_INFO:   return "CARD INFO";
        case MENU_SD_OPT_FORMAT: return "FORMAT CARD";
        default:                 return "";
    }
}

static void menu_sd_draw_main_(const menu_sd_ctx_t* ctx)
{
    uint8_t first_idx = 0U;
    uint8_t i;

    if(ctx == NULL)
    {
        return;
    }

    menu_sd_draw_frame_("SD");

    if((uint8_t)ctx->opt >= MENU_SD_VISIBLE_ROWS)
    {
        first_idx = (uint8_t)ctx->opt - (MENU_SD_VISIBLE_ROWS - 1U);
    }

    for(i = 0U; i < MENU_SD_VISIBLE_ROWS; i++)
    {
        const uint8_t idx = (uint8_t)(first_idx + i);
        const int16_t y = (int16_t)(MENU_SD_ROW_Y0 + (int16_t)i * MENU_SD_ROW_STEP);

        if(idx >= (uint8_t)MENU_SD_OPT_COUNT)
        {
            break;
        }

        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(menu_sd_opt_label_((menu_sd_opt_t)idx));
        if(idx == (uint8_t)ctx->opt)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MENU_SD_ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_sd_draw_result_(const menu_sd_ctx_t* ctx)
{
    if(ctx == NULL)
    {
        return;
    }

    menu_sd_draw_frame_("SD");

    poom_arduboy_set_cursor(4, 22);
    (void)poom_arduboy_print(ctx->status_line0);
    poom_arduboy_set_cursor(4, 34);
    (void)poom_arduboy_print(ctx->status_line1);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_sd_draw_confirm_(const menu_sd_ctx_t* ctx)
{
    const int16_t sel_y = 46;

    if(ctx == NULL)
    {
        return;
    }

    menu_sd_draw_frame_("FORMAT SD");

    poom_arduboy_set_cursor(12, 20);
    (void)poom_arduboy_print(F("FORMAT SD CARD?"));
    poom_arduboy_set_cursor(12, 32);
    (void)poom_arduboy_print(F("ERASE ALL DATA"));

    poom_arduboy_set_cursor(34, sel_y);
    (void)poom_arduboy_print(F("YES"));
    poom_arduboy_set_cursor(84, sel_y);
    (void)poom_arduboy_print(F("NO"));

    if(ctx->confirm_yes)
    {
        poom_arduboy_fill_rect(32, (int16_t)(sel_y - 1), 24, MENU_SD_ROW_HILITE_H, INVERT);
    }
    else
    {
        poom_arduboy_fill_rect(82, (int16_t)(sel_y - 1), 18, MENU_SD_ROW_HILITE_H, INVERT);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_sd_draw_info_(void)
{
    sd_card_info_t info = {0};
    char line0[22];
    char line1[22];
    char line2[22];

    menu_sd_draw_frame_("SD INFO");

    if(sd_card_is_mounted())
    {
        info = sd_card_get_info();
        (void)snprintf(line0, sizeof(line0), "NAME: %.15s", (info.name != NULL) ? info.name : "-");
        (void)snprintf(line1, sizeof(line1), "TYPE: %.15s", (info.type != NULL) ? info.type : "-");
        (void)snprintf(line2, sizeof(line2), "SIZE: %lu MB", (unsigned long)info.total_space);
    }
    else
    {
        (void)snprintf(line0, sizeof(line0), "NAME: -");
        (void)snprintf(line1, sizeof(line1), "TYPE: -");
        (void)snprintf(line2, sizeof(line2), "CHECK CARD");
    }

    poom_arduboy_set_cursor(4, 18);
    (void)poom_arduboy_print(line0);
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, 38);
    (void)poom_arduboy_print(line2);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_sd_set_status_lines_(menu_sd_ctx_t* ctx, const char* line0, const char* line1)
{
    if(ctx == NULL)
    {
        return;
    }

    (void)snprintf(ctx->status_line0, sizeof(ctx->status_line0), "%s", (line0 != NULL) ? line0 : "");
    (void)snprintf(ctx->status_line1, sizeof(ctx->status_line1), "%s", (line1 != NULL) ? line1 : "");
}

static void menu_sd_set_mount_status_(menu_sd_ctx_t* ctx, esp_err_t err, bool format_flow)
{
    if(ctx == NULL)
    {
        return;
    }

    if(err == ESP_OK)
    {
        if(format_flow)
        {
            menu_sd_set_status_lines_(ctx, "SD READY", "FOLDERS CREATED");
        }
        else
        {
            menu_sd_set_status_lines_(ctx, "CARD OK", "READY TO USE");
        }
        return;
    }

    if(err == ESP_ERR_NOT_SUPPORTED)
    {
        menu_sd_set_status_lines_(ctx, "BAD FILESYSTEM", "USE FORMAT CARD");
        return;
    }

    if(err == ESP_ERR_NOT_FOUND)
    {
        menu_sd_set_status_lines_(ctx, "CANNOT ACCESS", "CHECK SD CARD");
        return;
    }

    if(format_flow)
    {
        menu_sd_set_status_lines_(ctx, "FORMAT FAILED", "TRY AGAIN");
    }
    else
    {
        menu_sd_set_status_lines_(ctx, "SD ERROR", "TRY AGAIN");
    }
}

static esp_err_t menu_sd_create_dir_(const char* path)
{
    return sd_card_create_dir(path);
}

static esp_err_t menu_sd_ensure_default_dirs_(void)
{
    esp_err_t err;

    err = menu_sd_create_dir_("/apps");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/nfc");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/nfc_dumps");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/portals");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/captive_data");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/pcaps");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/tones");
    if(err != ESP_OK)
    {
        return err;
    }
    err = menu_sd_create_dir_("/harmonies");
    if(err != ESP_OK)
    {
        return err;
    }
    return menu_sd_create_dir_("/poom_capture_snnifer");
}

static esp_err_t menu_sd_check_card_(void)
{
    esp_err_t err;

    sd_card_begin();
    if(sd_card_is_mounted())
    {
        err = sd_card_unmount();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    return sd_card_mount();
}

static esp_err_t menu_sd_prepare_info_(void)
{
    sd_card_begin();
    if(sd_card_is_mounted())
    {
        return ESP_OK;
    }

    return sd_card_mount();
}

static esp_err_t menu_sd_format_card_(void)
{
    esp_err_t err;

    sd_card_begin();
    err = sd_card_format();
    if(err != ESP_OK)
    {
        return err;
    }

    return menu_sd_ensure_default_dirs_();
}

static void menu_sd_launch_browser_(void)
{
    (void)poom_sd_browser_set_exit_callback(menu_sd_browser_exit_cb_, NULL);
    (void)poom_sd_browser_start();
}

static void menu_sd_browser_exit_cb_(void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    app_sd_browser_menu();
}

static void menu_sd_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_sd_ctx_t* ctx = menu_sd_ctx_();
    menu_sd_button_msg_t ev;

    (void)user_ctx;

    if((ctx == NULL) || (msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));
    if(ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(ctx->state == MENU_SD_STATE_MAIN)
    {
        if((ev.button == BTN_B) || (ev.button == BTN_LEFT))
        {
            ctx->exit_requested = true;
            return;
        }
        if(ev.button == BTN_UP)
        {
            if(ctx->opt == MENU_SD_OPT_BROWSE)
            {
                ctx->opt = (menu_sd_opt_t)(MENU_SD_OPT_COUNT - 1);
            }
            else
            {
                ctx->opt = (menu_sd_opt_t)((int)ctx->opt - 1);
            }
            return;
        }
        if(ev.button == BTN_DOWN)
        {
            ctx->opt = (menu_sd_opt_t)(((int)ctx->opt + 1) % (int)MENU_SD_OPT_COUNT);
            return;
        }
        if((ev.button == BTN_A) || (ev.button == BTN_RIGHT))
        {
            if(ctx->opt == MENU_SD_OPT_BROWSE)
            {
                ctx->browse_requested = true;
            }
            else if(ctx->opt == MENU_SD_OPT_CHECK)
            {
                ctx->check_requested = true;
            }
            else if(ctx->opt == MENU_SD_OPT_INFO)
            {
                ctx->info_requested = true;
            }
            else
            {
                ctx->confirm_yes = false;
                ctx->state = MENU_SD_STATE_CONFIRM;
            }
        }
        return;
    }

    if(ctx->state == MENU_SD_STATE_INFO)
    {
        if((ev.button == BTN_B) || (ev.button == BTN_LEFT) || (ev.button == BTN_A) || (ev.button == BTN_RIGHT))
        {
            ctx->state = MENU_SD_STATE_MAIN;
        }
        return;
    }

    if(ctx->state == MENU_SD_STATE_CONFIRM)
    {
        if((ev.button == BTN_UP) || (ev.button == BTN_DOWN))
        {
            ctx->confirm_yes = !ctx->confirm_yes;
            return;
        }
        if((ev.button == BTN_B) || (ev.button == BTN_LEFT))
        {
            ctx->confirm_yes = false;
            ctx->state = MENU_SD_STATE_MAIN;
            return;
        }
        if((ev.button == BTN_A) || (ev.button == BTN_RIGHT))
        {
            if(ctx->confirm_yes)
            {
                ctx->format_requested = true;
            }
            else
            {
                ctx->state = MENU_SD_STATE_MAIN;
            }
        }
        return;
    }

    if((ev.button == BTN_B) || (ev.button == BTN_LEFT) || (ev.button == BTN_A) || (ev.button == BTN_RIGHT))
    {
        ctx->state = MENU_SD_STATE_MAIN;
    }
}

static void menu_sd_task_(void* arg)
{
    menu_sd_ctx_t* ctx = (menu_sd_ctx_t*)arg;

    if(ctx == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    while(ctx->active)
    {
        esp_err_t err;

        if(ctx->exit_requested)
        {
            menu_sd_destroy_ctx_(true);
            vTaskDelete(NULL);
            return;
        }

        if(ctx->browse_requested)
        {
            menu_sd_destroy_ctx_(false);
            menu_sd_launch_browser_();
            vTaskDelete(NULL);
            return;
        }

        if(ctx->check_requested)
        {
            ctx->check_requested = false;
            menu_sd_set_status_lines_(ctx, "CHECKING SD...", "PLEASE WAIT");
            menu_sd_draw_result_(ctx);
            err = menu_sd_check_card_();
            menu_sd_set_mount_status_(ctx, err, false);
            ctx->state = MENU_SD_STATE_RESULT;
        }

        if(ctx->info_requested)
        {
            ctx->info_requested = false;
            menu_sd_set_status_lines_(ctx, "READING INFO...", "PLEASE WAIT");
            menu_sd_draw_result_(ctx);
            err = menu_sd_prepare_info_();
            if(err == ESP_OK)
            {
                ctx->state = MENU_SD_STATE_INFO;
            }
            else
            {
                menu_sd_set_mount_status_(ctx, err, false);
                ctx->state = MENU_SD_STATE_RESULT;
            }
        }

        if(ctx->format_requested)
        {
            ctx->format_requested = false;
            ctx->confirm_yes = false;
            menu_sd_set_status_lines_(ctx, "FORMATTING SD...", "PLEASE WAIT");
            menu_sd_draw_result_(ctx);
            err = menu_sd_format_card_();
            menu_sd_set_mount_status_(ctx, err, err == ESP_OK);
            ctx->state = MENU_SD_STATE_RESULT;
        }

        if(ctx->state == MENU_SD_STATE_INFO)
        {
            menu_sd_draw_info_();
        }
        else if(ctx->state == MENU_SD_STATE_CONFIRM)
        {
            menu_sd_draw_confirm_(ctx);
        }
        else if(ctx->state == MENU_SD_STATE_RESULT)
        {
            menu_sd_draw_result_(ctx);
        }
        else
        {
            ctx->state = MENU_SD_STATE_MAIN;
            menu_sd_draw_main_(ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_SD_REFRESH_MS));
    }

    menu_sd_destroy_ctx_(true);
    vTaskDelete(NULL);
}

void app_sd_browser_menu(void)
{
    if(!menu_sd_alloc_ctx_())
    {
        menu_sd_publish_resume_();
        return;
    }

    if(s_menu_sd->task != NULL)
    {
        return;
    }

    s_menu_sd->active = true;
    s_menu_sd->exit_requested = false;
    s_menu_sd->browse_requested = false;
    s_menu_sd->check_requested = false;
    s_menu_sd->info_requested = false;
    s_menu_sd->format_requested = false;
    s_menu_sd->state = MENU_SD_STATE_MAIN;
    s_menu_sd->opt = MENU_SD_OPT_BROWSE;
    s_menu_sd->confirm_yes = false;
    menu_sd_set_status_lines_(s_menu_sd, "", "");

    if(!s_menu_sd->buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_sd_button_cb_, MENU_SD_SBUS_USER))
        {
            s_menu_sd->buttons_subscribed = true;
        }
        else
        {
            menu_sd_destroy_ctx_(true);
            return;
        }
    }

    if(xTaskCreate(menu_sd_task_, "menu_sd", MENU_SD_STACK, s_menu_sd, MENU_SD_PRIO, &s_menu_sd->task) != pdPASS)
    {
        menu_sd_destroy_ctx_(true);
    }
}
