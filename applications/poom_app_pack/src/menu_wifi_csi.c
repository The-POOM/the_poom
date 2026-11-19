// SPDX-License-Identifier: GPL-3.0-or-later
/* Native CSI action -> passive AP scan -> choose -> calibration -> RF estimate. */
#include "menu_wifi_csi.h"
#include "poom_wifi_csi.h"
#include "poom_wifi_scanner.h"
#include "poom_wifi_ctrl.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { CSI_SCAN, CSI_APS, CSI_RUN, CSI_ERROR } csi_view_t;
typedef struct {bool scan_done; uint8_t button; esp_err_t error;} csi_event_t;
typedef struct {
    poom_wifi_csi_t *receiver;
    QueueHandle_t events;
    wifi_ap_record_t aps[POOM_WIFI_SCANNER_MAX_AP];
    poom_csi_status_t status;
    uint16_t count, selected;
    csi_view_t view;
    esp_err_t error;
    bool scanning, cancel, subscribed;
} csi_ui_t;
static csi_ui_t *s_ui;
static portMUX_TYPE s_input_lock=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_input_queue;
static bool s_stop_requested;
static const char s_user[]="wifi_csi_ui";
bool menu_wifi_csi_is_active(void){portENTER_CRITICAL(&s_input_lock);const bool active=s_ui!=NULL;portEXIT_CRITICAL(&s_input_lock);return active;}
bool menu_wifi_csi_stop(void){
    portENTER_CRITICAL(&s_input_lock);s_stop_requested=true;portEXIT_CRITICAL(&s_input_lock);
    for(unsigned i=0;i<500U && menu_wifi_csi_is_active();++i)vTaskDelay(pdMS_TO_TICKS(10U));
    return !menu_wifi_csi_is_active();
}
static void resume(void){const uint8_t token=1;(void)poom_sbus_publish("poom/menu/resume",&token,1,0);}
static void line(unsigned y,const char *text){char trim[22];snprintf(trim,sizeof(trim),"%.21s",text);poom_arduboy_set_cursor(0,y);(void)poom_arduboy_print(trim);}
static void input(const poom_sbus_msg_t *msg,void *user){
    (void)user;if(!msg || msg->len<sizeof(button_event_msg_t))return;
    button_event_msg_t button;memcpy(&button,msg->data,sizeof(button));if(button.event!=BUTTON_SINGLE_CLICK)return;
    const csi_event_t event={.button=button.button};
    portENTER_CRITICAL(&s_input_lock);if(s_input_queue)(void)xQueueSend(s_input_queue,&event,0);portEXIT_CRITICAL(&s_input_lock);
}
static bool cancelled(csi_ui_t *s){portENTER_CRITICAL(&s_input_lock);bool value=s->cancel||s_stop_requested;portEXIT_CRITICAL(&s_input_lock);return value;}
static void scan_task(void *arg){
    csi_ui_t *s=arg;
    const wifi_scan_config_t config={.show_hidden=true,.scan_type=WIFI_SCAN_TYPE_PASSIVE,.scan_time.passive=100U};
    esp_err_t error=cancelled(s)?ESP_ERR_INVALID_STATE:poom_wifi_scanner_scan_configured(&config);
    s->count=0;
    if(error==ESP_OK){
        const poom_wifi_scanner_ap_records_t *found=poom_wifi_scanner_get_ap_records();
        for(uint16_t i=0;i<found->count && s->count<POOM_WIFI_SCANNER_MAX_AP;++i)
            if(poom_wifi_ctrl_channel_is_valid(found->records[i].primary))s->aps[s->count++]=found->records[i];
    }
    const QueueHandle_t queue=s->events;
    const csi_event_t event={.scan_done=true,.error=error};
    /* Completion is the lifetime barrier. No session accesses after this send. */
    (void)xQueueSend(queue,&event,portMAX_DELAY);vTaskDelete(NULL);
}
static void begin_scan(csi_ui_t *s){
    s->error=poom_wifi_csi_stop(&s->receiver);
    if(s->error!=ESP_OK){s->view=CSI_ERROR;return;}
    s->view=CSI_SCAN;s->selected=0;s->count=0;s->scanning=true;
    portENTER_CRITICAL(&s_input_lock);s->cancel=false;portEXIT_CRITICAL(&s_input_lock);
    if(xTaskCreate(scan_task,"csi_ap_scan",4096U,s,4U,NULL)!=pdPASS){s->scanning=false;s->error=ESP_ERR_NO_MEM;s->view=CSI_ERROR;}
}
static void draw(const csi_ui_t *s){
    char text[64];poom_arduboy_clear();poom_arduboy_set_text_size(1);line(0,"CSI RF MOTION");
    if(s->view==CSI_SCAN){line(19,s->cancel?"FINISHING SCAN...":"SCANNING LAB APS...");line(37,"PASSIVE AP DISCOVERY");line(56,"B:CANCEL");}
    else if(s->view==CSI_ERROR){line(19,"COULD NOT START");line(28,esp_err_to_name(s->error));line(56,"A:RESCAN B:BACK");}
    else if(s->view==CSI_APS){
        if(!s->count){line(19,"NO APS FOUND");line(37,"A:RESCAN");}
        else {
            const wifi_ap_record_t *a=&s->aps[s->selected];char name[33];
            size_t n=0;while(n<32U && a->ssid[n]){name[n]=(a->ssid[n]>=32U && a->ssid[n]<127U)?a->ssid[n]:'.';++n;}name[n]=0;
            snprintf(text,sizeof(text),"AP %u/%u CH%u %ddB",s->selected+1U,s->count,a->primary,a->rssi);line(10,text);line(20,name[0]?name:"HIDDEN AP");
            snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",a->bssid[0],a->bssid[1],a->bssid[2],a->bssid[3],a->bssid[4],a->bssid[5]);line(30,text);
            line(43,"KEEP STILL FOR 30s");
        }
        line(56,"U/D:AP A:GO B:BACK");
    } else {
        const poom_csi_status_t *v=&s->status;
        snprintf(text,sizeof(text),"CH%u %u/s %ddB Q:%u",v->channel,v->samples_per_second,v->rssi,v->reliability);line(9,text);
        if(!v->calibrated){
            snprintf(text,sizeof(text),"CALIBRATING %u%%",v->calibration);line(19,text);
            line(29,v->calibration==100U?"NEED MORE SAMPLES":"KEEP POOM/ROOM STILL");
            snprintf(text,sizeof(text),"SAMPLES:%lu DROP:%lu",(unsigned long)v->accepted,(unsigned long)v->dropped);line(39,text);
        } else {
            static const char *const states[]={"CALIBRATING","INSUFFICIENT SAMPLES","RF STILL","RF CHANGE POSSIBLE","RF MOTION ESTIMATE"};
            line(19,states[v->state]);
            snprintf(text,sizeof(text),"M:%u A:%u P:%u C:%u",v->score,v->amplitude,v->phase,v->correlation);line(29,text);
            poom_arduboy_draw_rect(0,39,128,14,1);
            for(unsigned i=1;i<64U;++i)if(v->history[i]!=255U && v->history[i-1]!=255U)
                poom_arduboy_draw_line(1+2*(i-1),51-v->history[i-1]*11/100,1+2*i,51-v->history[i]*11/100,1);
        }
        line(56,"A:RECAL B:STOP");
    }
    poom_arduboy_display();
}
static void task(void *arg){
    csi_ui_t *s=arg;bool running=true;begin_scan(s);
    while(running){
        if(cancelled(s)) {if(s->scanning)(void)esp_wifi_scan_stop();else break;}
        if(s->receiver)poom_wifi_csi_poll(s->receiver,&s->status);
        csi_event_t event;
        if(xQueueReceive(s->events,&event,pdMS_TO_TICKS(30))==pdTRUE){
            if(event.scan_done){
                s->scanning=false;
                if(cancelled(s))running=false;
                else if(event.error!=ESP_OK){s->error=event.error;s->view=CSI_ERROR;}
                else s->view=CSI_APS;
            } else if(s->scanning){
                if(event.button==BUTTON_B){portENTER_CRITICAL(&s_input_lock);s->cancel=true;portEXIT_CRITICAL(&s_input_lock);(void)esp_wifi_scan_stop();}
            } else if(event.button==BUTTON_B)running=false;
            else if(s->view==CSI_RUN){if(event.button==BUTTON_A)poom_wifi_csi_recalibrate(s->receiver);}
            else if(s->view==CSI_ERROR || !s->count){if(event.button==BUTTON_A)begin_scan(s);}
            else if(event.button==BUTTON_UP || event.button==BUTTON_DOWN)s->selected=(s->selected+(event.button==BUTTON_UP?s->count-1U:1U))%s->count;
            else if(event.button==BUTTON_A){
                const wifi_ap_record_t *a=&s->aps[s->selected];
                s->error=poom_wifi_csi_start(&s->receiver,a->bssid,a->primary);
                s->view=s->error==ESP_OK?CSI_RUN:CSI_ERROR;
                if(s->receiver)poom_wifi_csi_poll(s->receiver,&s->status);
            }
        }
        if(running)draw(s);
    }
    esp_err_t stop_error;
    do {
        stop_error=poom_wifi_csi_stop(&s->receiver);
        if(stop_error==ESP_OK)stop_error=poom_wifi_ctrl_deinit();
        if(stop_error!=ESP_OK){poom_arduboy_clear();line(0,"RADIO STOP FAILED");line(19,esp_err_to_name(stop_error));line(37,"RETRYING TEARDOWN");poom_arduboy_display();vTaskDelay(pdMS_TO_TICKS(250U));}
    } while(stop_error!=ESP_OK);
    (void)poom_wifi_scanner_clear_ap_records();
    portENTER_CRITICAL(&s_input_lock);s_input_queue=NULL;portEXIT_CRITICAL(&s_input_lock);
    if(s->subscribed)(void)poom_sbus_unsubscribe_cb("input/button",input,(void *)s_user);
    vQueueDelete(s->events);portENTER_CRITICAL(&s_input_lock);s_ui=NULL;portEXIT_CRITICAL(&s_input_lock);free(s);resume();vTaskDelete(NULL);
}
void menu_wifi_csi_show(void){
    if(menu_wifi_csi_is_active())return;
    csi_ui_t *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s){resume();return;}
    portENTER_CRITICAL(&s_input_lock);s_ui=s;s_stop_requested=false;portEXIT_CRITICAL(&s_input_lock);
    s->events=xQueueCreate(12,sizeof(csi_event_t));
    if(s->events){
        portENTER_CRITICAL(&s_input_lock);s_input_queue=s->events;portEXIT_CRITICAL(&s_input_lock);
        s->subscribed=poom_sbus_subscribe_cb("input/button",input,(void *)s_user);
        if(s->subscribed && xTaskCreate(task,"wifi_csi_ui",6144U,s,4U,NULL)==pdPASS)return;
    }
    portENTER_CRITICAL(&s_input_lock);s_input_queue=NULL;portEXIT_CRITICAL(&s_input_lock);
    if(s->subscribed)(void)poom_sbus_unsubscribe_cb("input/button",input,(void *)s_user);
    if(s->events)vQueueDelete(s->events);
    free(s);portENTER_CRITICAL(&s_input_lock);s_ui=NULL;portEXIT_CRITICAL(&s_input_lock);resume();
}
