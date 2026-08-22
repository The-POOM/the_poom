// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WEB_HTTP_SERVER_H
#define POOM_WEB_HTTP_SERVER_H

#include "esp_err.h"
#include "poom_web.h"

esp_err_t poom_web_http_server_start(void);
esp_err_t poom_web_http_server_stop(void);
esp_err_t poom_web_http_server_send_text(const char* text);
esp_err_t poom_web_http_server_set_command_cb(poom_web_command_cb_t cb, void* user_ctx);

#endif /* POOM_WEB_HTTP_SERVER_H */
