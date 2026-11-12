// SPDX-License-Identifier: MIT
#include "poom_wardrive.h"
#include "poom_wifi_ctrl.h"
#include "wardrive_ble.h"
#include "wardrive_wifi.h"
#include "esp_event.h"
#include "freertos/queue.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void (*worker)(void *),(*ble_cb)(const esp_ble_gap_cb_param_t *);
static void (*wifi_cb)(void *,esp_event_base_t,int,void *);
static unsigned delays,channel,trace_length;
static uint64_t milliseconds;
static bool wifi_on,ble_on,available,stop_in_wifi,burst,pause_test;
static char trace[16];
struct test_queue { wd_observation_t items[16]; unsigned count,head; };
QueueHandle_t xQueueCreate(unsigned n,size_t size) { assert(n==16&&size==sizeof(wd_observation_t)); return calloc(1,sizeof(struct test_queue)); }
void xQueueReset(QueueHandle_t q) { memset(q,0,sizeof(*q)); }
int xQueueSend(QueueHandle_t q,const void *o,unsigned wait) { assert(!wait); if(q->count==16) return 0; q->items[(q->head+q->count)%16]=*(const wd_observation_t *)o; q->count++; return 1; }
int xQueueReceive(QueueHandle_t q,void *o,unsigned wait) { assert(!wait); if(!q->count) return 0; *(wd_observation_t *)o=q->items[q->head]; q->head=(q->head+1)%16; q->count--; return 1; }
void vQueueDelete(QueueHandle_t q) { free(q); }
int64_t esp_timer_get_time(void) { return milliseconds*1000; }
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *out) { (void)name;(void)arg;(void)priority;(void)out; assert(stack==4096); worker=fn; return 1; }
void vTaskDelay(unsigned t) { milliseconds+=t; delays++; if(pause_test&&delays==1) { assert(!wifi_on&&!ble_on); wd_radio_pause(false); } else if(delays>=15) wd_radio_stop(); /* 10 ticks are the 200 ms post-Wi-Fi settle */ }
void vTaskDelete(void *p) { assert(!p); }
unsigned uxTaskGetStackHighWaterMark(void *p) { (void)p; return 2048; }
esp_err_t wd_wifi_init_sta(void) { assert(!ble_on&&!wifi_on); wifi_on=true; trace[trace_length++]='W'; return ESP_OK; }
esp_err_t wd_wifi_deinit(void) { assert(wifi_on); wifi_on=false; trace[trace_length++]='w'; return ESP_OK; }
esp_err_t esp_wifi_set_band_mode(int mode) { assert(mode==WIFI_BAND_MODE_AUTO); return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int id,void (*cb)(void *,esp_event_base_t,int,void *),void *c,esp_event_handler_instance_t *h) { (void)b;(void)id;(void)c; wifi_cb=cb; *h=(void *)1; return ESP_OK; }
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b,int id,esp_event_handler_instance_t h) { (void)b;(void)id;(void)h; wifi_cb=NULL; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c,bool blocking) {
    assert(wifi_on&&!ble_on&&!blocking&&c->scan_type==WIFI_SCAN_TYPE_PASSIVE&&c->scan_time.passive==200);
    channel=c->channel; available=channel==1||channel==36;
    wifi_cb(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,NULL);
    if(stop_in_wifi) wd_radio_stop();
    return ESP_OK;
}
esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *o) {
    if(!available) return ESP_FAIL;
    available=false; memset(o,0,sizeof(*o));
    o->bssid[0]=2; o->bssid[5]=channel; o->rssi=-55; o->primary=channel; memcpy(o->ssid,"synthetic",9); return ESP_OK;
}
esp_err_t esp_wifi_scan_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { return ESP_OK; }
esp_err_t wd_ble_start(wd_ble_report_t cb) {
    ble_cb=cb;
    assert(!wifi_on&&!ble_on); ble_on=true; trace[trace_length++]='B';
    esp_ble_gap_cb_param_t p={0}; p.scan_rst.bda[0]=2; p.scan_rst.ble_addr_type=1; p.scan_rst.rssi=-65;
    p.scan_rst.adv_data_len=5; memcpy(p.scan_rst.ble_adv,"\x04\x09" "BLE",5);
    for(unsigned i=0;i<(burst?32U:1U);i++) { p.scan_rst.bda[5]=i; ble_cb(&p); }
    return ESP_OK;
}
bool wd_wifi_owned(void) { return wifi_on; }
bool wd_ble_owned(void) { return ble_on; }
esp_err_t wd_ble_stop(void) { if(!ble_on) return ESP_OK; ble_on=false; ble_cb=NULL; trace[trace_length++]='b'; return ESP_OK; }
#include "../wardrive_radio.c"
static unsigned run(wd_mode_t mode,bool fresh) {
    delays=0; trace_length=0; memset(trace,0,sizeof(trace));
    wd_settings_t s; wd_settings_default(&s); s.mode=mode;
    poom_gps_fix_t f={.fields=GPS_POSITION,.fix=true,.connected=true,.latitude=1,.longitude=2};
    milliseconds=fresh?1000:8000; wd_radio_set_gps(&f);
    assert(wd_radio_start(&s)); if(pause_test) wd_radio_pause(true); worker(NULL); assert(wd_radio_finished());
    unsigned count=0; wd_observation_t o;
    while(wd_radio_next(&o)) {
        assert(!!(o.gps.fields&GPS_POSITION)==fresh);
        if(o.kind==WD_BLE) assert(o.name_length==3&&!memcmp(o.name,"BLE",3)&&o.address_type==1);
        count++;
    }
    assert(!wifi_on&&!ble_on&&!wifi_cb); wd_radio_release(); return count;
}
static void cached_gps_freshness(void) {
    wd_settings_t s; wd_settings_default(&s);
    assert(wd_radio_start(&s));
    wd_observation_t o={.rssi=-50};
    poom_gps_fix_t f={.fields=GPS_POSITION|GPS_DATE|GPS_TIME|GPS_SPEED|GPS_COURSE,.connected=true,.fix=true,
        .position_ms=4000,.sentence_ms=4000,.date_ms=1000,.time_ms=4000,
        .year=2026,.month=11,.day=8};
    milliseconds=4000; wd_radio_set_gps(&f); submit(&o);
    assert(wd_radio_next(&o));
    assert(o.gps.fix&&(o.gps.fields&GPS_POSITION)&&(o.gps.fields&GPS_TIME));
    assert(!(o.gps.fields&GPS_DATE)); /* position is fresh, cached date is not */
    assert(!(o.gps.fields&(GPS_SPEED|GPS_COURSE)));
    milliseconds=7000; submit(&o); assert(wd_radio_next(&o));
    assert(o.gps.connected&&!o.gps.fix);
    assert(!(o.gps.fields&(GPS_POSITION|GPS_DATE|GPS_TIME)));
    milliseconds=10000; submit(&o); assert(wd_radio_next(&o));
    assert(!o.gps.connected&&!o.gps.fix&&!o.gps.fields);
    milliseconds=3000; /* future timestamps fail closed */
    submit(&o); assert(wd_radio_next(&o));
    assert(!o.gps.connected&&!o.gps.fix&&!o.gps.fields);
    wd_radio_stop(); worker(NULL); wd_radio_release();
}
int main(void) {
    assert(run(WD_COMBINED,true)==3&&!strcmp(trace,"WwBb"));
    assert(run(WD_WIFI,false)==2&&!strcmp(trace,"Ww"));
    assert(run(WD_BLE,true)==1&&!strcmp(trace,"Bb"));
    pause_test=true; assert(run(WD_COMBINED,true)==3); pause_test=false;
    stop_in_wifi=true; assert(run(WD_COMBINED,true)==1&&!strcmp(trace,"Ww")); stop_in_wifi=false;
    burst=true; assert(run(WD_BLE,true)==16&&wd_radio_dropped()==16);
    cached_gps_freshness();
    puts("Simulated actual radio worker: combined order, passive configuration, pause/stop, GPS association and queue bounds passed");
}
