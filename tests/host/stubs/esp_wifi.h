#ifndef HOST_ESP_WIFI_H
#define HOST_ESP_WIFI_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint8_t ssid[33]; uint8_t bssid[6]; } wifi_ap_record_t;
typedef struct { uint8_t channel; } wifi_scan_config_t;
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
#endif
