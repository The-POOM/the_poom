#pragma once
#include "poom_wifi_ctrl.h"
#define POOM_WIFI_SCANNER_MAX_AP 20

typedef struct { uint16_t count; wifi_ap_record_t records[POOM_WIFI_SCANNER_MAX_AP]; } poom_wifi_scanner_ap_records_t;
esp_err_t poom_wifi_scanner_clear_ap_records(void);
esp_err_t poom_wifi_scanner_scan_configured(const wifi_scan_config_t *);
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void);
wifi_ap_record_t *poom_wifi_scanner_get_ap_record(unsigned);
