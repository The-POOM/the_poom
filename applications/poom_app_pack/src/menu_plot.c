// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_plot.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_ble_plot.h"
#include "poom_imu_plot.h"
#include "poom_imu_stream.h"
#include "poom_led_rainbow.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define MENU_PLOT_REFRESH_MS (60U)
#define MENU_PLOT_STACK (4096U)
#define MENU_PLOT_PRIO (4U)
#define MENU_PLOT_DEVICE_NAME "PPLOT"
#define MENU_PLOT_HEADER_H (11)
#define MENU_PLOT_BOX_Y (12)
#define MENU_PLOT_BOX_H (42)
#define MENU_PLOT_TILT_CX (108)
#define MENU_PLOT_TILT_CY (35)
#define MENU_PLOT_TILT_R (10)
#define MENU_PLOT_TILT_MG_PER_PIXEL (100)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

static bool s_plot_active = false;
static bool s_plot_buttons_subscribed = false;
static volatile bool s_plot_exit_requested = false;
static TaskHandle_t s_plot_task = NULL;
static imuplot_mode_t s_plot_mode = IMUPLOT_ACCEL_ONLY;

static void menu_plot_on_button_(const poom_sbus_msg_t *msg, void *user);
static void menu_plot_task_(void *arg);
static void menu_plot_exit_(void);

/**
 * @brief Internal helper for `menu_plot_round_i32`.
 *
 * @param[in] v Parameter passed to the helper.
 * @return int32_t
 */
static int32_t menu_plot_round_i32_(float v)
{
    if (v >= 0.0f)
    {
        return (int32_t)(v + 0.5f);
    }

    return (int32_t)(v - 0.5f);
}

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] mode Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_plot_mode_label_(imuplot_mode_t mode)
{
    switch (mode)
    {
        case IMUPLOT_ACCEL_ONLY:
            return "ACC";

        case IMUPLOT_GYRO_ONLY:
            return "GYR";

        case IMUPLOT_ACCEL_GYRO:
            return "6AX";

        default:
            return "UNK";
    }
}

/**
 * @brief Internal helper for `menu_plot_series_count`.
 *
 * @param[in] mode Parameter passed to the helper.
 * @return uint8_t
 */
static uint8_t menu_plot_series_count_(imuplot_mode_t mode)
{
    return (mode == IMUPLOT_ACCEL_GYRO) ? 6U : 3U;
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] ax_mg Parameter passed to the helper.
 * @param[in] ay_mg Parameter passed to the helper.
 * @return void
 */
static void menu_plot_draw_tilt_indicator_(int32_t ax_mg, int32_t ay_mg)
{
    int16_t dx = (int16_t)(ax_mg / MENU_PLOT_TILT_MG_PER_PIXEL);
    int16_t dy = (int16_t)(-ay_mg / MENU_PLOT_TILT_MG_PER_PIXEL);

    if (dx < -MENU_PLOT_TILT_R)
    {
        dx = -MENU_PLOT_TILT_R;
    }
    else if (dx > MENU_PLOT_TILT_R)
    {
        dx = MENU_PLOT_TILT_R;
    }

    if (dy < -MENU_PLOT_TILT_R)
    {
        dy = -MENU_PLOT_TILT_R;
    }
    else if (dy > MENU_PLOT_TILT_R)
    {
        dy = MENU_PLOT_TILT_R;
    }

    poom_arduboy_draw_circle(MENU_PLOT_TILT_CX, MENU_PLOT_TILT_CY, MENU_PLOT_TILT_R, WHITE);
    poom_arduboy_draw_fast_hline(MENU_PLOT_TILT_CX - MENU_PLOT_TILT_R,
                                 MENU_PLOT_TILT_CY,
                                 (2 * MENU_PLOT_TILT_R) + 1,
                                 WHITE);
    poom_arduboy_draw_fast_vline(MENU_PLOT_TILT_CX,
                                 MENU_PLOT_TILT_CY - MENU_PLOT_TILT_R,
                                 (2 * MENU_PLOT_TILT_R) + 1,
                                 WHITE);
    poom_arduboy_fill_rect(MENU_PLOT_TILT_CX + dx - 1,
                           MENU_PLOT_TILT_CY + dy - 1,
                           3,
                           3,
                           WHITE);
}

/**
 * @brief Internal helper for `menu_plot_prepare_values`.
 *
 * @param[in] data Parameter passed to the helper.
 * @param[in] mode Parameter passed to the helper.
 * @param[in] out_values Parameter passed to the helper.
 * @return size_t
 */
static size_t menu_plot_prepare_values_(const poom_imu_data_t *data,
                                        imuplot_mode_t mode,
                                        float *out_values)
{
    if ((data == NULL) || (out_values == NULL))
    {
        return 0U;
    }

    if (mode == IMUPLOT_GYRO_ONLY)
    {
        out_values[0] = data->angular_rate_mdps[0] / 1000.0f;
        out_values[1] = data->angular_rate_mdps[1] / 1000.0f;
        out_values[2] = data->angular_rate_mdps[2] / 1000.0f;
        return 3U;
    }

    out_values[0] = data->acceleration_mg[0] / 1000.0f;
    out_values[1] = data->acceleration_mg[1] / 1000.0f;
    out_values[2] = data->acceleration_mg[2] / 1000.0f;

    if (mode == IMUPLOT_ACCEL_GYRO)
    {
        out_values[3] = data->angular_rate_mdps[0] / 1000.0f;
        out_values[4] = data->angular_rate_mdps[1] / 1000.0f;
        out_values[5] = data->angular_rate_mdps[2] / 1000.0f;
        return 6U;
    }

    return 3U;
}

/**
 * @brief Renders the current menu state.
 *
 * @param[in] data Parameter passed to the helper.
 * @param[in] ble_connected Parameter passed to the helper.
 * @return void
 */
static void menu_plot_render_(const poom_imu_data_t *data, bool ble_connected)
{
    char line_ble[18];
    char line_mode[18];
    char line_data0[18];
    char line_data1[18];

    int32_t ax = 0;
    int32_t ay = 0;
    int32_t az = 0;
    int32_t gx = 0;
    int32_t gy = 0;
    int32_t gz = 0;

    if (data != NULL)
    {
        ax = menu_plot_round_i32_(data->acceleration_mg[0]);
        ay = menu_plot_round_i32_(data->acceleration_mg[1]);
        az = menu_plot_round_i32_(data->acceleration_mg[2]);
        gx = menu_plot_round_i32_(data->angular_rate_mdps[0] / 1000.0f);
        gy = menu_plot_round_i32_(data->angular_rate_mdps[1] / 1000.0f);
        gz = menu_plot_round_i32_(data->angular_rate_mdps[2] / 1000.0f);
    }

    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", ble_connected ? "LINK" : "WAIT");
    (void)snprintf(line_mode, sizeof(line_mode), "MODE:%s", menu_plot_mode_label_(s_plot_mode));

    if (s_plot_mode == IMUPLOT_GYRO_ONLY)
    {
        (void)snprintf(line_data0, sizeof(line_data0), "GX%+4ld GY%+4ld", (long)gx, (long)gy);
        (void)snprintf(line_data1, sizeof(line_data1), "GZ%+4ld dps", (long)gz);
    }
    else if (s_plot_mode == IMUPLOT_ACCEL_GYRO)
    {
        (void)snprintf(line_data0, sizeof(line_data0), "A%+4ld %4ld", (long)ax, (long)ay);
        (void)snprintf(line_data1, sizeof(line_data1), "G%+4ld %4ld", (long)gx, (long)gy);
    }
    else
    {
        (void)snprintf(line_data0, sizeof(line_data0), "AX%+4ld AY%+4ld", (long)ax, (long)ay);
        (void)snprintf(line_data1, sizeof(line_data1), "AZ%+4ld mg", (long)az);
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("POOM PLOT"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_PLOT_HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, MENU_PLOT_BOX_Y, ARDUBOY_WIDTH, MENU_PLOT_BOX_H, WHITE);

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line_ble);

    poom_arduboy_set_cursor(4, 24);
    (void)poom_arduboy_print(line_mode);

    poom_arduboy_set_cursor(4, 34);
    (void)poom_arduboy_print(line_data0);

    poom_arduboy_set_cursor(4, 44);
    (void)poom_arduboy_print(line_data1);
    menu_plot_draw_tilt_indicator_(ax, ay);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("U:A D:G R:6"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_plot_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_plot_active = false;
    s_plot_exit_requested = false;

    poom_ble_plot_stop();
    poom_led_rainbow_stop();

    if (s_plot_task != NULL)
    {
        if (s_plot_task != current_task)
        {
            TaskHandle_t task = s_plot_task;
            s_plot_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_plot_task = NULL;
        }
    }

    if (s_plot_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_plot_on_button_, "menu_plot");
        s_plot_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_plot_on_button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;

    if (!s_plot_active)
    {
        return;
    }

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BUTTON_B)
    {
        s_plot_exit_requested = true;
        return;
    }

    if (ev.button == BUTTON_UP)
    {
        s_plot_mode = IMUPLOT_ACCEL_ONLY;
    }
    else if (ev.button == BUTTON_DOWN)
    {
        s_plot_mode = IMUPLOT_GYRO_ONLY;
    }
    else if (ev.button == BUTTON_RIGHT)
    {
        s_plot_mode = IMUPLOT_ACCEL_GYRO;
    }
    else
    {
        return;
    }

    (void)poom_ble_plot_set_series_count(menu_plot_series_count_(s_plot_mode));
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_plot_task_(void *arg)
{
    poom_imu_data_t data = {0};

    (void)arg;

    (void)poom_ble_plot_set_series_count(menu_plot_series_count_(s_plot_mode));

    while (s_plot_active)
    {
        bool ble_connected;
        bool sample_ready;

        if (s_plot_exit_requested)
        {
            menu_plot_exit_();
            break;
        }

        sample_ready = poom_imu_stream_read_data(&data);
        ble_connected = poom_ble_plot_is_connected();

        menu_plot_render_(&data, ble_connected);

        if (sample_ready && ble_connected)
        {
            float values[6];
            size_t count = menu_plot_prepare_values_(&data, s_plot_mode, values);
            if (count > 0U)
            {
                (void)poom_ble_plot_send_line(values, count);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_PLOT_REFRESH_MS));
    }

    s_plot_task = NULL;
    vTaskDelete(NULL);
}

void menu_plot_init(void)
{
    s_plot_active = true;
    s_plot_exit_requested = false;
    s_plot_mode = IMUPLOT_ACCEL_ONLY;

    if (!s_plot_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_plot_on_button_, "menu_plot"))
        {
            s_plot_buttons_subscribed = true;
        }
        else
        {
            s_plot_active = false;
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    poom_imu_stream_init();
    if (poom_ble_plot_init(MENU_PLOT_DEVICE_NAME) == 0)
    {
        poom_ble_plot_set_format(',', 2);
    }

    menu_plot_render_(NULL, false);

    if (xTaskCreate(menu_plot_task_,
                    "menu_plot",
                    MENU_PLOT_STACK,
                    NULL,
                    MENU_PLOT_PRIO,
                    &s_plot_task) != pdPASS)
    {
        menu_plot_exit_();
    }
}
