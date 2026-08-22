// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*poom_lua_done_cb_t)(esp_err_t status, const char* error_msg, void* user_ctx);

/**
 * @brief Runs a Lua script file on SD in a dedicated FreeRTOS task.
 *
 * @param[in] abs_path Absolute script path under `/sdcard` (e.g. "/sdcard/main.lua").
 * @param[in] cb Optional completion callback (invoked from the Lua task context).
 * @param[in] user_ctx Callback context pointer.
 */
esp_err_t poom_lua_run_file_async(const char* abs_path, poom_lua_done_cb_t cb, void* user_ctx);

/**
 * @brief Returns true when a Lua script task is running.
 */
bool poom_lua_is_running(void);

/**
 * @brief Requests the currently running Lua script to stop.
 *
 * Stop is cooperative: the VM checks a stop flag via an instruction-count hook
 * and aborts execution with an error (reported as "stopped").
 */
esp_err_t poom_lua_request_stop(void);

#ifdef __cplusplus
}
#endif
