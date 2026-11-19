// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "menu_ssid_spam.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "poom_sbus.h"
#include "poom_wifi_spam.h"
#include "poom_wifi_ctrl.h"

enum { KEY_A, KEY_B, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN };
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_events;
static bool s_active;
static char s_user[] = "menu_ssid_spam";
static const uint8_t s_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,
#if CONFIG_IDF_TARGET_ESP32C5
    36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,
#endif
};
static void resume_(void)
{
    const uint8_t token = 1;
    (void)poom_sbus_publish("poom/menu/resume", &token, sizeof(token), 0);
}
static void button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    button_event_msg_t event;
    if(!msg || msg->len < sizeof(event)) return;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    const uint8_t key = event.button;
    portENTER_CRITICAL(&s_lock);
    if(s_events) (void)xQueueSend(s_events, &key, 0);
    portEXIT_CRITICAL(&s_lock);
}
static void line_(int y, const char *text)
{
    poom_arduboy_set_cursor(0, y);
    (void)poom_arduboy_print(text);
}
static void draw_(poom_wifi_spam_mode_t mode, uint8_t channel,
                  const poom_wifi_spam_status_t *status, esp_err_t error)
{
    char text[32];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    line_(1, "WIFI SPAM");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 10, INVERT);
    (void)snprintf(text, sizeof(text), "UD: %.17s", poom_wifi_spam_mode_label(mode));
    line_(13, text);
    (void)snprintf(text, sizeof(text), "LR: Ch %u  30 seconds", (unsigned)channel);
    line_(23, text);
    if(error != ESP_OK) (void)snprintf(text, sizeof(text), "%.21s", esp_err_to_name(error));
    else (void)snprintf(text, sizeof(text), "Sent %lu Err %lu", (unsigned long)status->sent, (unsigned long)status->errors);
    line_(33, text);
    (void)snprintf(text, sizeof(text), "%s %lus / 30s", status->running ? "RUN" : "READY", (unsigned long)(status->elapsed_ms / 1000U));
    line_(43, text);
    line_(56, status->running ? "A:STOP       B:BACK" : "A:START      B:BACK");
    poom_arduboy_display();
}
static void worker_(void *arg)
{
    QueueHandle_t events = arg;
    poom_wifi_spam_mode_t mode = POOM_WIFI_SPAM_SAVED;
    size_t channel_index = 0;
    esp_err_t error = ESP_OK;
    poom_wifi_spam_status_t status = {0};
    for(;;) {
        poom_wifi_spam_get_status(&status);
        if(status.last_error != ESP_OK) error = status.last_error;
        draw_(mode, s_channels[channel_index], &status, error);
        uint8_t key;
        if(xQueueReceive(events, &key, pdMS_TO_TICKS(250)) != pdTRUE) continue;
        if(key == KEY_B) break;
        if(key == KEY_A) {
            if(status.running) error = poom_wifi_spam_stop();
            else error = poom_wifi_spam_start_mode(mode, s_channels[channel_index], 30000U);
        } else if(!status.running) {
            if(key == KEY_UP || key == KEY_DOWN) {
                mode = (poom_wifi_spam_mode_t)((mode + (key == KEY_DOWN ? 1 : POOM_WIFI_SPAM_MODE_COUNT - 1)) % POOM_WIFI_SPAM_MODE_COUNT);
                error = ESP_OK;
            } else if(key == KEY_LEFT || key == KEY_RIGHT) {
                const size_t count = sizeof(s_channels) / sizeof(s_channels[0]);
                for(size_t i = 0; i < count; ++i) {
                    channel_index = (channel_index + (key == KEY_RIGHT ? 1 : count - 1)) % count;
                    if(poom_wifi_ctrl_channel_is_valid(s_channels[channel_index])) break;
                }
                error = ESP_OK;
            }
        }
    }
    (void)poom_wifi_spam_stop();
    portENTER_CRITICAL(&s_lock);
    s_events = NULL;
    portEXIT_CRITICAL(&s_lock);
    (void)poom_sbus_unsubscribe_cb("input/button", button_, s_user);
    vQueueDelete(events);
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
    resume_();
    vTaskDelete(NULL);
}
void menu_ssid_spam_init(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_active;
    if(!active) s_active = true;
    portEXIT_CRITICAL(&s_lock);
    if(active) return;
    QueueHandle_t events = xQueueCreate(12, sizeof(uint8_t));
    bool subscribed = false;
    if(events) {
        portENTER_CRITICAL(&s_lock);
        s_events = events;
        portEXIT_CRITICAL(&s_lock);
        subscribed = poom_sbus_subscribe_cb("input/button", button_, s_user);
        if(subscribed && xTaskCreate(worker_, "wifi_spam_ui", 4096, events, 4, NULL) == pdPASS) return;
    }
    portENTER_CRITICAL(&s_lock);
    s_events = NULL;
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
    if(subscribed) (void)poom_sbus_unsubscribe_cb("input/button", button_, s_user);
    if(events) vQueueDelete(events);
    resume_();
}
