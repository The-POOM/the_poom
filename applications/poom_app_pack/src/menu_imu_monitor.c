// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_imu_monitor.h"

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
#include "poom_sbus.h"

#include "poom_imu_stream.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_IMU_REFRESH_MS (180U)
#define MENU_IMU_STACK (3072U)
#define MENU_IMU_PRIO (4U)

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
} menu_imu_button_msg_t;

static bool s_imu_active = false;
static bool s_imu_buttons_subscribed = false;
static volatile bool s_imu_exit_requested = false;
static TaskHandle_t s_imu_task = NULL;
static char s_imu_sbus_user[] = "menu_imu_monitor";

static void menu_imu_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_imu_task_(void* arg);

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void imu_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(46, 2);
    (void)poom_arduboy_print(F("IMU"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Internal helper for `imu_round_i32`.
 *
 * @param[in] v Parameter passed to the helper.
 * @return int32_t
 */
static int32_t imu_round_i32_(float v)
{
    if (v >= 0.0f)
    {
        return (int32_t)(v + 0.5f);
    }
    return (int32_t)(v - 0.5f);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] d Parameter passed to the helper.
 * @return void
 */
static void imu_draw_(const poom_imu_data_t* d)
{
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];

    int32_t ax = 0;
    int32_t ay = 0;
    int32_t az = 0;
    int32_t gx = 0;
    int32_t gy = 0;
    int32_t gz = 0;
    float temp_c = 0.0f;

    if (d != NULL)
    {
        ax = imu_round_i32_(d->acceleration_mg[0]);
        ay = imu_round_i32_(d->acceleration_mg[1]);
        az = imu_round_i32_(d->acceleration_mg[2]);
        gx = imu_round_i32_(d->angular_rate_mdps[0] / 1000.0f);
        gy = imu_round_i32_(d->angular_rate_mdps[1] / 1000.0f);
        gz = imu_round_i32_(d->angular_rate_mdps[2] / 1000.0f);
        temp_c = d->temperature_degC;
    }

    (void)snprintf(line1, sizeof(line1), "AX%+6ld AY%+6ld", (long)ax, (long)ay);
    (void)snprintf(line2, sizeof(line2), "AZ%+6ld mg", (long)az);
    (void)snprintf(line3, sizeof(line3), "GX%+6ld GY%+6ld", (long)gx, (long)gy);
    (void)snprintf(line4, sizeof(line4), "GZ%+6ld T%4.1fC", (long)gz, (double)temp_c);

    poom_arduboy_clear();
    imu_draw_header_();

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
static void menu_imu_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_imu_active = false;
    s_imu_exit_requested = false;

    if (s_imu_task != NULL)
    {
        if (s_imu_task != current_task)
        {
            TaskHandle_t task = s_imu_task;
            s_imu_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_imu_task = NULL;
        }
    }

    if (s_imu_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_imu_button_cb_, s_imu_sbus_user);
        s_imu_buttons_subscribed = false;
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
static void menu_imu_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(menu_imu_button_msg_t)))
    {
        return;
    }

    menu_imu_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BTN_B)
    {
        s_imu_exit_requested = true;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_imu_task_(void* arg)
{
    (void)arg;

    poom_imu_stream_init();

    poom_imu_data_t d = {0};

    while (s_imu_active)
    {
        if (s_imu_exit_requested)
        {
            menu_imu_exit_();
            break;
        }

        (void)poom_imu_stream_read_data(&d);
        imu_draw_(&d);

        vTaskDelay(pdMS_TO_TICKS(MENU_IMU_REFRESH_MS));
    }

    s_imu_task = NULL;
    vTaskDelete(NULL);
}

void menu_imu_monitor_show(void)
{
    if (s_imu_task != NULL)
    {
        return;
    }

    s_imu_active = true;
    s_imu_exit_requested = false;

    if (!s_imu_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_imu_button_cb_, s_imu_sbus_user))
        {
            s_imu_buttons_subscribed = true;
        }
        else
        {
            s_imu_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_imu_task_, "menu_imu", MENU_IMU_STACK, NULL, MENU_IMU_PRIO, &s_imu_task);
}
