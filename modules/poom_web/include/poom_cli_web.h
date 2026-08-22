// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

/*
 * Compatibility shim: `poom_cli_web` was renamed to `poom_web`.
 *
 * Keep this header so existing code keeps building while new code migrates to:
 *   #include "poom_web.h"
 */

#include "poom_web.h"

typedef poom_web_command_cb_t poom_cli_web_command_cb_t;

static inline esp_err_t poom_cli_web_init(void)
{
    return poom_web_init();
}

static inline esp_err_t poom_cli_web_init_sta(const char* ssid,
                                              const char* password,
                                              char* out_ip,
                                              size_t out_ip_len)
{
    return poom_web_init_sta(ssid, password, out_ip, out_ip_len);
}

static inline esp_err_t poom_cli_web_init_sta_saved(char* out_ip, size_t out_ip_len)
{
    return poom_web_init_sta_saved(out_ip, out_ip_len);
}

static inline esp_err_t poom_cli_web_deinit(void)
{
    return poom_web_deinit();
}

static inline esp_err_t poom_cli_web_set_command_cb(poom_cli_web_command_cb_t cb, void* user_ctx)
{
    return poom_web_set_command_cb(cb, user_ctx);
}

static inline esp_err_t poom_cli_web_send_text(const char* text)
{
    return poom_web_send_text(text);
}

static inline const char* poom_cli_web_get_wifi_ap_ssid(void)
{
    return poom_web_get_wifi_ap_ssid();
}

static inline const char* poom_cli_web_get_wifi_ap_password(void)
{
    return poom_web_get_wifi_ap_password();
}

static inline const char* poom_cli_web_get_wifi_ap_ip(void)
{
    return poom_web_get_wifi_ap_ip();
}

static inline const char* poom_cli_web_get_wifi_sta_ssid(void)
{
    return poom_web_get_wifi_sta_ssid();
}

static inline const char* poom_cli_web_get_wifi_sta_ip(void)
{
    return poom_web_get_wifi_sta_ip();
}
