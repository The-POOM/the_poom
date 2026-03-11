// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file input_events.h
 * @brief Shared message payloads published by the button driver.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Event payload published to `input/button` SBUS topic.
 */
typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} button_event_msg_t;
