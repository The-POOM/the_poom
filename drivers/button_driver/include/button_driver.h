// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file button_driver.h
 * @brief Physical button input driver that publishes events through SBUS.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_pong.h"
#include "iot_button.h"

/** @brief GPIO pin mapping for board button A. */
#define A_BUTTON_PIN        PIN_NUM_A
/** @brief GPIO pin mapping for board button B. */
#define B_BUTTON_PIN        PIN_NUM_B
/** @brief GPIO pin mapping for board button LEFT. */
#define LEFT_BUTTON_PIN     PIN_NUM_LEFT
/** @brief GPIO pin mapping for board button RIGHT. */
#define RIGHT_BUTTON_PIN    PIN_NUM_RIGHT
/** @brief GPIO pin mapping for board button UP. */
#define UP_BUTTON_PIN       PIN_NUM_UP
/** @brief GPIO pin mapping for board button DOWN. */
#define DOWN_BUTTON_PIN     PIN_NUM_DOWN

/** @brief High nibble mask for button A events. */
#define A_BUTTON_MASK       (0x0U << 4)
/** @brief High nibble mask for button B events. */
#define B_BUTTON_MASK       (0x1U << 4)
/** @brief High nibble mask for button LEFT events. */
#define LEFT_BUTTON_MASK    (0x2U << 4)
/** @brief High nibble mask for button RIGHT events. */
#define RIGHT_BUTTON_MASK   (0x3U << 4)
/** @brief High nibble mask for button UP events. */
#define UP_BUTTON_MASK      (0x4U << 4)
/** @brief High nibble mask for button DOWN events. */
#define DOWN_BUTTON_MASK    (0x5U << 4)

/** @brief Active logic level for all board buttons. */
#define BUTTON_ACTIVE_LEVEL (0)

/**
 * @brief Logical button identifiers.
 */
typedef enum
{
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_UP,
    BUTTON_DOWN,
} button_layout_t;

/**
 * @brief Local representation of one button event.
 */
typedef struct
{
    uint8_t button_pressed;
    uint8_t button_event;
} button_event_state_t;

/**
 * @brief Legacy callback signature used by older modules.
 */
typedef void (*input_callback_t)(uint8_t, uint8_t);

/**
 * @brief Initialize button hardware and register SBUS event publishing.
 */
void button_module_begin(void);

/**
 * @brief Reset input idle timer.
 */
void button_module_reset_idle_timer(void);

/**
 * @brief Lock or unlock event publishing.
 *
 * @param lock true to block published callbacks/events, false to enable them.
 */
void button_module_set_lock(bool lock);
