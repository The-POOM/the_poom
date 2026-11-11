// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
/* Wardrive-only passive scanner. No stock scanner/GATT settings or callback
 * slots, GATT application registration, bonding or irreversible memory release. */
#include "wardrive_ble.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
static bool controller_owned, host_owned;
static SemaphoreHandle_t completion;
static atomic_int scan_result;
static atomic_bool accepting;
static wd_ble_report_t report_callback;
static void gap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if(!param) return;
    if(event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if(param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS)
            WD_DEBUG("[wardrive BLE init] stage=scan_parameters status=0x%x\n", (unsigned)param->scan_param_cmpl.status);
        esp_err_t result = param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS ? esp_ble_gap_start_scanning(0) : ESP_FAIL;
        if(result != ESP_OK) { atomic_store(&scan_result, result); if(completion) xSemaphoreGive(completion); }
    } else if(event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        if(param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            WD_DEBUG("[wardrive BLE init] stage=scan_start status=0x%x\n", (unsigned)param->scan_start_cmpl.status);
        atomic_store(&scan_result, param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL);
        if(completion) xSemaphoreGive(completion);
    } else if(event == ESP_GAP_BLE_SCAN_RESULT_EVT && atomic_load(&accepting) &&
              param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && report_callback)
        report_callback(param);
}
bool wd_ble_owned(void) { return controller_owned || host_owned || completion != NULL; }
esp_err_t wd_ble_stop(void)
{
    atomic_store(&accepting, false);
    esp_err_t result = ESP_OK, err;
    if(host_owned) {
        if(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
            (void)esp_ble_gap_stop_scanning();
            err = esp_bluedroid_disable(); if(err != ESP_OK) return err;
        }
        err = esp_bluedroid_deinit(); if(err != ESP_OK) return err;
        host_owned = false;
    }
    if(controller_owned) {
        if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
            err = esp_bt_controller_disable(); if(err != ESP_OK) return err;
        }
        err = esp_bt_controller_deinit(); if(err != ESP_OK) return err;
        controller_owned = false;
    }
    /* Host deinit has joined its callback tasks before releasing callback data. */
    report_callback = NULL;
    if(completion) { vSemaphoreDelete(completion); completion = NULL; }
    return result;
}
esp_err_t wd_ble_start(wd_ble_report_t report)
{
    if(controller_owned || host_owned || completion || esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
       esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_ERR_INVALID_STATE;
    wifi_mode_t wifi_mode;
    if(esp_wifi_get_mode(&wifi_mode) != ESP_ERR_WIFI_NOT_INIT) return ESP_ERR_INVALID_STATE;
    completion = xSemaphoreCreateBinary();
    if(!completion) return ESP_ERR_NO_MEM;
    report_callback = report;
    atomic_store(&scan_result, ESP_ERR_TIMEOUT);
    atomic_store(&accepting, true);
    const char *stage = "controller_init";
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&config);
    if(err != ESP_OK) goto failed;
    controller_owned = true;
    stage = "controller_enable";
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK) goto failed;
    esp_bluedroid_config_t host_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    stage = "host_init";
    err = esp_bluedroid_init_with_cfg(&host_config);
    if(err != ESP_OK) goto failed;
    host_owned = true;
    stage = "host_enable";
    err = esp_bluedroid_enable();
    if(err != ESP_OK) goto failed;
    stage = "register_callback";
    err = esp_ble_gap_register_callback(gap);
    if(err != ESP_OK) goto failed;
    esp_ble_scan_params_t params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        /* The stock helper requests 0x0003 (invalid: minimum is 0x0004),
         * ignores the async rejection and scans with Bluedroid's fallback.
         * Use that fallback timing explicitly: 18 * 0.625 ms = 11.25 ms. */
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x0012,
        .scan_window = 0x0012, .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    stage = "set_scan_parameters";
    err = esp_ble_gap_set_scan_params(&params);
    if(err != ESP_OK) goto failed;
    stage = "scan_completion";
    if(xSemaphoreTake(completion, pdMS_TO_TICKS(5000)) != pdTRUE) { err = ESP_ERR_TIMEOUT; goto failed; }
    err = atomic_load(&scan_result);
    if(err == ESP_OK) return ESP_OK;
failed:
    WD_DEBUG("[wardrive BLE init] stage=%s result=0x%x\n", stage, (unsigned)err);
    (void)wd_ble_stop();
    return err;
}
