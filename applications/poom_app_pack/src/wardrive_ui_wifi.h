// SPDX-License-Identifier: MIT
#pragma once
#include "wardrive_ui_internal.h"
#include "poom_wifi_profile_store.h"
typedef struct { poom_wifi_profile_t *items; uint16_t count; } wd_ui_profiles_t;
typedef struct { char ssid[33]; int rssi; bool personal,open; } wd_ui_network_t;
bool wd_ui_wifi_password_valid(const char *password);
esp_err_t wd_ui_wifi_init_transfer(void);
bool wd_ui_wifi_matches(const char *ssid,const char *password);
esp_err_t wd_ui_wifi_connect(const char *ssid,const char *password);
bool wd_ui_profiles_open(wd_ui_profiles_t *profiles);
void wd_ui_profiles_close(wd_ui_profiles_t *profiles);
bool wd_ui_profile_pick(const wd_ui_profiles_t *profiles,unsigned index,wd_credentials_t *out);
esp_err_t wd_ui_profile_save(const wd_credentials_t *credentials);
bool wd_ui_wifi_scan(wd_ui_job_t *job);
void wd_ui_networks_clear(void);
unsigned wd_ui_network_count(void);
bool wd_ui_network_get(unsigned index,wd_ui_network_t *network);
