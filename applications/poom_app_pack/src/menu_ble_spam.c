// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "menu_ble_spam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "poom_ble_spam.h"
#include "poom_ble_tracker.h"
#include "poom_ble_scan.h"
#include "poom_ble_detector.h"
#include "poom_ble_gatt_client.h"
#include "poom_sbus.h"

enum {KEY_A,KEY_B,KEY_LEFT,KEY_RIGHT,KEY_UP,KEY_DOWN};
typedef struct {
    QueueHandle_t input;
    bool subscribed;
    bool has_run;
    bool awaiting_result;
    poom_ble_spam_config_t config;
    poom_ble_spam_status_t status;
    esp_err_t error;
} session_t;
static portMUX_TYPE input_lock=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t input_queue;
static session_t *session;
static bool stopping;
static const char subscriber[]="menu_ble_spam";

static void print_(int y,const char *text)
{
    char line[22];
    (void)snprintf(line,sizeof(line),"%.21s",text);
    poom_arduboy_set_cursor(1,y);
    (void)poom_arduboy_print(line);
}
static void draw_(session_t *s)
{
    char text[64];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    print_(1,"BLE ADVERTISING");
    poom_arduboy_fill_rect(0,0,ARDUBOY_WIDTH,10,INVERT);
    print_(14,poom_ble_spam_mode_name(s->status.active?s->status.mode:s->config.mode));
    if(s->status.active) {
        print_(25,s->status.label[0]?s->status.label:"Starting Bluetooth...");
        (void)snprintf(text,sizeof(text),"%lu cycles %lu/%lus",(unsigned long)s->status.cycles,
                       (unsigned long)(s->status.elapsed_ms/1000U),(unsigned long)(s->status.duration_ms/1000U));
        print_(36,text);
        print_(46,"Receiver effect unknown");
        print_(56,"A:STOP B:BACK");
    } else {
        (void)snprintf(text,sizeof(text),"%lus  LEFT/RIGHT: time",(unsigned long)(s->config.duration_ms/1000U));
        print_(25,text);
        if(s->error!=ESP_OK) print_(36,esp_err_to_name(s->error));
        else if(s->has_run) {
            (void)snprintf(text,sizeof(text),"Finished: %lu cycles",(unsigned long)s->status.cycles);
            print_(36,text);
        } else print_(36,"UP/DOWN: choose mode");
        print_(46,s->has_run && !s->status.radio_released?"BT release failed":"A starts selected mode");
        print_(56,"A:START B:BACK");
    }
    poom_arduboy_display();
}
static void input_(const poom_sbus_msg_t *msg,void *user)
{
    (void)user;
    button_event_msg_t button;
    if(!msg || msg->len<sizeof(button))return;
    memcpy(&button,msg->data,sizeof(button));
    if(button.event!=BUTTON_SINGLE_CLICK)return;
    const uint8_t key=button.button;
    portENTER_CRITICAL(&input_lock);
    if(input_queue)(void)xQueueSend(input_queue,&key,0);
    portEXIT_CRITICAL(&input_lock);
}
bool menu_ble_spam_is_active(void)
{
    portENTER_CRITICAL(&input_lock);
    const bool active=session!=NULL;
    portEXIT_CRITICAL(&input_lock);
    return active;
}
void menu_ble_spam_stop(void)
{
    portENTER_CRITICAL(&input_lock);
    stopping=true;
    portEXIT_CRITICAL(&input_lock);
    while(menu_ble_spam_is_active())vTaskDelay(pdMS_TO_TICKS(10));
}
static esp_err_t release_previous_(void)
{
    const esp_err_t advertiser_error=poom_ble_spam_stop();
    if(advertiser_error!=ESP_OK)return advertiser_error;
    poom_ble_tracker_stop();
    esp_err_t error=poom_ble_detector_stop();
    if(error==ESP_OK)error=poom_ble_scan_stop();
    if(error==ESP_OK)error=poom_ble_gatt_client_stop();
    return error;
}
static bool key_(session_t *s,uint8_t key)
{
    if(key==KEY_B)return false;
    if(key==KEY_A) {
        if(s->status.active) {
            s->error=poom_ble_spam_stop();
            poom_ble_spam_get_status(&s->status);
        } else {
            s->error=release_previous_();
            if(s->error==ESP_OK)s->error=poom_ble_spam_start_mode(&s->config);
            s->awaiting_result=s->error==ESP_OK;
            s->has_run=true;
            poom_ble_spam_get_status(&s->status);
            if(s->awaiting_result && !s->status.active) {
                s->error=s->status.error;
                s->awaiting_result=false;
            }
        }
    } else if(!s->status.active) {
        if(key==KEY_UP || key==KEY_DOWN) {
            s->config.mode=(poom_ble_spam_mode_t)((s->config.mode+(key==KEY_DOWN?1:POOM_BLE_SPAM_MODE_COUNT-1))%POOM_BLE_SPAM_MODE_COUNT);
            s->has_run=false;s->error=ESP_OK;
        } else if(key==KEY_LEFT || key==KEY_RIGHT) {
            s->config.duration_ms=s->config.duration_ms==15000U?30000U:s->config.duration_ms==30000U?60000U:15000U;
        }
    }
    return true;
}
static void finish_(session_t *s)
{
    portENTER_CRITICAL(&input_lock);
    input_queue=NULL;
    portEXIT_CRITICAL(&input_lock);
    if(s->subscribed)(void)poom_sbus_unsubscribe_cb("input/button",input_,(void *)subscriber);
    poom_ble_spam_register_cb(NULL);
    poom_ble_spam_app_stop();
    if(s->input)vQueueDelete(s->input);
    portENTER_CRITICAL(&input_lock);
    session=NULL;
    portEXIT_CRITICAL(&input_lock);
    free(s);
    const uint8_t resume=1;
    (void)poom_sbus_publish("poom/menu/resume",&resume,sizeof(resume),0);
}
static void task_(void *arg)
{
    session_t *s=arg;
    /* Entering the menu is read-only. Only A requests advertising. */
    for(;;) {
        portENTER_CRITICAL(&input_lock);
        const bool stop=stopping;
        portEXIT_CRITICAL(&input_lock);
        if(stop)break;
        poom_ble_spam_get_status(&s->status);
        if(s->awaiting_result && !s->status.active) {
            s->error=s->status.error;
            s->awaiting_result=false;
        }
        uint8_t key;
        if(xQueueReceive(s->input,&key,pdMS_TO_TICKS(100))==pdTRUE && !key_(s,key))break;
        draw_(s);
    }
    finish_(s);
    vTaskDelete(NULL);
}
void menu_ble_spam_display(void)
{
    if(menu_ble_spam_is_active())return;
    session_t *s=calloc(1,sizeof(*s));
    if(!s) {
        const uint8_t resume=1;
        (void)poom_sbus_publish("poom/menu/resume",&resume,sizeof(resume),0);
        return;
    }
    s->config.duration_ms=30000U;
    portENTER_CRITICAL(&input_lock);
    session=s;stopping=false;
    portEXIT_CRITICAL(&input_lock);
    s->input=xQueueCreate(12,sizeof(uint8_t));
    if(!s->input){finish_(s);return;}
    portENTER_CRITICAL(&input_lock);
    input_queue=s->input;
    portEXIT_CRITICAL(&input_lock);
    s->subscribed=poom_sbus_subscribe_cb("input/button",input_,(void *)subscriber);
    if(!s->subscribed || xTaskCreate(task_,"ble_spam_ui",3072,s,4,NULL)!=pdPASS)finish_(s);
}
