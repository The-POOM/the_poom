#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int esp_ble_scan_filter_t;
typedef int esp_ble_scan_type_t;
#define BLE_SCAN_TYPE_PASSIVE 0
#define BLE_SCAN_FILTER_ALLOW_ALL 0
#define ESP_BLE_AD_TYPE_NAME_SHORT 0x08
#define ESP_BLE_AD_TYPE_NAME_CMPL 0x09
#define ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE 0xFF
#define ESP_BLE_AD_TYPE_16SRV_PART 0x02
#define ESP_BLE_AD_TYPE_16SRV_CMPL 0x03
#define ESP_BLE_AD_TYPE_SERVICE_DATA 0x16
typedef struct {
    struct {
        uint8_t adv_data_len, scan_rsp_len;
        uint8_t ble_adv[62];
        uint8_t bda[6];
        int rssi;
    } scan_rst;
} esp_ble_gap_cb_param_t;
