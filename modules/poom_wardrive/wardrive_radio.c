// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
#include "poom_wardrive.h"
#include "wardrive_wifi.h"
#include "wardrive_ble.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_system.h"
#endif
static QueueHandle_t reports;
static wd_settings_t config;
static atomic_bool running, stopping, paused, scan_done;
static atomic_uint lost, error_code, stack_free, ble_seen, ble_kept;
static portMUX_TYPE gps_lock=portMUX_INITIALIZER_UNLOCKED;
static poom_gps_fix_t current_gps;
void wd_radio_set_gps(const poom_gps_fix_t *f) {
    portENTER_CRITICAL(&gps_lock); current_gps=*f; portEXIT_CRITICAL(&gps_lock);
}
static void submit(wd_observation_t *o) {
    o->monotonic_ms=(uint64_t)esp_timer_get_time()/1000;
    portENTER_CRITICAL(&gps_lock); o->gps=current_gps; portEXIT_CRITICAL(&gps_lock);
    o->gps=poom_gps_fix_at(&o->gps,o->monotonic_ms);
    if(o->rssi>=config.rssi_threshold && xQueueSend(reports,o,0)!=pdTRUE) atomic_fetch_add(&lost,1);
}
static void ble_report(const esp_ble_gap_cb_param_t *p) {
    if(atomic_load(&paused)||atomic_load(&stopping)) return;
    atomic_fetch_add(&ble_seen,1);
    if(p->scan_rst.rssi>=config.rssi_threshold) atomic_fetch_add(&ble_kept,1);
    wd_observation_t o={0}; o.kind=WD_BLE;
    memcpy(o.address,p->scan_rst.bda,6); o.address_type=p->scan_rst.ble_addr_type;
    o.rssi=p->scan_rst.rssi; o.security=p->scan_rst.ble_evt_type;
    o.adv_length=p->scan_rst.adv_data_len>31?31:p->scan_rst.adv_data_len;
    memcpy(o.adv,p->scan_rst.ble_adv,o.adv_length);
    for(unsigned i=0;i<o.adv_length;) {
        unsigned n=o.adv[i]; if(!n||i+n>=o.adv_length) break;
        if(o.adv[i+1]==8||o.adv[i+1]==9) {
            o.name_length=n-1>32?32:n-1; memcpy(o.name,o.adv+i+2,o.name_length);
            if(o.adv[i+1]==9) break;
        }
        i+=n+1;
    }
    submit(&o);
}
static void wifi_event(void *ctx,esp_event_base_t base,int32_t id,void *data) {
    (void)ctx;(void)base;(void)id;(void)data; atomic_store(&scan_done,true);
}
static void wait_ms(unsigned ms) {
    for(unsigned t=0;t<ms&&!atomic_load(&stopping)&&!atomic_load(&paused);t+=20) vTaskDelay(pdMS_TO_TICKS(20));
}
static void wifi_window(void) {
    esp_err_t err=wd_wifi_init_sta();
    if(err!=ESP_OK) { atomic_store(&error_code,err); return; }
#if CONFIG_IDF_TARGET_ESP32C5
    err=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if(err!=ESP_OK) { atomic_store(&error_code,err); wd_wifi_deinit(); return; }
#endif
    esp_event_handler_instance_t handler;
    err=esp_event_handler_instance_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,wifi_event,NULL,&handler);
    if(err!=ESP_OK) { atomic_store(&error_code,err); wd_wifi_deinit(); return; }
    static const uint8_t channels[]={1,2,3,4,5,6,7,8,9,10,11,12,13,
#if CONFIG_IDF_TARGET_ESP32C5
        36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177
#endif
    };
    for(unsigned ch=0;ch<sizeof(channels)&&!atomic_load(&stopping)&&!atomic_load(&paused);ch++) {
        wifi_scan_config_t sc={0}; sc.channel=channels[ch]; sc.show_hidden=true;
        sc.scan_type=WIFI_SCAN_TYPE_PASSIVE; sc.scan_time.passive=200;
        atomic_store(&scan_done,false);
        err=esp_wifi_scan_start(&sc,false);
        if(err!=ESP_OK) { atomic_store(&error_code,err); continue; } /* country restriction */
        unsigned waited=0;
        while(!atomic_load(&scan_done)&&!atomic_load(&stopping)&&!atomic_load(&paused)&&waited<3000) {
            vTaskDelay(pdMS_TO_TICKS(20)); waited+=20;
        }
        if(!atomic_load(&scan_done)) { esp_wifi_scan_stop(); break; }
        wifi_ap_record_t ap;
        while(esp_wifi_scan_get_ap_record(&ap)==ESP_OK) {
            wd_observation_t o={0}; o.kind=WD_WIFI; memcpy(o.address,ap.bssid,6);
            o.name_length=strnlen((const char *)ap.ssid,32); memcpy(o.name,ap.ssid,o.name_length);
            o.rssi=ap.rssi; o.channel=ap.primary;
            o.frequency=ap.primary==14?2484:(ap.primary<=14?2407+5*ap.primary:5000+5*ap.primary);
            o.security=ap.authmode; submit(&o);
        }
        esp_wifi_clear_ap_list();
    }
    esp_wifi_scan_stop(); esp_wifi_clear_ap_list();
    esp_event_handler_instance_unregister(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,handler);
    esp_err_t cleanup=wd_wifi_deinit();
    if(cleanup!=ESP_OK) { atomic_store(&error_code,cleanup); atomic_store(&stopping,true); }
}
static void task_exit(bool lp);
static bool stack_lp,queue_lp;
static TaskHandle_t worker_handle;
static void task(void *ctx) {
    (void)ctx; bool lp=stack_lp; /* fixed for this task's lifetime */
    while(!atomic_load(&stopping)) {
        if(atomic_load(&paused)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if(config.mode!=WD_BLE) {
            wifi_window();
            /* Wi-Fi deinit deletes its driver task; FreeRTOS frees that stack
             * only when the idle task runs. Starting Bluetooth immediately
             * occasionally left ~6.8 KiB of HP SRAM unreclaimed and only ~350
             * bytes of DMA memory after scan enable (hardware trace, Nov 2026). */
            wait_ms(200);
        }
        if(config.mode!=WD_WIFI&&!atomic_load(&stopping)&&!atomic_load(&paused)) {
            /* Private scanner: no mutation of the stock BLE helper. */
            atomic_store(&ble_seen,0); atomic_store(&ble_kept,0);
            unsigned long heap=0,dma=0;
#ifdef ESP_PLATFORM
            heap=esp_get_free_heap_size(); dma=heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
#endif
            esp_err_t e=wd_ble_start(ble_report);
            if(e==ESP_OK) wait_ms(config.ble_window_ms); else atomic_store(&error_code,e);
            esp_err_t stopped=wd_ble_stop();
            if(stopped!=ESP_OK) { atomic_store(&error_code,stopped); atomic_store(&stopping,true); }
            /* Counts only: no addresses, names or payloads are logged. */
            /* adv=0 in a busy area means scanning never started: Bluetooth needs
             * ~51 KiB of DMA-capable HP SRAM (dma= at window start). */
            WD_DEBUG("[wardrive BLE] start=0x%x stop=0x%x adv=%u kept=%u heap=%lu dma=%lu\n",
                (unsigned)e,(unsigned)stopped,atomic_load(&ble_seen),atomic_load(&ble_kept),heap,dma);
        }
        atomic_store(&stack_free,uxTaskGetStackHighWaterMark(NULL));
        wait_ms(config.mode==WD_BLE?100:config.wifi_interval_ms);
    }
    atomic_store(&running,false); task_exit(lp);
}
/* ESP32-C5: Bluetooth's controller and Bluedroid take ~42 KiB of DMA-capable
 * HP SRAM; with the radio stack and queue also in HP SRAM, scan enable failed
 * (status 0x107) with 152 bytes of DMA memory left (hardware trace, Nov 2026).
 * Neither the stack nor the queue needs DMA, and file jobs (the other LP SRAM
 * user) can't run while recording, so place both in LP SRAM, falling back to
 * the previous HP allocations if LP SRAM is unavailable. */
#define WD_RADIO_STACK 4096
#define WD_RADIO_QUEUE 16
static bool queue_create(void) {
#ifdef ESP_PLATFORM
    reports=xQueueCreateWithCaps(WD_RADIO_QUEUE,sizeof(wd_observation_t),MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT);
    queue_lp=reports!=NULL;
    if(reports) return true;
#endif
    reports=xQueueCreate(WD_RADIO_QUEUE,sizeof(wd_observation_t));
    return reports!=NULL;
}
static void queue_delete(void) {
#ifdef ESP_PLATFORM
    if(queue_lp) { vQueueDeleteWithCaps(reports); reports=NULL; return; }
#endif
    vQueueDelete(reports); reports=NULL;
}
static void task_exit(bool lp) {
#ifdef ESP_PLATFORM
    (void)lp;
    vTaskSuspend(NULL);
    return;
#else
    (void)lp;
#endif
    vTaskDelete(NULL);
}
bool wd_radio_start(const wd_settings_t *settings) {
    if(atomic_load(&running)||!wd_settings_valid(settings)) return false;
    if(!reports&&!queue_create()) return false;
    xQueueReset(reports); config=*settings; atomic_store(&stopping,false);
    atomic_store(&paused,false); atomic_store(&lost,0); atomic_store(&error_code,0);
    atomic_store(&running,true);
    /* Set before creation: the worker may run before create returns. */
    stack_lp=false;
#ifdef ESP_PLATFORM
    stack_lp=true;
    if(xTaskCreateWithCaps(task,"wardrive-radio",WD_RADIO_STACK,NULL,3,&worker_handle,MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT)!=pdPASS) stack_lp=false;
#endif
    if(!stack_lp&&xTaskCreate(task,"wardrive-radio",WD_RADIO_STACK,NULL,3,&worker_handle)!=pdPASS) { atomic_store(&running,false); queue_delete(); return false; }
    WD_DEBUG("[wardrive radio] stack_lp=%u queue_lp=%u\n",(unsigned)stack_lp,(unsigned)queue_lp);
    return true;
}
void wd_radio_pause(bool p) { atomic_store(&paused,p); }
void wd_radio_stop(void) { atomic_store(&stopping,true); }
bool wd_radio_finished(void) { return !atomic_load(&running); }
bool wd_radio_next(wd_observation_t *o) { return reports&&xQueueReceive(reports,o,0)==pdTRUE; }
uint32_t wd_radio_dropped(void) { return atomic_load(&lost); }
int wd_radio_error(void) { return atomic_load(&error_code); }
unsigned wd_radio_stack_free(void) { return atomic_load(&stack_free); }
void wd_radio_release(void) {
    if(!atomic_load(&running)) {
#ifdef ESP_PLATFORM
        if(worker_handle) {
            while(eTaskGetState(worker_handle)!=eSuspended) vTaskDelay(1);
            if(stack_lp) vTaskDeleteWithCaps(worker_handle);
            else vTaskDelete(worker_handle);
            worker_handle=NULL;
        }
#endif
        if(wd_ble_stop()!=ESP_OK) return;
        if(wd_wifi_owned() && wd_wifi_deinit()!=ESP_OK) return;
        if(reports) queue_delete();
        memset(&config,0,sizeof(config));
        portENTER_CRITICAL(&gps_lock); memset(&current_gps,0,sizeof(current_gps)); portEXIT_CRITICAL(&gps_lock);
    }
}


bool wd_radio_quiescent(void) { return !atomic_load(&running) && !reports && !wd_ble_owned() && !wd_wifi_owned(); }
