#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#define BLE_SCAN_TYPE_PASSIVE 1
#define BLE_SCAN_TYPE_ACTIVE 0
typedef struct { struct { uint8_t bda[6],ble_addr_type,adv_data_len,ble_adv[31],ble_evt_type; int rssi; } scan_rst; } esp_ble_gap_cb_param_t;
typedef int esp_ble_scan_filter_t;
#define BLE_SCAN_FILTER_ALLOW_ALL 0
void poom_ble_scan_set_scan_type(int);
void poom_ble_scan_set_filter_type(esp_ble_scan_filter_t);
esp_err_t poom_ble_scan_set_timing(uint16_t,uint16_t);
void poom_ble_scan_set_uart_forward_enabled(bool);
void poom_ble_scan_register_cb(void (*)(const esp_ble_gap_cb_param_t *));
esp_err_t poom_ble_scan_start(void);
esp_err_t poom_ble_scan_stop(void);
