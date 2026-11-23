// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "poom_ble_spam.h"
#include <stdio.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { esp_gap_ble_cb_event_t event; int status; } adv_event_t;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t events;
static bool stopping, overflow, controller_owned, controller_enabled, host_owned, host_enabled;
static bool duration_expired;
static poom_ble_spam_config_t config;
static poom_ble_spam_status_t status={.radio_released=true};
static uint32_t started_ms;
static poom_ble_spam_cb_display display_callback;
static uint32_t now_(void) { return (uint32_t)(esp_timer_get_time()/1000LL); }
static bool cancelled_(void)
{
    portENTER_CRITICAL(&lock);
    const bool value=stopping;
    portEXIT_CRITICAL(&lock);
    return value;
}
static void gap_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    adv_event_t e={.event=event};
    switch(event) {
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT: e.status=param->set_rand_addr_cmpl.status; break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: e.status=param->adv_data_raw_cmpl.status; break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: e.status=param->adv_start_cmpl.status; break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: e.status=param->adv_stop_cmpl.status; break;
    default:return;
    }
    portENTER_CRITICAL(&lock);
    if(events && xQueueSend(events,&e,0)!=pdTRUE) overflow=true;
    portEXIT_CRITICAL(&lock);
}
static esp_err_t wait_(esp_gap_ble_cb_event_t expected, bool allow_cancel)
{
    const uint32_t start=now_();
    while((uint32_t)(now_()-start)<3000U) {
        if(allow_cancel && cancelled_()) return ESP_ERR_INVALID_STATE;
        if(allow_cancel && (uint32_t)(now_()-started_ms)>=config.duration_ms) {
            duration_expired=true;
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&lock);
        const bool full=overflow;
        portEXIT_CRITICAL(&lock);
        if(full) return ESP_ERR_NO_MEM;
        adv_event_t e;
        if(xQueueReceive(events,&e,pdMS_TO_TICKS(20))!=pdTRUE) continue;
        if(e.event==expected) return e.status==ESP_BT_STATUS_SUCCESS?ESP_OK:ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t initialize_(void)
{
    if(esp_bt_controller_get_status()!=ESP_BT_CONTROLLER_STATUS_IDLE ||
       esp_bluedroid_get_status()!=ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_ERR_INVALID_STATE;
    esp_bt_controller_config_t bt=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t error=esp_bt_controller_init(&bt);
    if(error!=ESP_OK)return error;
    controller_owned=true;
    if((error=esp_bt_controller_enable(ESP_BT_MODE_BLE))!=ESP_OK)return error;
    controller_enabled=true;
    if((error=esp_bluedroid_init())!=ESP_OK)return error;
    host_owned=true;
    if((error=esp_bluedroid_enable())!=ESP_OK)return error;
    host_enabled=true;
    return esp_ble_gap_register_callback(gap_);
}
static esp_err_t cleanup_(void)
{
    esp_err_t first=ESP_OK,error;
    if(host_enabled) {
        error=esp_bluedroid_disable();
        if(error==ESP_OK)host_enabled=false;else first=error;
    }
    if(host_owned && !host_enabled) {
        error=esp_bluedroid_deinit();
        if(error==ESP_OK)host_owned=false;else if(first==ESP_OK)first=error;
    }
    if(controller_enabled && !host_enabled) {
        error=esp_bt_controller_disable();
        if(error==ESP_OK)controller_enabled=false;else if(first==ESP_OK)first=error;
    }
    if(controller_owned && !controller_enabled && !host_owned) {
        error=esp_bt_controller_deinit();
        if(error==ESP_OK)controller_owned=false;else if(first==ESP_OK)first=error;
    }
    return first;
}
static esp_err_t stop_advertising_(void)
{
    const esp_err_t submitted=esp_ble_gap_stop_advertising();
    const esp_err_t error=submitted==ESP_OK?wait_(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT,false):submitted;
    if(error==ESP_OK) {
        portENTER_CRITICAL(&lock);
        status.advertising=false;
        portEXIT_CRITICAL(&lock);
    }
    return error;
}
static void worker_(void *arg)
{
    (void)arg;
    esp_err_t error=cancelled_()?ESP_ERR_INVALID_STATE:initialize_();
    uint32_t sequence=0;
    const esp_ble_adv_params_t parameters={.adv_int_min=0xa0,.adv_int_max=0xa0,
        .adv_type=ADV_TYPE_NONCONN_IND,.own_addr_type=BLE_ADDR_TYPE_RANDOM,
        .channel_map=ADV_CHNL_ALL,.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY};
    while(error==ESP_OK && !cancelled_() && (uint32_t)(now_()-started_ms)<config.duration_ms) {
        uint8_t payload[31],address[6];
        char label[24];
        const size_t length=poom_ble_payload_build(config.mode,sequence,esp_random(),payload,label);
        if(!length) {error=ESP_ERR_INVALID_ARG;break;}
        esp_fill_random(address,sizeof(address));
        address[0]=(uint8_t)((address[0]&0x3fU)|0xc0U);
        address[5]=(uint8_t)((address[5]&0xfcU)|1U);
        error=esp_ble_gap_set_rand_addr(address);
        if(error==ESP_OK)error=wait_(ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT,true);
        if(error==ESP_OK)error=esp_ble_gap_config_adv_data_raw(payload,(uint32_t)length);
        if(error==ESP_OK)error=wait_(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT,true);
        esp_ble_adv_params_t mutable_parameters=parameters;
        if(error==ESP_OK)error=esp_ble_gap_start_advertising(&mutable_parameters);
        if(error==ESP_OK)error=wait_(ESP_GAP_BLE_ADV_START_COMPLETE_EVT,true);
        if(error!=ESP_OK)break;
        portENTER_CRITICAL(&lock);
        status.advertising=true;
        ++status.cycles;
        memcpy(status.label,label,sizeof(status.label));
        poom_ble_spam_cb_display callback=display_callback;
        portEXIT_CRITICAL(&lock);
        if(callback)callback(label);
        const uint32_t cycle_start=now_();
        const poom_ble_spam_mode_t family=config.mode==POOM_BLE_SPAM_ALL?
            (poom_ble_spam_mode_t)(sequence%POOM_BLE_SPAM_ALL):config.mode;
        const uint32_t dwell=config.dwell_ms?config.dwell_ms:family==POOM_BLE_SPAM_SOUR_APPLE?150U:500U;
        while(!cancelled_() && (uint32_t)(now_()-cycle_start)<dwell &&
              (uint32_t)(now_()-started_ms)<config.duration_ms) vTaskDelay(pdMS_TO_TICKS(10));
        error=stop_advertising_();
        ++sequence;
    }
    /* Callbacks only report completions; they never start/rotate advertisements.
     * Host teardown also cancels a start whose ACK was not consumed on stop. */
    const esp_err_t released=cleanup_();
    if(released!=ESP_OK)error=released;
    portENTER_CRITICAL(&lock);
    status.error=released==ESP_OK && ((stopping && error==ESP_ERR_INVALID_STATE) ||
                 (duration_expired && error==ESP_ERR_TIMEOUT))?ESP_OK:error;
    status.elapsed_ms=(uint32_t)(now_()-started_ms);
    status.radio_released=!host_owned && !controller_owned;
    if(status.radio_released)status.advertising=false;
    QueueHandle_t old=events;
    events=NULL;
    portEXIT_CRITICAL(&lock);
    vQueueDelete(old);
    portENTER_CRITICAL(&lock);
    status.active=false;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}
void poom_ble_spam_register_cb(poom_ble_spam_cb_display callback)
{
    portENTER_CRITICAL(&lock);
    display_callback=callback;
    portEXIT_CRITICAL(&lock);
}
esp_err_t poom_ble_spam_start_mode(const poom_ble_spam_config_t *requested)
{
    if(!requested || (unsigned)requested->mode>=POOM_BLE_SPAM_MODE_COUNT || requested->duration_ms>60000U ||
       (requested->duration_ms && requested->duration_ms<1000U) ||
       (requested->dwell_ms && (requested->dwell_ms<100U || requested->dwell_ms>5000U))) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    if(status.active || controller_owned || host_owned) {portEXIT_CRITICAL(&lock);return ESP_ERR_INVALID_STATE;}
    memset(&status,0,sizeof(status));
    config=*requested;
    if(!config.duration_ms)config.duration_ms=30000U;
    status.duration_ms=config.duration_ms;
    status.mode=config.mode;
    status.active=true;
    stopping=overflow=false;
    duration_expired=false;
    started_ms=now_();
    portEXIT_CRITICAL(&lock);
    QueueHandle_t queue=xQueueCreate(8,sizeof(adv_event_t));
    portENTER_CRITICAL(&lock);
    events=queue;
    portEXIT_CRITICAL(&lock);
    if(!queue || xTaskCreate(worker_,"ble_advert",4096,NULL,4,NULL)!=pdPASS) {
        if(queue)vQueueDelete(queue);
        portENTER_CRITICAL(&lock);
        events=NULL;
        status.active=false;
        status.radio_released=true;
        status.error=ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t poom_ble_spam_start(void)
{
    const poom_ble_spam_config_t defaults={.mode=POOM_BLE_SPAM_APPLE_DEVICES};
    return poom_ble_spam_start_mode(&defaults);
}
bool poom_ble_spam_is_active(void)
{
    portENTER_CRITICAL(&lock);
    const bool active=status.active;
    portEXIT_CRITICAL(&lock);
    return active;
}
esp_err_t poom_ble_spam_stop(void)
{
    portENTER_CRITICAL(&lock);
    stopping=true;
    portEXIT_CRITICAL(&lock);
    while(poom_ble_spam_is_active())vTaskDelay(pdMS_TO_TICKS(10));
    portENTER_CRITICAL(&lock);
    const esp_err_t error=host_owned || controller_owned?(status.error?status.error:ESP_ERR_INVALID_STATE):ESP_OK;
    portEXIT_CRITICAL(&lock);
    return error;
}
void poom_ble_spam_app_stop(void)
{
    const esp_err_t error=poom_ble_spam_stop();
    if(error!=ESP_OK)printf("[poom_ble_spam] Bluetooth release failed: 0x%x\n",(unsigned)error);
}
void poom_ble_spam_get_status(poom_ble_spam_status_t *out)
{
    if(!out)return;
    const uint32_t now=now_();
    portENTER_CRITICAL(&lock);
    *out=status;
    if(out->active)out->elapsed_ms=(uint32_t)(now-started_ms);
    portEXIT_CRITICAL(&lock);
}
