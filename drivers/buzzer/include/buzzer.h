// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file buzzer.h
 * @brief PWM buzzer driver based on ESP-IDF LEDC.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize buzzer GPIO and LEDC channel/timer.
 *
 * @param pin GPIO number connected to the buzzer.
 */
void buzzer_init(uint8_t pin);

/**
 * @brief Play a tone for a fixed duration.
 *
 * This call is blocking during @p duration_ms.
 *
 * @param freq_hz Tone frequency in Hz.
 * @param duration_ms Tone duration in milliseconds.
 */
void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
