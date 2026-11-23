// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../applications/poom_app_pack/src/menu_ble_spam.c"
static unsigned checks,start_calls,stop_calls,queue_sends,resumes;
static bool subscription_ok=true,task_ok=true,immediate_failure;
static poom_ble_spam_status_t backend={.radio_released=true};
static poom_ble_spam_config_t started;
static char drawn[8192];
#define CHECK(x) do {assert(x);++checks;} while(0)
typedef struct {uint8_t keys[12];unsigned read,used;} queue_t;
QueueHandle_t xQueueCreate(unsigned count,size_t size){CHECK(count==12 && size==1);return calloc(1,sizeof(queue_t));}
int xQueueSend(QueueHandle_t queue,const void *item,unsigned ticks){(void)ticks;queue_t*q=queue;CHECK(q->used<12);q->keys[(q->read+q->used)%12]=*(const uint8_t*)item;++q->used;++queue_sends;return 1;}
int xQueueReceive(QueueHandle_t queue,void *item,unsigned ticks){(void)ticks;queue_t*q=queue;CHECK(q->used>0);*(uint8_t*)item=q->keys[q->read];q->read=(q->read+1)%12;--q->used;return 1;}
void vQueueDelete(QueueHandle_t queue){free(queue);}
int xTaskCreate(void(*task)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{(void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return task_ok;}
void vTaskDelay(unsigned ticks){(void)ticks;}
void vTaskDelete(void *task){(void)task;}
void poom_arduboy_clear(void){}
void poom_arduboy_set_text_size(int size){(void)size;}
void poom_arduboy_set_cursor(int x,int y){(void)x;(void)y;}
int poom_arduboy_print(const char *text){if(strlen(drawn)+strlen(text)+2<sizeof(drawn)){strcat(drawn,text);strcat(drawn,"\n");}return 0;}
void poom_arduboy_fill_rect(int x,int y,int width,int height,int color){(void)x;(void)y;(void)width;(void)height;(void)color;}
void poom_arduboy_display(void){}
bool poom_sbus_subscribe_cb(const char *topic,void(*callback)(const poom_sbus_msg_t*,void*),void *user){(void)topic;(void)callback;(void)user;return subscription_ok;}
bool poom_sbus_unsubscribe_cb(const char *topic,void(*callback)(const poom_sbus_msg_t*,void*),void *user){(void)topic;(void)callback;(void)user;return true;}
bool poom_sbus_publish(const char *topic,const void *data,size_t length,unsigned flags){(void)data;(void)length;(void)flags;CHECK(!strcmp(topic,"poom/menu/resume"));++resumes;return true;}
void poom_ble_tracker_stop(void){}
esp_err_t poom_ble_detector_stop(void){return ESP_OK;}
esp_err_t poom_ble_scan_stop(void){return ESP_OK;}
esp_err_t poom_ble_gatt_client_stop(void){return ESP_OK;}
const char *esp_err_to_name(esp_err_t error){return error==ESP_OK?"ESP_OK":"ESP_FAIL";}
void poom_ble_spam_register_cb(poom_ble_spam_cb_display callback){(void)callback;}
esp_err_t poom_ble_spam_stop(void){++stop_calls;backend.active=false;backend.advertising=false;backend.radio_released=true;return ESP_OK;}
void poom_ble_spam_app_stop(void){(void)poom_ble_spam_stop();}
void poom_ble_spam_get_status(poom_ble_spam_status_t *out){*out=backend;}
esp_err_t poom_ble_spam_start_mode(const poom_ble_spam_config_t *config)
{++start_calls;started=*config;backend.mode=config->mode;backend.duration_ms=config->duration_ms;backend.active=!immediate_failure;backend.error=immediate_failure?ESP_FAIL:ESP_OK;return ESP_OK;}
static void press_(uint8_t key){const button_event_msg_t button={.button=key,.event=BUTTON_SINGLE_CLICK};const poom_sbus_msg_t msg={.data=&button,.len=sizeof(button)};input_(&msg,NULL);}
int main(void)
{
    menu_ble_spam_display();CHECK(menu_ble_spam_is_active() && start_calls==0);
    press_(KEY_B);task_(session);CHECK(!menu_ble_spam_is_active() && resumes==1 && start_calls==0);
    const unsigned sent=queue_sends;press_(KEY_A);CHECK(queue_sends==sent);
    immediate_failure=true;drawn[0]=0;
    menu_ble_spam_display();press_(KEY_UP);press_(KEY_RIGHT);press_(KEY_A);press_(KEY_B);task_(session);
    CHECK(start_calls==1 && started.mode==POOM_BLE_SPAM_ALL && started.duration_ms==60000);
    CHECK(strstr(drawn,"ESP_FAIL") && !strstr(drawn,"Finished: 0 cycles"));
    immediate_failure=false;
    menu_ble_spam_display();press_(KEY_DOWN);press_(KEY_A);press_(KEY_A);press_(KEY_B);task_(session);
    CHECK(start_calls==2 && started.mode==POOM_BLE_SPAM_APPLE_JUICE && !backend.active && stop_calls>=4);
    subscription_ok=false;menu_ble_spam_display();CHECK(!menu_ble_spam_is_active());subscription_ok=true;
    task_ok=false;menu_ble_spam_display();CHECK(!menu_ble_spam_is_active());task_ok=true;
    menu_ble_spam_stop();CHECK(!menu_ble_spam_is_active());
    printf("BLE advertising UI: %u checks passed\n",checks);
    return 0;
}
