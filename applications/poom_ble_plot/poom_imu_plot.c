// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_imu_plot.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "poom_ble_plot.h"
#include "poom_imu_stream.h"
#include "poom_sbus.h"

/**
 * @file poom_imu_plot.c
 * @brief IMU to BLE-plot bridge.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */

#if IMUPLOT_LOG_ENABLED
static const char *IMUPLOT_LOG_TAG = "imu_plot";

#define IMUPLOT_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", IMUPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define IMUPLOT_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", IMUPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define IMUPLOT_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", IMUPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#if IMUPLOT_DEBUG_LOG_ENABLED
#define IMUPLOT_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", IMUPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define IMUPLOT_PRINTF_D(...) do { } while (0)
#endif
#else
#define IMUPLOT_PRINTF_E(...) do { } while (0)
#define IMUPLOT_PRINTF_W(...) do { } while (0)
#define IMUPLOT_PRINTF_I(...) do { } while (0)
#define IMUPLOT_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define IMUPLOT_BUTTON_EVENT_SINGLE_CLICK      (4U)
#define IMUPLOT_BUTTON_RIGHT                   (3U)
#define IMUPLOT_BUTTON_UP                      (4U)
#define IMUPLOT_BUTTON_DOWN                    (5U)
#define IMUPLOT_BUTTON_TOPIC                   "input/button"
#define IMUPLOT_BUTTON_SUBSCRIBER_ID           "poom_imu_plot"

#define IMUPLOT_PERIOD_DEFAULT_MS              (20U)
#define IMUPLOT_PERIOD_BUTTON_SWITCH_MS        (50U)
#define IMUPLOT_TASK_STACK_SIZE                (4096U)
#define IMUPLOT_TASK_PRIORITY                  (tskIDLE_PRIORITY + 2)
#define IMUPLOT_TASK_NAME                      "poom_imu_plot_task"

#define IMUPLOT_VECTOR3_LEN                    (3U)
#define IMUPLOT_VECTOR6_LEN                    (6U)
#define IMUPLOT_DEFAULT_DEVICE_NAME            "POOM-BLE-PLOT"

#define IMUPLOT_MG_TO_G                        (1.0f / 1000.0f)
#define IMUPLOT_MDPS_TO_DPS                    (1.0f / 1000.0f)

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} button_event_msg_t;

/* =========================
 * Local state
 * ========================= */
static TaskHandle_t s_task = NULL;
static imuplot_mode_t s_mode = IMUPLOT_ACCEL_ONLY;
static uint32_t s_period_ms = IMUPLOT_PERIOD_DEFAULT_MS;
static bool s_buttons_subscribed = false;

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Handles button topic events used to switch plotting mode.
 *
 * @param msg Incoming SBUS message.
 * @param user User context (unused).
 */
static void on_button_any(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t ev;

    (void)user;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event == IMUPLOT_BUTTON_EVENT_SINGLE_CLICK)
    {
        if (ev.button == IMUPLOT_BUTTON_RIGHT)
        {
            poom_ble_plot_imu_stop();
            (void)poom_ble_plot_imu_start(IMUPLOT_ACCEL_GYRO, IMUPLOT_PERIOD_BUTTON_SWITCH_MS);
        }
        else if (ev.button == IMUPLOT_BUTTON_UP)
        {
            poom_ble_plot_imu_stop();
            (void)poom_ble_plot_imu_start(IMUPLOT_ACCEL_ONLY, IMUPLOT_PERIOD_BUTTON_SWITCH_MS);
        }
        else if (ev.button == IMUPLOT_BUTTON_DOWN)
        {
            poom_ble_plot_imu_stop();
            (void)poom_ble_plot_imu_start(IMUPLOT_GYRO_ONLY, IMUPLOT_PERIOD_BUTTON_SWITCH_MS);
        }
        else
        {
            /* no-op */
        }
    }
}

/**
 * @brief Worker task that reads IMU data and publishes it over BLE plot.
 *
 * @param arg Task argument (unused).
 */
static void poom_imu_plot_task(void *arg)
{
    poom_imu_data_t data;

    (void)arg;

    switch (s_mode)
    {
        case IMUPLOT_ACCEL_ONLY:
            (void)poom_ble_plot_set_series_count(IMUPLOT_VECTOR3_LEN);
            break;

        case IMUPLOT_GYRO_ONLY:
            (void)poom_ble_plot_set_series_count(IMUPLOT_VECTOR3_LEN);
            break;

        case IMUPLOT_ACCEL_GYRO:
            (void)poom_ble_plot_set_series_count(IMUPLOT_VECTOR6_LEN);
            break;

        default:
            (void)poom_ble_plot_set_series_count(IMUPLOT_VECTOR3_LEN);
            break;
    }

    while (1)
    {
        if (!poom_imu_stream_read_data(&data))
        {
            vTaskDelay(pdMS_TO_TICKS(s_period_ms));
            continue;
        }

        {
            float ax = data.acceleration_mg[0] * IMUPLOT_MG_TO_G;
            float ay = data.acceleration_mg[1] * IMUPLOT_MG_TO_G;
            float az = data.acceleration_mg[2] * IMUPLOT_MG_TO_G;

            float gx = data.angular_rate_mdps[0] * IMUPLOT_MDPS_TO_DPS;
            float gy = data.angular_rate_mdps[1] * IMUPLOT_MDPS_TO_DPS;
            float gz = data.angular_rate_mdps[2] * IMUPLOT_MDPS_TO_DPS;

            if (poom_ble_plot_is_connected())
            {
                if (s_mode == IMUPLOT_ACCEL_ONLY)
                {
                    float values[IMUPLOT_VECTOR3_LEN] = {ax, ay, az};
                    (void)poom_ble_plot_send_line(values, IMUPLOT_VECTOR3_LEN);
                }
                else if (s_mode == IMUPLOT_GYRO_ONLY)
                {
                    float values[IMUPLOT_VECTOR3_LEN] = {gx, gy, gz};
                    (void)poom_ble_plot_send_line(values, IMUPLOT_VECTOR3_LEN);
                }
                else
                {
                    float values[IMUPLOT_VECTOR6_LEN] = {ax, ay, az, gx, gy, gz};
                    (void)poom_ble_plot_send_line(values, IMUPLOT_VECTOR6_LEN);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }
}

/**
 * @brief Subscribes to all required input button topics.
 */
static void buttons_subscribe_all(void)
{
    if (!s_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb(IMUPLOT_BUTTON_TOPIC, on_button_any, (void *)IMUPLOT_BUTTON_SUBSCRIBER_ID))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            IMUPLOT_PRINTF_W("Failed to subscribe to %s", IMUPLOT_BUTTON_TOPIC);
        }
    }
}

/**
 * @brief Unsubscribes from input button topic.
 */
static void buttons_unsubscribe_all(void)
{
    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(IMUPLOT_BUTTON_TOPIC, on_button_any, (void *)IMUPLOT_BUTTON_SUBSCRIBER_ID);
        s_buttons_subscribed = false;
    }
}

/* =========================
 * Public API
 * ========================= */
/**
 * @brief Initializes BLE plot transport and output format.
 *
 * @param device_name BLE device name, or NULL for default name.
 * @param sep Output separator.
 * @param precision Output decimal precision.
 * @return true on success, false on failure.
 */
bool poom_ble_plot_imu_init(const char *device_name,
                  char sep,
                  uint8_t precision)
{
    const char *plot_name = (device_name != NULL) ? device_name : IMUPLOT_DEFAULT_DEVICE_NAME;

    if (poom_ble_plot_init(plot_name) != 0)
    {
        IMUPLOT_PRINTF_E("poom_ble_plot_init failed");
        return false;
    }

    poom_ble_plot_set_format(sep, precision);
    return true;
}

/**
 * @brief Starts the IMU plotting task.
 *
 * @param mode Plot mode.
 * @param period_ms Task period in milliseconds.
 * @return true if task was created, false otherwise.
 */
bool poom_ble_plot_imu_start(imuplot_mode_t mode,
                   uint32_t period_ms)
{
    BaseType_t ok;

    if (s_task != NULL)
    {
        IMUPLOT_PRINTF_W("Task is already running");
        return false;
    }

    s_mode = mode;
    s_period_ms = (period_ms == 0U) ? IMUPLOT_PERIOD_DEFAULT_MS : period_ms;

    buttons_subscribe_all();

    ok = xTaskCreate(poom_imu_plot_task,
                     IMUPLOT_TASK_NAME,
                     IMUPLOT_TASK_STACK_SIZE,
                     NULL,
                     IMUPLOT_TASK_PRIORITY,
                     &s_task);

    if (ok != pdPASS)
    {
        IMUPLOT_PRINTF_E("Failed to create task");
        buttons_unsubscribe_all();
        s_task = NULL;
        return false;
    }

    IMUPLOT_PRINTF_I("Task started (mode=%d, period_ms=%u)", (int)s_mode, (unsigned)s_period_ms);
    return true;
}

/**
 * @brief Stops the IMU plotting task and unsubscribes button events.
 */
void poom_ble_plot_imu_stop(void)
{
    buttons_unsubscribe_all();

    if (s_task != NULL)
    {
        TaskHandle_t task = s_task;
        s_task = NULL;
        vTaskDelete(task);
        IMUPLOT_PRINTF_I("Task stopped");
    }
}

/**
 * @brief Returns whether the plotting task is running.
 *
 * @return true if running.
 */
bool poom_ble_plot_imu_is_running(void)
{
    return (s_task != NULL);
}
