#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
typedef struct { uint8_t ssid[32]; uint8_t password[64]; } wifi_sta_config_t;
typedef union { wifi_sta_config_t sta; } wifi_config_t;
typedef struct { uint8_t bssid[6]; uint8_t ssid[33]; uint8_t primary; int8_t rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c);
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef void (*wifi_promiscuous_cb_t)(void *buf, int type);
typedef struct { uint8_t *ssid; } wifi_scan_config_t;
