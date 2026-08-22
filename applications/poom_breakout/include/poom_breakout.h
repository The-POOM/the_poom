// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BREAKOUT_H
#define POOM_BREAKOUT_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*poom_breakout_exit_cb_t)(void *user_ctx);

esp_err_t poom_breakout_start(void);
esp_err_t poom_breakout_stop(void);
bool poom_breakout_is_running(void);
esp_err_t poom_breakout_set_exit_callback(poom_breakout_exit_cb_t callback, void *user_ctx);

void app_breakout_menu(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BREAKOUT_H */
