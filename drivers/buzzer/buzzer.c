// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file buzzer.c
 * @brief PWM buzzer driver based on ESP-IDF LEDC.
 */

#include "buzzer.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUZZER_LEDC_TIMER              LEDC_TIMER_0
#define BUZZER_LEDC_MODE               LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL            LEDC_CHANNEL_0
#define BUZZER_LEDC_DUTY_RESOLUTION    LEDC_TIMER_13_BIT
#define BUZZER_DEFAULT_FREQ_HZ         (4000U)
#define BUZZER_DUTY_OFF                (0U)
#define BUZZER_HPOINT_DEFAULT          (0U)
#define BUZZER_DUTY_HALF_SCALE         (4096U)

typedef struct
{
    uint8_t pin;
    bool enabled;
    bool initialized;
} buzzer_state_t;

static buzzer_state_t s_buzzer = {0};

/**
 * @brief Disable PWM output for the configured buzzer channel.
 */
static void buzzer_stop_(void)
{
    (void)ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_OFF);
    (void)ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

void buzzer_init(uint8_t pin)
{
    ledc_timer_config_t timer = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_LEDC_DUTY_RESOLUTION,
        .freq_hz = BUZZER_DEFAULT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = pin,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = BUZZER_DUTY_OFF,
        .hpoint = BUZZER_HPOINT_DEFAULT,
    };

    s_buzzer.pin = pin;
    s_buzzer.enabled = true;
    s_buzzer.initialized = true;

    (void)ledc_timer_config(&timer);
    (void)ledc_channel_config(&channel);
}

void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if ((!s_buzzer.initialized) || (!s_buzzer.enabled))
    {
        return;
    }
    if ((freq_hz == 0U) || (duration_ms == 0U))
    {
        buzzer_stop_();
        return;
    }

    (void)ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    (void)ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_HALF_SCALE);
    (void)ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_stop_();
}
