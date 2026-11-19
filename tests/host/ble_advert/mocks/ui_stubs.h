// SPDX-License-Identifier: MIT
#pragma once
#include "mock_idf.h"
#define ARDUBOY_WIDTH 128
#define INVERT 2
#define BUTTON_SINGLE_CLICK 4
typedef struct {uint8_t button,event;} button_event_msg_t;
typedef struct {const void *data;size_t len;} poom_sbus_msg_t;
void poom_arduboy_clear(void);
void poom_arduboy_set_text_size(int size);
void poom_arduboy_set_cursor(int x,int y);
int poom_arduboy_print(const char *text);
void poom_arduboy_fill_rect(int x,int y,int width,int height,int color);
void poom_arduboy_display(void);
bool poom_sbus_subscribe_cb(const char *topic,void(*callback)(const poom_sbus_msg_t*,void*),void *user);
bool poom_sbus_unsubscribe_cb(const char *topic,void(*callback)(const poom_sbus_msg_t*,void*),void *user);
bool poom_sbus_publish(const char *topic,const void *data,size_t length,unsigned flags);
void poom_ble_tracker_stop(void);
esp_err_t poom_ble_detector_stop(void);
esp_err_t poom_ble_scan_stop(void);
esp_err_t poom_ble_gatt_client_stop(void);
const char *esp_err_to_name(esp_err_t error);
