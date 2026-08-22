// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_button_sound.c
 * @brief Optional "click" sound when buttons are pressed (SBUS input/button).
 *
 * Designed to be started/stopped around app lifecycle to avoid peripheral conflicts.
 */

#include "poom_button_sound.h"

#include <stdint.h>
#include <string.h>

#include "bsp_pong.h"
#include "buzzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_secrets_store.h"
#include "poom_sbus.h"

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN (0U)
#endif

#define POOM_BTN_SOUND_TOPIC          "input/button"
#define POOM_BTN_SOUND_SECRETS_KEY    "btn_sound"

#define POOM_BTN_SOUND_TASK_STACK     (2048)
#define POOM_BTN_SOUND_TASK_PRIO      (4)
#define POOM_BTN_SOUND_Q_LEN          (8U)

#define POOM_BTN_SOUND_FREQ_HZ        (4200U)
#define POOM_BTN_SOUND_DUR_MS         (20U)

typedef struct
{
    uint8_t button;
    uint32_t ts_ms;
} poom_btn_sound_req_t;

static bool s_initialized = false;
static bool s_enabled_setting = false;
static bool s_suspended = false;
static bool s_subscribed = false;

static TaskHandle_t s_task = NULL;
static QueueHandle_t s_q = NULL;

/**
 * @brief Internal helper for `poom_btn_sound_read_enabled`.
 *
 * @param[in] out_enabled Parameter passed to the function.
 * @return bool
 */
static bool poom_btn_sound_read_enabled_(bool *out_enabled)
{
    if (out_enabled == NULL)
    {
        return false;
    }

    if (poom_secrets_init() != ESP_OK)
    {
        return false;
    }

    uint32_t v = 0U;
    if (poom_secrets_get_u32(POOM_BTN_SOUND_SECRETS_KEY, &v) != ESP_OK)
    {
        *out_enabled = false;
        return true;
    }

    *out_enabled = (v != 0U);
    return true;
}

/**
 * @brief Internal helper for `poom_btn_sound_write_enabled`.
 *
 * @param[in] enabled Parameter passed to the function.
 * @return bool
 */
static bool poom_btn_sound_write_enabled_(bool enabled)
{
    if (poom_secrets_init() != ESP_OK)
    {
        return false;
    }

    return (poom_secrets_set_u32(POOM_BTN_SOUND_SECRETS_KEY, enabled ? 1U : 0U) == ESP_OK);
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_btn_sound_task_(void *arg)
{
    (void)arg;

    buzzer_init(PIN_NUM_BUZZER);

    for (;;)
    {
        poom_btn_sound_req_t req;
        if (xQueueReceive(s_q, &req, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        (void)req;
        buzzer_tone(POOM_BTN_SOUND_FREQ_HZ, POOM_BTN_SOUND_DUR_MS);
    }
}

/**
 * @brief Handles button events for this module.
 *
 * @param[in] msg Parameter passed to the function.
 * @param[in] user Parameter passed to the function.
 * @return void
 */
static void poom_btn_sound_on_button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)) || (s_q == NULL))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_PRESS_DOWN)
    {
        return;
    }

    poom_btn_sound_req_t req = {.button = ev.button, .ts_ms = ev.ts_ms};
    (void)xQueueSend(s_q, &req, 0);
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return void
 */
static void poom_btn_sound_start_(void)
{
    if (s_task != NULL)
    {
        return;
    }

    if (s_q == NULL)
    {
        s_q = xQueueCreate(POOM_BTN_SOUND_Q_LEN, sizeof(poom_btn_sound_req_t));
        if (s_q == NULL)
        {
            return;
        }
    }

    if (xTaskCreate(poom_btn_sound_task_,
                    "btn_sound",
                    POOM_BTN_SOUND_TASK_STACK,
                    NULL,
                    POOM_BTN_SOUND_TASK_PRIO,
                    &s_task) != pdPASS)
    {
        s_task = NULL;
        return;
    }

    if (!s_subscribed)
    {
        if (poom_sbus_subscribe_cb(POOM_BTN_SOUND_TOPIC, poom_btn_sound_on_button_, NULL))
        {
            s_subscribed = true;
        }
    }
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_btn_sound_stop_(void)
{
    if (s_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(POOM_BTN_SOUND_TOPIC, poom_btn_sound_on_button_, NULL);
        s_subscribed = false;
    }

    if (s_task != NULL)
    {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_q != NULL)
    {
        xQueueReset(s_q);
    }
}

void poom_button_sound_init(void)
{
    if (s_initialized)
    {
        return;
    }
    s_initialized = true;

    bool enabled = false;
    if (!poom_btn_sound_read_enabled_(&enabled))
    {
        enabled = false;
    }
    s_enabled_setting = enabled;

    s_suspended = false;
    if (s_enabled_setting)
    {
        poom_btn_sound_start_();
    }
}

bool poom_button_sound_get_enabled_setting(void)
{
    return s_enabled_setting;
}

bool poom_button_sound_set_enabled_setting(bool enabled)
{
    poom_button_sound_init();

    if (!poom_btn_sound_write_enabled_(enabled))
    {
        return false;
    }

    s_enabled_setting = enabled;

    if (!s_enabled_setting)
    {
        poom_btn_sound_stop_();
        return true;
    }

    if (!s_suspended)
    {
        poom_btn_sound_start_();
    }
    return true;
}

void poom_button_sound_suspend(void)
{
    poom_button_sound_init();
    s_suspended = true;
    poom_btn_sound_stop_();
}

void poom_button_sound_resume(void)
{
    poom_button_sound_init();
    s_suspended = false;
    if (s_enabled_setting)
    {
        poom_btn_sound_start_();
    }
}
