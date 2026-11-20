// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT };
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count,size_t size);
int xQueueSend(QueueHandle_t queue,const void *item,unsigned ticks);
int xQueueReceive(QueueHandle_t queue,void *item,unsigned ticks);
void xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle);
void vTaskDelay(unsigned ticks);
void vTaskDelete(void *task);
int64_t esp_timer_get_time(void);
typedef int esp_gatt_if_t;
typedef uint16_t esp_gatt_status_t;
typedef int esp_ble_addr_type_t;
typedef uint8_t esp_bd_addr_t[6];
enum {ESP_GATT_OK=0,ESP_GATT_NOT_FOUND=10,ESP_GATT_IF_NONE=255};
enum {ESP_UUID_LEN_16=2,ESP_UUID_LEN_128=16};
typedef struct {uint16_t len;union {uint16_t uuid16;uint8_t uuid128[16];} uuid;} esp_bt_uuid_t;
enum {ESP_BT_CONTROLLER_STATUS_IDLE,ESP_BT_CONTROLLER_STATUS_INITED,ESP_BT_CONTROLLER_STATUS_ENABLED};
enum {ESP_BLUEDROID_STATUS_UNINITIALIZED,ESP_BLUEDROID_STATUS_INITIALIZED,ESP_BLUEDROID_STATUS_ENABLED};
typedef struct {int unused;} esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {0}
#define ESP_BT_MODE_BLE 1
#define ESP_BT_STATUS_SUCCESS 0
int esp_bt_controller_get_status(void);
int esp_bluedroid_get_status(void);
esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *config);
esp_err_t esp_bt_controller_enable(int mode);
esp_err_t esp_bt_controller_disable(void);
esp_err_t esp_bt_controller_deinit(void);
esp_err_t esp_bluedroid_init(void);
esp_err_t esp_bluedroid_enable(void);
esp_err_t esp_bluedroid_disable(void);
esp_err_t esp_bluedroid_deinit(void);
enum {BLE_SCAN_TYPE_ACTIVE=1,BLE_ADDR_TYPE_PUBLIC=0,BLE_SCAN_FILTER_ALLOW_ALL=0,BLE_SCAN_DUPLICATE_DISABLE=0};
enum {ESP_BLE_EVT_CONN_ADV,ESP_BLE_EVT_CONN_DIR_ADV,ESP_BLE_EVT_DISC_ADV,ESP_BLE_EVT_NON_CONN_ADV,ESP_BLE_EVT_SCAN_RSP};
enum {ESP_GAP_SEARCH_INQ_RES_EVT,ESP_GAP_SEARCH_INQ_CMPL_EVT};
typedef enum {ESP_GAP_BLE_SCAN_RESULT_EVT,ESP_GAP_BLE_SEC_REQ_EVT,ESP_GAP_BLE_NC_REQ_EVT,ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,
 ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT} esp_gap_ble_cb_event_t;
typedef struct {int scan_type,own_addr_type,scan_filter_policy,scan_interval,scan_window,scan_duplicate;} esp_ble_scan_params_t;
typedef union {
 struct {uint8_t adv_data_len,scan_rsp_len,ble_adv[62],bda[6];int ble_addr_type,rssi,ble_evt_type,search_evt;} scan_rst;
 struct {struct {uint8_t bd_addr[6];} ble_req,key_notif;} ble_security;
 struct {int status;} scan_param_cmpl,scan_start_cmpl,scan_stop_cmpl;
} esp_ble_gap_cb_param_t;
esp_err_t esp_ble_gap_register_callback(void (*cb)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *));
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *params);
esp_err_t esp_ble_gap_start_scanning(uint32_t seconds);
esp_err_t esp_ble_gap_security_rsp(uint8_t bda[6],bool accept);
esp_err_t esp_ble_confirm_reply(uint8_t bda[6],bool accept);
typedef enum {ESP_GATTC_REG_EVT,ESP_GATTC_OPEN_EVT,ESP_GATTC_CONNECT_EVT,ESP_GATTC_DISCONNECT_EVT,ESP_GATTC_DIS_SRVC_CMPL_EVT,
 ESP_GATTC_SEARCH_RES_EVT,ESP_GATTC_SEARCH_CMPL_EVT,ESP_GATTC_REG_FOR_NOTIFY_EVT,ESP_GATTC_WRITE_DESCR_EVT,ESP_GATTC_WRITE_CHAR_EVT,ESP_GATTC_NOTIFY_EVT} esp_gattc_cb_event_t;
typedef union {
 struct {esp_gatt_status_t status;uint16_t app_id;} reg;
 struct {esp_gatt_status_t status;uint16_t conn_id;uint8_t remote_bda[6];} open;
 struct {uint16_t conn_id;uint8_t remote_bda[6];esp_ble_addr_type_t ble_addr_type;} connect;
 struct {uint16_t conn_id;uint8_t remote_bda[6];} disconnect;
 struct {esp_gatt_status_t status;uint16_t conn_id;} dis_srvc_cmpl,search_cmpl;
 struct {uint16_t conn_id,start_handle,end_handle;struct {esp_bt_uuid_t uuid;} srvc_id;} search_res;
 struct {esp_gatt_status_t status;uint16_t handle;} reg_for_notify;
 struct {esp_gatt_status_t status;uint16_t conn_id,handle;} write;
 struct {uint16_t conn_id,handle,value_len;uint8_t remote_bda[6];uint8_t *value;} notify;
} esp_ble_gattc_cb_param_t;
typedef struct {uint16_t char_handle;uint8_t properties;} esp_gattc_char_elem_t;
typedef struct {uint16_t handle;} esp_gattc_descr_elem_t;
enum {ESP_GATT_CHAR_PROP_BIT_WRITE=8,ESP_GATT_CHAR_PROP_BIT_NOTIFY=16,ESP_GATT_CHAR_PROP_BIT_INDICATE=32,
 ESP_GATT_UUID_CHAR_CLIENT_CONFIG=0x2902,ESP_GATT_WRITE_TYPE_RSP=1,ESP_GATT_AUTH_REQ_NONE=0};
esp_err_t esp_ble_gattc_register_callback(void (*cb)(esp_gattc_cb_event_t,esp_gatt_if_t,esp_ble_gattc_cb_param_t *));
esp_err_t esp_ble_gattc_app_register(uint16_t app);
esp_err_t esp_ble_gattc_open(esp_gatt_if_t interface,esp_bd_addr_t bda,esp_ble_addr_type_t type,bool direct);
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t interface,uint16_t connection,esp_bt_uuid_t *uuid);
esp_gatt_status_t esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t interface,uint16_t connection,uint16_t start,uint16_t end,esp_bt_uuid_t uuid,esp_gattc_char_elem_t *result,uint16_t *count);
esp_err_t esp_ble_gattc_register_for_notify(esp_gatt_if_t interface,esp_bd_addr_t bda,uint16_t handle);
esp_gatt_status_t esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,esp_bt_uuid_t uuid,esp_gattc_descr_elem_t *result,uint16_t *count);
esp_err_t esp_ble_gattc_write_char_descr(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,uint16_t length,uint8_t *value,int type,int auth);
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,uint16_t length,uint8_t *value,int type,int auth);
