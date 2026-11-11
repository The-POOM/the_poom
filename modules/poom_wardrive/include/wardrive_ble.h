// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_gap_ble_api.h"
#include "esp_err.h"
typedef void (*wd_ble_report_t)(const esp_ble_gap_cb_param_t *report);
esp_err_t wd_ble_start(wd_ble_report_t report);
esp_err_t wd_ble_stop(void);

bool wd_ble_owned(void);
