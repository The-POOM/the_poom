// SPDX-License-Identifier: MIT
// Actual backend source, deterministic mock GAP/GATT transport, no Bluetooth hardware.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../modules/poom_findmy_beep/poom_findmy_beep.c"

static unsigned checks, clock_ms, opens, writes, initializations, releases;
static int ctrl, host, mode, chosen_profile;
static bool fail_task, auto_begin, fail_disable, fail_host_enable, cancel_write;
static uint32_t cancel_at;
static size_t auto_count;
static uint8_t connected_mac[6], written[3];
static unsigned written_length;
static void (*gap_cb)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *);
static void (*gatt_cb)(esp_gattc_cb_event_t,esp_gatt_if_t,esp_ble_gattc_cb_param_t *);
enum {GOOD,OPEN_TIMEOUT,WRITE_REJECT,NO_SERVICE,NO_REPLY,DULT_REJECT,WRITE_TIMEOUT,OPEN_FAIL};
#define CHECK(x) do { assert(x); ++checks; } while(0)
typedef struct {size_t size;unsigned used,read;unsigned char data[24][sizeof(radio_event_t)];} queue_t;
QueueHandle_t xQueueCreate(unsigned count,size_t size) {CHECK(count==24);queue_t*q=calloc(1,sizeof(*q));q->size=size;return q;}
int xQueueSend(QueueHandle_t queue,const void *item,unsigned ticks) {queue_t*q=queue;(void)ticks;if(q->used==24)return 0;memcpy(q->data[(q->read+q->used)%24],item,q->size);++q->used;return 1;}
int xQueueReceive(QueueHandle_t queue,void *item,unsigned ticks)
{
    queue_t*q=queue;
    if(cancel_at && clock_ms>=cancel_at) stopping=true;
    if(q->used){memcpy(item,q->data[q->read],q->size);q->read=(q->read+1)%24;--q->used;return 1;}
    if(auto_begin && state.phase==POOM_FINDMY_SCANNING) {
        poom_findmy_key_t keys[POOM_FINDMY_MAX_DEVICES];
        const size_t count=poom_findmy_freeze(devices,device_count,clock_ms,keys,auto_count);
        CHECK(count==auto_count);
        CHECK(poom_findmy_beep_begin(keys,count)==ESP_OK);
        auto_begin=false;
    }
    clock_ms+=ticks;
    return 0;
}
void xQueueReset(QueueHandle_t queue) {queue_t*q=queue;q->used=q->read=0;}
void vQueueDelete(QueueHandle_t queue) {free(queue);}
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{(void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return !fail_task;}
void vTaskDelay(unsigned ticks) {clock_ms+=ticks;}
void vTaskDelete(void *task) {(void)task;}
int64_t esp_timer_get_time(void) {return (int64_t)clock_ms*1000;}
int esp_bt_controller_get_status(void) {return ctrl;}
int esp_bluedroid_get_status(void) {return host;}
esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *config) {(void)config;CHECK(ctrl==0);ctrl=1;++initializations;return ESP_OK;}
esp_err_t esp_bt_controller_enable(int value) {(void)value;CHECK(ctrl==1);ctrl=2;return ESP_OK;}
esp_err_t esp_bt_controller_disable(void) {CHECK(ctrl==2 && host!=2);ctrl=1;return ESP_OK;}
esp_err_t esp_bt_controller_deinit(void) {CHECK(ctrl==1 && host==0);ctrl=0;++releases;return ESP_OK;}
esp_err_t esp_bluedroid_init(void) {CHECK(ctrl==2 && host==0);host=1;return ESP_OK;}
esp_err_t esp_bluedroid_enable(void) {CHECK(host==1);if(fail_host_enable)return ESP_FAIL;host=2;return ESP_OK;}
esp_err_t esp_bluedroid_disable(void) {CHECK(host==2);if(fail_disable)return ESP_FAIL;host=1;return ESP_OK;}
esp_err_t esp_bluedroid_deinit(void) {CHECK(host==1);host=0;return ESP_OK;}
esp_err_t esp_ble_gap_register_callback(void (*cb)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *)) {gap_cb=cb;return ESP_OK;}
esp_err_t esp_ble_gattc_register_callback(void (*cb)(esp_gattc_cb_event_t,esp_gatt_if_t,esp_ble_gattc_cb_param_t *)) {gatt_cb=cb;return ESP_OK;}
static void emit_(esp_gattc_cb_event_t event,esp_ble_gattc_cb_param_t *param) {gatt_cb(event,7,param);}
static void report_(unsigned id,int type,bool classified)
{
    esp_ble_gap_cb_param_t p={0};
    p.scan_rst.bda[5]=(uint8_t)id;
    p.scan_rst.ble_addr_type=id%2;
    p.scan_rst.ble_evt_type=type;
    p.scan_rst.rssi=-55;
    p.scan_rst.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT;
    if(classified) {
        const uint8_t ad[]={3,3,0x44,0xfd};
        memcpy(p.scan_rst.ble_adv,ad,sizeof(ad));
        if(type==ESP_BLE_EVT_SCAN_RSP)p.scan_rst.scan_rsp_len=sizeof(ad);
        else p.scan_rst.adv_data_len=sizeof(ad);
    }
    gap_cb(ESP_GAP_BLE_SCAN_RESULT_EVT,&p);
}
esp_err_t esp_ble_gattc_app_register(uint16_t app) {esp_ble_gattc_cb_param_t p={0};p.reg.app_id=app;emit_(ESP_GATTC_REG_EVT,&p);return ESP_OK;}
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *params) {CHECK(params->scan_duplicate==BLE_SCAN_DUPLICATE_DISABLE);esp_ble_gap_cb_param_t p={0};gap_cb(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,&p);return ESP_OK;}
esp_err_t esp_ble_gap_start_scanning(uint32_t seconds)
{
    CHECK(!seconds);esp_ble_gap_cb_param_t p={0};gap_cb(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&p);
    for(unsigned i=1;i<=auto_count;++i)report_(i,ESP_BLE_EVT_CONN_ADV,true);
    return ESP_OK;
}
esp_err_t esp_ble_gap_security_rsp(uint8_t bda[6],bool accept) {(void)bda;CHECK(!accept);return ESP_OK;}
esp_err_t esp_ble_confirm_reply(uint8_t bda[6],bool accept) {(void)bda;CHECK(!accept);return ESP_OK;}
esp_err_t esp_ble_gattc_open(esp_gatt_if_t interface,esp_bd_addr_t bda,esp_ble_addr_type_t type,bool direct)
{
    CHECK(interface==7 && direct);CHECK(type==(int)(bda[5]%2));CHECK(initializations==releases+1);++opens;memcpy(connected_mac,bda,6);
    if(mode==OPEN_TIMEOUT)return ESP_OK;
    esp_ble_gattc_cb_param_t p={0};
    if(mode!=OPEN_FAIL) {p.connect.conn_id=9;p.connect.ble_addr_type=type;memcpy(p.connect.remote_bda,bda,6);emit_(ESP_GATTC_CONNECT_EVT,&p);}
    memset(&p,0,sizeof(p));p.open.conn_id=9;p.open.status=mode==OPEN_FAIL?5:0;memcpy(p.open.remote_bda,bda,6);emit_(ESP_GATTC_OPEN_EVT,&p);
    if(mode!=OPEN_FAIL){memset(&p,0,sizeof(p));p.dis_srvc_cmpl.conn_id=9;emit_(ESP_GATTC_DIS_SRVC_CMPL_EVT,&p);}
    return ESP_OK;
}
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t interface,uint16_t connection,esp_bt_uuid_t *uuid)
{
    CHECK(interface==7 && connection==9 && !uuid);esp_ble_gattc_cb_param_t p={0};
    if(mode!=NO_SERVICE) {
        p.search_res.conn_id=9;p.search_res.start_handle=10;p.search_res.end_handle=90;
        const poom_findmy_profile_t*profile=poom_findmy_profile((poom_findmy_protocol_t)chosen_profile);
        p.search_res.srvc_id.uuid=uuid_(profile->service128);
        if(profile->service16){p.search_res.srvc_id.uuid.len=2;p.search_res.srvc_id.uuid.uuid.uuid16=profile->service16;}
        emit_(ESP_GATTC_SEARCH_RES_EVT,&p);
    }
    memset(&p,0,sizeof(p));p.search_cmpl.conn_id=9;emit_(ESP_GATTC_SEARCH_CMPL_EVT,&p);return ESP_OK;
}
esp_gatt_status_t esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t interface,uint16_t connection,uint16_t start,uint16_t end,esp_bt_uuid_t uuid,esp_gattc_char_elem_t *result,uint16_t *count)
{
    CHECK(interface==7 && connection==9 && start==10 && end==90 && *count==1);
    CHECK(uuid.len==16 && !memcmp(uuid.uuid.uuid128,poom_findmy_profile((poom_findmy_protocol_t)chosen_profile)->characteristic128,16));
    result->char_handle=42;result->properties=ESP_GATT_CHAR_PROP_BIT_WRITE|ESP_GATT_CHAR_PROP_BIT_INDICATE;return ESP_GATT_OK;
}
esp_err_t esp_ble_gattc_register_for_notify(esp_gatt_if_t interface,esp_bd_addr_t bda,uint16_t handle)
{CHECK(interface==7 && handle==42 && !memcmp(bda,connected_mac,6));esp_ble_gattc_cb_param_t p={0};p.reg_for_notify.handle=handle;emit_(ESP_GATTC_REG_FOR_NOTIFY_EVT,&p);return ESP_OK;}
esp_gatt_status_t esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,esp_bt_uuid_t uuid,esp_gattc_descr_elem_t *result,uint16_t *count)
{CHECK(interface==7 && connection==9 && handle==42 && uuid.uuid.uuid16==0x2902 && *count==1);result->handle=43;return ESP_GATT_OK;}
esp_err_t esp_ble_gattc_write_char_descr(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,uint16_t length,uint8_t *value,int type,int auth)
{CHECK(interface==7 && connection==9 && handle==43 && length==2 && value[0]==2 && value[1]==0 && type==1 && auth==0);esp_ble_gattc_cb_param_t p={0};p.write.conn_id=connection;p.write.handle=handle;emit_(ESP_GATTC_WRITE_DESCR_EVT,&p);return ESP_OK;}
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t interface,uint16_t connection,uint16_t handle,uint16_t length,uint8_t *value,int type,int auth)
{
    CHECK(interface==7 && connection==9 && handle==42 && length<=3 && type==1 && auth==0);
    ++writes;written_length=length;memcpy(written,value,length);
    if(cancel_write) stopping=true;
    if(mode==WRITE_TIMEOUT)return ESP_OK;
    esp_ble_gattc_cb_param_t p={0};p.write.conn_id=connection;p.write.handle=handle;p.write.status=mode==WRITE_REJECT?5:0;emit_(ESP_GATTC_WRITE_CHAR_EVT,&p);
    if(chosen_profile==POOM_FINDMY_DULT_SOUND && mode!=NO_REPLY) {
        uint8_t value_reply[]={2,3,0,3,mode==DULT_REJECT?2:0,0};
        memset(&p,0,sizeof(p));p.notify.conn_id=connection;p.notify.handle=handle;p.notify.value_len=6;p.notify.value=value_reply;memcpy(p.notify.remote_bda,connected_mac,6);emit_(ESP_GATTC_NOTIFY_EVT,&p);
    }
    return ESP_OK;
}
static poom_findmy_status_t run_(int scenario,int profile,size_t targets)
{
    mode=scenario;chosen_profile=profile;clock_ms=100;opens=writes=initializations=releases=0;
    auto_count=targets;auto_begin=targets!=0;
    CHECK(poom_findmy_beep_scan()==ESP_OK);
    CHECK(poom_findmy_beep_scan()==ESP_ERR_INVALID_STATE);
    worker_(NULL);
    poom_findmy_status_t status;poom_findmy_beep_status(&status);
    CHECK(!status.active && status.phase==POOM_FINDMY_FINISHED && status.radio_released);
    CHECK(!host && !ctrl && initializations==releases);
    poom_findmy_beep_stop();
    return status;
}
int main(void)
{
    poom_findmy_status_t status=run_(GOOD,POOM_FINDMY_AIRTAG,2);
    CHECK(status.count==2 && opens==2 && writes==2 && initializations==3);
    CHECK(status.results[0].code==POOM_FINDMY_ACKNOWLEDGED && status.results[1].write_acknowledged);
    CHECK(written_length==1 && written[0]==0xaf && clock_ms<CONNECT_MS);
    status=run_(OPEN_TIMEOUT,POOM_FINDMY_AIRTAG,2);
    CHECK(opens==2 && writes==0 && clock_ms>=14000 && clock_ms<14500);
    CHECK(status.results[0].code==POOM_FINDMY_TIMEOUT && status.results[1].code==POOM_FINDMY_TIMEOUT);
    status=run_(OPEN_TIMEOUT,POOM_FINDMY_AIRTAG,6);
    CHECK(opens==5 && writes==0 && status.results[5].code==POOM_FINDMY_STALE);
    status=run_(WRITE_REJECT,POOM_FINDMY_AIRTAG,1);
    CHECK(status.results[0].code==POOM_FINDMY_REJECTED && !status.results[0].write_acknowledged && status.results[0].att_status==5);
    status=run_(NO_SERVICE,POOM_FINDMY_AIRTAG,1);
    CHECK(status.results[0].code==POOM_FINDMY_UNSUPPORTED && writes==0);
    status=run_(GOOD,POOM_FINDMY_DULT_SOUND,1);
    CHECK(status.results[0].write_acknowledged && status.results[0].reply==POOM_FINDMY_REPLY_ACCEPTED);
    CHECK(written_length==2 && written[0]==0 && written[1]==3 && clock_ms>=3000 && clock_ms<3500);
    status=run_(DULT_REJECT,POOM_FINDMY_DULT_SOUND,1);
    CHECK(status.results[0].code==POOM_FINDMY_REJECTED && status.results[0].write_acknowledged && status.results[0].response_status==2);
    status=run_(NO_REPLY,POOM_FINDMY_ACCESSORY,1);
    CHECK(status.results[0].code==POOM_FINDMY_ACKNOWLEDGED && status.results[0].reply==POOM_FINDMY_REPLY_NONE && clock_ms>=3000);
    CHECK(written_length==3 && !memcmp(written,(uint8_t[]){1,0,3},3));
    status=run_(WRITE_TIMEOUT,POOM_FINDMY_AIRTAG,1);
    CHECK(status.results[0].code==POOM_FINDMY_TIMEOUT && !status.results[0].write_acknowledged);
    status=run_(OPEN_FAIL,POOM_FINDMY_AIRTAG,1);
    CHECK(status.results[0].code==POOM_FINDMY_FAILED && status.results[0].att_status==5 && writes==0);
    cancel_at=1000;
    status=run_(OPEN_TIMEOUT,POOM_FINDMY_AIRTAG,2);
    CHECK(status.results[0].code==POOM_FINDMY_CANCELLED && status.results[1].code==POOM_FINDMY_CANCELLED && opens==1 && writes==0);
    cancel_at=0;
    cancel_write=true;
    status=run_(GOOD,POOM_FINDMY_AIRTAG,2);
    CHECK(writes==1 && status.results[0].code==POOM_FINDMY_CANCELLED && status.results[0].command_submitted && !status.results[0].write_acknowledged);
    CHECK(status.results[1].code==POOM_FINDMY_CANCELLED);
    cancel_write=false;
    fail_host_enable=true;
    status=run_(GOOD,POOM_FINDMY_AIRTAG,1);
    CHECK(status.error==ESP_FAIL && writes==0 && initializations==1 && releases==1);
    fail_host_enable=false;
    /* Scan response-only UUIDs use the original advertiser's connectability. */
    CHECK(poom_findmy_beep_scan()==ESP_OK);accepting_reports=true;gap_cb=gap_;clock_ms=100;
    report_(3,ESP_BLE_EVT_CONN_ADV,false);report_(3,ESP_BLE_EVT_SCAN_RSP,true);
    CHECK(device_count==1 && poom_findmy_eligible(&devices[0],clock_ms));
    report_(4,ESP_BLE_EVT_DISC_ADV,false);report_(4,ESP_BLE_EVT_SCAN_RSP,true);
    CHECK(device_count==2 && !poom_findmy_eligible(&devices[1],clock_ms));
    report_(5,ESP_BLE_EVT_SCAN_RSP,true);
    CHECK(device_count==3 && !poom_findmy_eligible(&devices[2],clock_ms));
    state.phase=POOM_FINDMY_SCANNING;
    CHECK(poom_findmy_beep_begin(&devices[1].key,1)==ESP_ERR_NOT_FOUND);
    poom_findmy_key_t duplicate[]={devices[0].key,devices[0].key};
    CHECK(poom_findmy_beep_begin(duplicate,2)==ESP_ERR_INVALID_ARG);
    CHECK(poom_findmy_beep_begin(&devices[0].key,1)==ESP_OK);
    CHECK(poom_findmy_beep_begin(&devices[0].key,1)==ESP_ERR_INVALID_STATE);
    vQueueDelete(events);events=NULL;state.active=false;stopping=true;
    fail_task=true;CHECK(poom_findmy_beep_scan()==ESP_ERR_NO_MEM);fail_task=false;
    CHECK(!state.active && !events);
    /* Refuse an unrelated stack instead of silently deinitializing its owner. */
    ctrl=2;host=2;CHECK(poom_findmy_beep_scan()==ESP_OK);worker_(NULL);
    CHECK(state.error==ESP_ERR_INVALID_STATE && ctrl==2 && host==2);
    ctrl=host=0;
    /* A persistent teardown failure is reported and blocks radio reuse. */
    auto_count=1;auto_begin=true;fail_disable=true;clock_ms=100;
    CHECK(poom_findmy_beep_scan()==ESP_OK);worker_(NULL);
    CHECK(!state.active && !state.radio_released && state.error==ESP_FAIL);
    CHECK(poom_findmy_beep_stop()==ESP_FAIL && poom_findmy_beep_scan()==ESP_ERR_INVALID_STATE);
    fail_disable=false;CHECK(cleanup_()==ESP_OK);
    printf("Find My native backend: %u checks passed\n",checks);
    return 0;
}
