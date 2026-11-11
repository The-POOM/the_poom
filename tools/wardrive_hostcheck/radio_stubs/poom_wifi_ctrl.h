#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#define WIFI_BAND_MODE_AUTO 0
#define WIFI_SCAN_TYPE_PASSIVE 1
#define ESP_FAIL -1
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif
#define WIFI_IF_STA 0
typedef struct { struct { uint8_t ssid[32],password[64]; struct { int authmode; } threshold; } sta; } wifi_config_t;
esp_err_t poom_wifi_ctrl_sta_connect(const char *,const char *);
esp_err_t esp_wifi_set_config(int,const wifi_config_t *);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
typedef struct { uint8_t channel; bool show_hidden; int scan_type; struct { unsigned passive; } scan_time; } wifi_scan_config_t;
typedef struct { uint8_t ssid[33],bssid[6],primary; int rssi,authmode; } wifi_ap_record_t;
esp_err_t poom_wifi_ctrl_init_sta(void);
esp_err_t poom_wifi_ctrl_deinit(void);
esp_err_t esp_wifi_set_band_mode(int);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *,bool);
esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_clear_ap_list(void);

esp_err_t esp_wifi_get_config(int,wifi_config_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);

typedef struct { int static_rx_buf_num,dynamic_rx_buf_num,dynamic_tx_buf_num,rx_ba_win,mgmt_sbuf_num,rx_mgmt_buf_num,magic; uint64_t feature_caps; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { .static_rx_buf_num=10,.dynamic_rx_buf_num=32,.dynamic_tx_buf_num=32,.rx_ba_win=6,.mgmt_sbuf_num=32,.rx_mgmt_buf_num=5,.magic=12345,.feature_caps=123 }
esp_err_t poom_wifi_ctrl_init_sta_with_config(const wifi_init_config_t *);


typedef void (*poom_wifi_ctrl_evt_cb_t)(const void *,void *);
bool wd_wifi_owned(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);

enum { WIFI_AUTH_OPEN=0,WIFI_AUTH_WPA2_PSK=3,WIFI_AUTH_WPA_WPA2_PSK=4,WIFI_AUTH_WPA3_PSK=6,WIFI_AUTH_WPA2_WPA3_PSK=7 };
