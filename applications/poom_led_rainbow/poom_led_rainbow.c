// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_led_rainbow.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "bsp_pong.h"
#include "ws2812.h"

/**
 * @file poom_led_rainbow.c
 * @brief Rainbow animation over WS2812 strip.
 */

#define POOM_LED_RAINBOW_TASK_NAME               "poom_led_rainbow"
#define POOM_LED_RAINBOW_TASK_STACK_SIZE         (3072U)
#define POOM_LED_RAINBOW_TASK_PRIORITY           (6U)

#define POOM_LED_RAINBOW_RESOLUTION_HZ           (10 * 1000 * 1000)
#define POOM_LED_RAINBOW_IS_RGBW                 (false)

#define POOM_LED_RAINBOW_INIT_BRIGHTNESS         (30U)
#define POOM_LED_RAINBOW_ANIM_BRIGHTNESS         (64U)
#define POOM_LED_RAINBOW_ANIM_DELAY_MS           (20U)
#define POOM_LED_RAINBOW_BOOT_BLINK_COUNT        (2U)
#define POOM_LED_RAINBOW_BOOT_BLINK_DELAY_MS     (150U)
#define POOM_LED_RAINBOW_BOOT_LED_INDEX          (0U)

#define POOM_LED_RAINBOW_COLOR_MAX               (255U)
#define POOM_LED_RAINBOW_WHEEL_STEP              (3U)
#define POOM_LED_RAINBOW_WHEEL_SPLIT_1           (85U)
#define POOM_LED_RAINBOW_WHEEL_SPLIT_2           (170U)

static ws2812_strip_t s_strip;
static TaskHandle_t s_led_task = NULL;
static bool s_initialized = false;

/**
 * @brief Converts wheel position into RGB color.
 */
static inline void poom_led_rainbow_wheel_(uint8_t pos,
                                           uint8_t *r,
                                           uint8_t *g,
                                           uint8_t *b)
{
    if (pos < POOM_LED_RAINBOW_WHEEL_SPLIT_1)
    {
        *r = POOM_LED_RAINBOW_COLOR_MAX - (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
        *g = (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
        *b = 0U;
    }
    else if (pos < POOM_LED_RAINBOW_WHEEL_SPLIT_2)
    {
        pos = (uint8_t)(pos - POOM_LED_RAINBOW_WHEEL_SPLIT_1);
        *r = 0U;
        *g = POOM_LED_RAINBOW_COLOR_MAX - (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
        *b = (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
    }
    else
    {
        pos = (uint8_t)(pos - POOM_LED_RAINBOW_WHEEL_SPLIT_2);
        *r = (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
        *g = 0U;
        *b = POOM_LED_RAINBOW_COLOR_MAX - (uint8_t)(pos * POOM_LED_RAINBOW_WHEEL_STEP);
    }
}

/**
 * @brief 8-bit scale helper.
 */
static inline uint8_t poom_led_rainbow_scale8_(uint8_t value, uint8_t scale)
{
    return (uint8_t)(((uint16_t)value * (uint16_t)scale) / POOM_LED_RAINBOW_COLOR_MAX);
}

/**
 * @brief Turns off all LEDs and pushes frame.
 */
static void poom_led_rainbow_clear_all_(void)
{
    for (int i = 0; i < s_strip.led_count; ++i)
    {
        ws2812_set_pixel(&s_strip, i, 0U, 0U, 0U, 0U);
    }
    (void)ws2812_show(&s_strip);
}

/**
 * @brief Optional boot blink feedback.
 */
static void poom_led_rainbow_boot_blink_(void)
{
    for (uint8_t i = 0; i < POOM_LED_RAINBOW_BOOT_BLINK_COUNT; ++i)
    {
        ws2812_set_pixel(&s_strip, POOM_LED_RAINBOW_BOOT_LED_INDEX, 0U, 0U, POOM_LED_RAINBOW_COLOR_MAX, 0U);
        (void)ws2812_show(&s_strip);
        vTaskDelay(pdMS_TO_TICKS(POOM_LED_RAINBOW_BOOT_BLINK_DELAY_MS));

        ws2812_set_pixel(&s_strip, POOM_LED_RAINBOW_BOOT_LED_INDEX, 0U, 0U, 0U, 0U);
        (void)ws2812_show(&s_strip);
        vTaskDelay(pdMS_TO_TICKS(POOM_LED_RAINBOW_BOOT_BLINK_DELAY_MS));
    }
}

/**
 * @brief Rainbow worker task.
 */
static void poom_led_rainbow_task_(void *arg)
{
    uint8_t base = 0U;
    uint8_t spread = 1U;
    const TickType_t delay_ticks = pdMS_TO_TICKS(POOM_LED_RAINBOW_ANIM_DELAY_MS);

    (void)arg;

    if (s_strip.led_count > 0)
    {
        spread = (uint8_t)(((uint16_t)POOM_LED_RAINBOW_COLOR_MAX + 1U) / (uint16_t)s_strip.led_count);
        if (spread == 0U)
        {
            spread = 1U;
        }
    }

    while (1)
    {
        for (int i = 0; i < s_strip.led_count; ++i)
        {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t pos = (uint8_t)(base + ((uint8_t)i * spread));

            poom_led_rainbow_wheel_(pos, &r, &g, &b);
            r = poom_led_rainbow_scale8_(r, POOM_LED_RAINBOW_ANIM_BRIGHTNESS);
            g = poom_led_rainbow_scale8_(g, POOM_LED_RAINBOW_ANIM_BRIGHTNESS);
            b = poom_led_rainbow_scale8_(b, POOM_LED_RAINBOW_ANIM_BRIGHTNESS);
            ws2812_set_pixel(&s_strip, i, r, g, b, 0U);
        }

        (void)ws2812_show(&s_strip);
        ++base;
        vTaskDelay(delay_ticks);
    }
}

void poom_led_rainbow_init(void)
{
    esp_err_t err;

    if (s_initialized)
    {
        return;
    }

    err = ws2812_init(&s_strip,
                      PIN_NUM_WS2812,
                      PIN_NUM_LEDS,
                      POOM_LED_RAINBOW_IS_RGBW,
                      POOM_LED_RAINBOW_RESOLUTION_HZ);
    if (err != ESP_OK)
    {
        return;
    }

    ws2812_set_brightness(&s_strip, POOM_LED_RAINBOW_INIT_BRIGHTNESS);

    poom_led_rainbow_boot_blink_();
    poom_led_rainbow_clear_all_();
    s_initialized = true;
}

bool poom_led_rainbow_start(void)
{
    BaseType_t ok;

    if (s_led_task != NULL)
    {
        return true;
    }

    if (!s_initialized)
    {
        poom_led_rainbow_init();
        if (!s_initialized)
        {
            return false;
        }
    }

    ok = xTaskCreate(poom_led_rainbow_task_,
                     POOM_LED_RAINBOW_TASK_NAME,
                     POOM_LED_RAINBOW_TASK_STACK_SIZE,
                     NULL,
                     POOM_LED_RAINBOW_TASK_PRIORITY,
                     &s_led_task);

    return (ok == pdPASS);
}

void poom_led_rainbow_stop(void)
{
    if (s_led_task != NULL)
    {
        TaskHandle_t task = s_led_task;
        s_led_task = NULL;
        vTaskDelete(task);
    }

    if (s_initialized)
    {
        poom_led_rainbow_clear_all_();
    }
}
