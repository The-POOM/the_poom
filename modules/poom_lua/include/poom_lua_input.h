// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} poom_lua_button_event_t;

/**
 * @brief Poll next queued button event (from SBUS `input/button`).
 *
 * Returns false on timeout or when buttons are unavailable.
 */
bool poom_lua_buttons_poll(poom_lua_button_event_t* out_event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
