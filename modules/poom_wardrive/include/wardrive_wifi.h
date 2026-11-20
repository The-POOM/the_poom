// SPDX-License-Identifier: MIT
#pragma once
#include "poom_wifi_ctrl.h" /* Shared event value types. */
esp_err_t wd_wifi_init_sta(void);
esp_err_t wd_wifi_init_sta_with_config(const wifi_init_config_t *config);
esp_err_t wd_wifi_deinit(void);
esp_err_t wd_wifi_sta_disconnect(void);
esp_err_t wd_wifi_sta_connect(const char *ssid, const char *password);
esp_err_t wd_wifi_register_cb(poom_wifi_ctrl_evt_cb_t cb, void *context);
esp_err_t wd_wifi_unregister_cb(void);
bool wd_wifi_sta_has_ip(void);
bool wd_wifi_owned(void);

/* Menu-entry handoff only: release a stock-manager-owned, inactive NULL-mode
 * driver left by passive stock apps. Active/unmanaged owners are refused. */
esp_err_t wd_wifi_release_idle_stock(void);
