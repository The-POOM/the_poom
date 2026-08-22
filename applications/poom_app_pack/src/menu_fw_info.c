// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_fw_info.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_FW_INFO_REFRESH_MS (180U)
#define MENU_FW_INFO_STACK (3072U)
#define MENU_FW_INFO_PRIO (4U)

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_fw_info_button_msg_t;

static bool s_fw_info_active = false;
static bool s_fw_info_buttons_subscribed = false;
static volatile bool s_fw_info_exit_requested = false;
static TaskHandle_t s_fw_info_task = NULL;
static char s_fw_info_sbus_user[] = "menu_fw_info";

static void menu_fw_info_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_fw_info_task_(void *arg);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void fw_info_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(36, 2);
    (void)poom_arduboy_print(F("FW INFO"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void fw_info_draw_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];

    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = (desc != NULL) ? desc->version : "unknown";
    const char *date = (desc != NULL) ? desc->date : "";
    const char *time = (desc != NULL) ? desc->time : "";

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const char *run_label = (run != NULL) ? run->label : "?";
    const char *boot_label = (boot != NULL) ? boot->label : "?";

    (void)snprintf(line1, sizeof(line1), "VER:%.16s", ver ? ver : "");
    if ((date[0] != '\0') && (time[0] != '\0'))
    {
        (void)snprintf(line2, sizeof(line2), "BLD:%.6s %.5s", date, time);
    }
    else
    {
        (void)snprintf(line2, sizeof(line2), "BLD:<unknown>");
    }
    (void)snprintf(line3, sizeof(line3), "RUN:%.16s", run_label);
    (void)snprintf(line4, sizeof(line4), "BOOT:%.15s", boot_label);

    poom_arduboy_clear();
    fw_info_draw_header_();

    poom_arduboy_set_cursor(0, 14);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(0, 24);
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(0, 34);
    (void)poom_arduboy_print(line3);
    poom_arduboy_set_cursor(0, 44);
    (void)poom_arduboy_print(line4);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_fw_info_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_fw_info_active = false;
    s_fw_info_exit_requested = false;

    if (s_fw_info_task != NULL)
    {
        if (s_fw_info_task != current_task)
        {
            TaskHandle_t task = s_fw_info_task;
            s_fw_info_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_fw_info_task = NULL;
        }
    }

    if (s_fw_info_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_fw_info_button_cb_, s_fw_info_sbus_user);
        s_fw_info_buttons_subscribed = false;
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_fw_info_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(menu_fw_info_button_msg_t)))
    {
        return;
    }

    menu_fw_info_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BTN_B)
    {
        s_fw_info_exit_requested = true;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_fw_info_task_(void *arg)
{
    (void)arg;

    while (s_fw_info_active)
    {
        if (s_fw_info_exit_requested)
        {
            menu_fw_info_exit_();
            break;
        }

        fw_info_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_FW_INFO_REFRESH_MS));
    }

    s_fw_info_task = NULL;
    vTaskDelete(NULL);
}

void menu_fw_info_show(void)
{
    if (s_fw_info_task != NULL)
    {
        return;
    }

    s_fw_info_active = true;
    s_fw_info_exit_requested = false;

    if (!s_fw_info_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_fw_info_button_cb_, s_fw_info_sbus_user))
        {
            s_fw_info_buttons_subscribed = true;
        }
        else
        {
            s_fw_info_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_fw_info_task_, "menu_fw_info", MENU_FW_INFO_STACK, NULL, MENU_FW_INFO_PRIO, &s_fw_info_task);
}
