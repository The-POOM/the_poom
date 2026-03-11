// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file button_driver.c
 * @brief Physical button input driver that publishes events through SBUS.
 */

#include "button_driver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "poom_sbus.h"

#include "input_events.h"

#define BUTTON_TOPIC_ALL                    "input/button"
#define BUTTON_IDLE_TIMEOUT_SECONDS         (30U)
#define BUTTON_EVENT_BUTTON_SHIFT           (4U)
#define BUTTON_EVENT_TYPE_MASK              (0x0FU)
#define BUTTON_EVENT_ARG_MASK               (0xFFU)
#define BUTTON_PER_BUTTON_TOPIC_COUNT       (6U)

#ifndef ZBUS_PUBLISH_PER_BUTTON
#define ZBUS_PUBLISH_PER_BUTTON             (0)
#endif

static const char *BUTTON_DRIVER_TAG = "button_driver";

#if defined(CONFIG_BUTTON_DRIVER_ENABLE_LOG) && CONFIG_BUTTON_DRIVER_ENABLE_LOG

#define BUTTON_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", BUTTON_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define BUTTON_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", BUTTON_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define BUTTON_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", BUTTON_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define BUTTON_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", BUTTON_DRIVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define BUTTON_PRINTF_E(...)
#define BUTTON_PRINTF_W(...)
#define BUTTON_PRINTF_I(...)
#define BUTTON_PRINTF_D(...)

#endif

typedef struct
{
    uint32_t pin;
    uint8_t mask;
} button_map_t;

static const button_map_t s_button_map[] = {
    {A_BUTTON_PIN, A_BUTTON_MASK},
    {B_BUTTON_PIN, B_BUTTON_MASK},
    {LEFT_BUTTON_PIN, LEFT_BUTTON_MASK},
    {RIGHT_BUTTON_PIN, RIGHT_BUTTON_MASK},
    {UP_BUTTON_PIN, UP_BUTTON_MASK},
    {DOWN_BUTTON_PIN, DOWN_BUTTON_MASK},
};

static const button_event_t s_registered_events[] = {
    BUTTON_PRESS_DOWN,
    BUTTON_PRESS_UP,
    BUTTON_PRESS_REPEAT,
    BUTTON_PRESS_REPEAT_DONE,
    BUTTON_SINGLE_CLICK,
    BUTTON_DOUBLE_CLICK,
    BUTTON_LONG_PRESS_START,
    BUTTON_LONG_PRESS_HOLD,
    BUTTON_LONG_PRESS_UP,
};

#if ZBUS_PUBLISH_PER_BUTTON
static const char *s_button_topics[BUTTON_PER_BUTTON_TOPIC_COUNT] = {
    "input/button/A",
    "input/button/B",
    "input/button/LEFT",
    "input/button/RIGHT",
    "input/button/UP",
    "input/button/DOWN",
};
#endif

static input_callback_t s_input_callback = NULL;
static esp_timer_handle_t s_idle_timer = NULL;
static bool s_lock_input = false;

static void button_event_cb(void *arg, void *data);

/**
 * @brief Return current system time in milliseconds.
 */
static uint32_t button_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief Idle timer callback placeholder.
 */
static void button_idle_timer_cb_(void *arg)
{
    (void)arg;
}

/**
 * @brief Decode button metadata from callback arg payload.
 */
static void button_decode_event_(void *data, uint8_t *button_name, uint8_t *button_event)
{
    uint8_t payload = (uint8_t)((uintptr_t)data & BUTTON_EVENT_ARG_MASK);

    if ((button_name == NULL) || (button_event == NULL))
    {
        return;
    }

    *button_name = (uint8_t)(payload >> BUTTON_EVENT_BUTTON_SHIFT);
    *button_event = (uint8_t)(payload & BUTTON_EVENT_TYPE_MASK);
}

/**
 * @brief Register all configured event types for one hardware button.
 */
static void button_register_callbacks_(button_handle_t button, uint8_t mask)
{
    size_t i = 0U;
    esp_err_t err = ESP_OK;

    for (i = 0U; i < (sizeof(s_registered_events) / sizeof(s_registered_events[0])); i++)
    {
        button_event_t payload = (button_event_t)(s_registered_events[i] | mask);
        err |= iot_button_register_cb(button, s_registered_events[i], button_event_cb, (void *)(uintptr_t)payload);
    }

    ESP_ERROR_CHECK(err);
}

/**
 * @brief Initialize one physical button instance.
 */
static void button_init_(uint32_t button_num, uint8_t mask)
{
    button_config_t btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        .gpio_button_config =
            {
                .gpio_num = button_num,
                .active_level = BUTTON_ACTIVE_LEVEL,
            },
    };
    button_handle_t btn = iot_button_create(&btn_cfg);

    assert(btn != NULL);
    button_register_callbacks_(btn, mask);
}

/**
 * @brief Main callback that publishes button events through SBUS.
 */
static void button_event_cb(void *arg, void *data)
{
    button_event_msg_t event_msg = {0};
    uint8_t button_name = 0U;
    uint8_t button_event = 0U;

    (void)arg;

    button_decode_event_(data, &button_name, &button_event);
    if (s_lock_input)
    {
        return;
    }

    event_msg.button = button_name;
    event_msg.event = button_event;
    event_msg.ts_ms = button_now_ms_();

    (void)poom_sbus_publish(BUTTON_TOPIC_ALL, &event_msg, sizeof(event_msg), 0U);

#if ZBUS_PUBLISH_PER_BUTTON
    if (button_name < BUTTON_PER_BUTTON_TOPIC_COUNT)
    {
        (void)poom_sbus_publish(s_button_topics[button_name], &event_msg, sizeof(event_msg), 0U);
    }
#endif

    if (s_input_callback != NULL)
    {
        s_input_callback(button_name, button_event);
    }
}

void button_module_reset_idle_timer(void)
{
    if (s_idle_timer == NULL)
    {
        return;
    }

    (void)esp_timer_stop(s_idle_timer);
    (void)esp_timer_start_once(s_idle_timer, (uint64_t)BUTTON_IDLE_TIMEOUT_SECONDS * 1000000ULL);
}

void button_module_set_lock(bool lock)
{
    s_lock_input = lock;
}

void button_module_begin(void)
{
    size_t i = 0U;

    (void)poom_sbus_register_topic(BUTTON_TOPIC_ALL);

#if ZBUS_PUBLISH_PER_BUTTON
    for (i = 0U; i < BUTTON_PER_BUTTON_TOPIC_COUNT; i++)
    {
        (void)poom_sbus_register_topic(s_button_topics[i]);
    }
#endif

    if (s_idle_timer == NULL)
    {
        const esp_timer_create_args_t timer_cfg = {
            .callback = button_idle_timer_cb_,
            .arg = NULL,
            .name = "btn_idle",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_err_t err = esp_timer_create(&timer_cfg, &s_idle_timer);
        if (err != ESP_OK)
        {
            BUTTON_PRINTF_E("idle timer create failed (%d)", (int)err);
        }
        else
        {
            button_module_reset_idle_timer();
        }
    }

    for (i = 0U; i < (sizeof(s_button_map) / sizeof(s_button_map[0])); i++)
    {
        button_init_(s_button_map[i].pin, s_button_map[i].mask);
    }
}
