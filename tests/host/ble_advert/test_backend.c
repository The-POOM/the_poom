// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../applications/poom_ble_spam/poom_ble_spam.c"
static unsigned checks,clock_ms,starts,stops,addresses,initializations,releases;
static int ctrl,host,failure;
static bool fail_task,fail_release,cancel_pending,mock_advertising;
static unsigned cancel_at;
enum {GOOD,INIT_FAIL,ADDRESS_FAIL,CONFIG_FAIL,START_FAIL,START_TIMEOUT,STOP_FAIL};
static void(*gap_callback)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *);
#define CHECK(x) do {assert(x);++checks;} while(0)
typedef struct {adv_event_t data[8];unsigned used,read;} queue_t;
QueueHandle_t xQueueCreate(unsigned count,size_t size) {CHECK(count==8 && size==sizeof(adv_event_t));return calloc(1,sizeof(queue_t));}
int xQueueSend(QueueHandle_t q,const void *p,unsigned ticks) {(void)ticks;queue_t*x=q;if(x->used==8)return 0;x->data[(x->read+x->used)%8]=*(const adv_event_t*)p;++x->used;return 1;}
int xQueueReceive(QueueHandle_t q,void *p,unsigned ticks) {queue_t*x=q;if(x->used){*(adv_event_t*)p=x->data[x->read];x->read=(x->read+1)%8;--x->used;return 1;}clock_ms+=ticks;return 0;}
void xQueueReset(QueueHandle_t q) {memset(q,0,sizeof(queue_t));}
void vQueueDelete(QueueHandle_t q) {free(q);}
int xTaskCreate(void(*task)(void*),const char*name,unsigned stack,void*arg,unsigned priority,void*handle)
{(void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return !fail_task;}
void vTaskDelay(unsigned ticks) {clock_ms+=ticks;if(cancel_at && clock_ms>=cancel_at)stopping=true;}
void vTaskDelete(void*task) {(void)task;}
int64_t esp_timer_get_time(void) {return (int64_t)clock_ms*1000;}
uint32_t esp_random(void) {return clock_ms*997+123;}
void esp_fill_random(void*out,size_t length) {memset(out,0,length);}
int esp_bt_controller_get_status(void) {return ctrl;}
int esp_bluedroid_get_status(void) {return host;}
esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *config) {(void)config;CHECK(!ctrl);ctrl=1;++initializations;return ESP_OK;}
esp_err_t esp_bt_controller_enable(int mode) {(void)mode;CHECK(ctrl==1);ctrl=2;return ESP_OK;}
esp_err_t esp_bt_controller_disable(void) {CHECK(ctrl==2 && host!=2);ctrl=1;return ESP_OK;}
esp_err_t esp_bt_controller_deinit(void) {CHECK(ctrl==1 && host==0);ctrl=0;++releases;return ESP_OK;}
esp_err_t esp_bluedroid_init(void) {CHECK(ctrl==2 && !host);host=1;return ESP_OK;}
esp_err_t esp_bluedroid_enable(void) {CHECK(host==1);if(failure==INIT_FAIL)return ESP_FAIL;host=2;return ESP_OK;}
esp_err_t esp_bluedroid_disable(void) {CHECK(host==2);if(fail_release)return ESP_FAIL;host=1;mock_advertising=false;return ESP_OK;}
esp_err_t esp_bluedroid_deinit(void) {CHECK(host==1);host=0;return ESP_OK;}
esp_err_t esp_ble_gap_register_callback(void(*cb)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *)) {gap_callback=cb;return ESP_OK;}
static void emit_(esp_gap_ble_cb_event_t event,int result) {esp_ble_gap_cb_param_t p={0};p.adv_start_cmpl.status=result;gap_callback(event,&p);}
esp_err_t esp_ble_gap_set_rand_addr(uint8_t address[6]) {CHECK((address[0]&0xc0)==0xc0 && address[5]==1 && !mock_advertising);++addresses;emit_(ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT,failure==ADDRESS_FAIL);return ESP_OK;}
esp_err_t esp_ble_gap_config_adv_data_raw(uint8_t *data,uint32_t length) {CHECK(length>0 && length<=31 && data[0]);emit_(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT,failure==CONFIG_FAIL);return ESP_OK;}
esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t *params)
{
    CHECK(params->adv_type==ADV_TYPE_NONCONN_IND && params->adv_int_min==0xa0 && params->own_addr_type==BLE_ADDR_TYPE_RANDOM);
    if(failure==START_TIMEOUT)return ESP_OK;
    if(failure!=START_FAIL){++starts;mock_advertising=true;}
    emit_(ESP_GAP_BLE_ADV_START_COMPLETE_EVT,failure==START_FAIL);
    if(cancel_pending)stopping=true;
    return ESP_OK;
}
esp_err_t esp_ble_gap_stop_advertising(void) {++stops;mock_advertising=false;emit_(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT,failure==STOP_FAIL);return ESP_OK;}
static poom_ble_spam_status_t run_(int scenario,poom_ble_spam_mode_t mode)
{
    failure=scenario;clock_ms=starts=stops=addresses=initializations=releases=0;
    const poom_ble_spam_config_t config={.mode=mode,.duration_ms=scenario==START_TIMEOUT?5000U:1000U};
    CHECK(poom_ble_spam_start_mode(&config)==ESP_OK && poom_ble_spam_is_active());
    CHECK(poom_ble_spam_start_mode(&config)==ESP_ERR_INVALID_STATE);
    worker_(NULL);
    poom_ble_spam_status_t result;poom_ble_spam_get_status(&result);
    CHECK(!result.active && !result.advertising && result.radio_released && !ctrl && !host);
    CHECK(initializations==releases && poom_ble_spam_stop()==ESP_OK);
    return result;
}
int main(void)
{
    poom_ble_spam_status_t s=run_(GOOD,POOM_BLE_SPAM_APPLE_DEVICES);
    CHECK(s.error==ESP_OK && s.cycles==2 && starts==2 && stops==2 && s.elapsed_ms==1000);
    s=run_(GOOD,POOM_BLE_SPAM_SOUR_APPLE);CHECK(s.cycles==7 && starts==7 && stops==7);
    for(int failure_mode=INIT_FAIL;failure_mode<=STOP_FAIL;++failure_mode) {
        s=run_(failure_mode,POOM_BLE_SPAM_ALL);
        CHECK(s.error!=ESP_OK);
        if(failure_mode<STOP_FAIL)CHECK(s.cycles==0);
    }
    cancel_at=120;s=run_(GOOD,POOM_BLE_SPAM_ALL);
    CHECK(s.error==ESP_OK && s.cycles==1 && starts==1 && stops==1 && s.elapsed_ms==120);cancel_at=0;
    /* Overall expiry during an ACK wait is normal completion; a 3s stage timeout
     * while session time remains is the separate failure tested above. */
    failure=START_TIMEOUT;clock_ms=0;
    CHECK(poom_ble_spam_start_mode(&(poom_ble_spam_config_t){.duration_ms=1000})==ESP_OK);worker_(NULL);
    CHECK(status.error==ESP_OK && status.cycles==0 && status.elapsed_ms==1000 && status.radio_released);
    cancel_pending=true;s=run_(GOOD,POOM_BLE_SPAM_ALL);
    CHECK(s.error==ESP_OK && starts==1 && s.cycles==0 && s.radio_released);cancel_pending=false;
    fail_task=true;CHECK(poom_ble_spam_start()==ESP_ERR_NO_MEM);fail_task=false;
    CHECK(!poom_ble_spam_is_active() && !events);
    poom_ble_spam_config_t bad={.mode=POOM_BLE_SPAM_MODE_COUNT};CHECK(poom_ble_spam_start_mode(&bad)==ESP_ERR_INVALID_ARG);
    bad.mode=POOM_BLE_SPAM_ALL;bad.duration_ms=60001;CHECK(poom_ble_spam_start_mode(&bad)==ESP_ERR_INVALID_ARG);
    bad.duration_ms=0;bad.dwell_ms=99;CHECK(poom_ble_spam_start_mode(&bad)==ESP_ERR_INVALID_ARG);
    ctrl=host=2;CHECK(poom_ble_spam_start()==ESP_OK);worker_(NULL);
    CHECK(status.error==ESP_ERR_INVALID_STATE && ctrl==2 && host==2);ctrl=host=0;
    fail_release=true;failure=GOOD;CHECK(poom_ble_spam_start_mode(&(poom_ble_spam_config_t){.duration_ms=1000})==ESP_OK);worker_(NULL);
    CHECK(!status.active && !status.radio_released && poom_ble_spam_stop()==ESP_FAIL);
    CHECK(poom_ble_spam_start()==ESP_ERR_INVALID_STATE);fail_release=false;CHECK(cleanup_()==ESP_OK);
    printf("BLE advertising backend: %u checks passed\n",checks);
    return 0;
}
