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
enum { HEADER_H = 11, BOX_Y = 12, BOX_H = 41, ROW_Y0 = 14, ROW_STEP = 10, ROW_H = 9 };
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
static void line_(int x, int y, const char *text)
{
    poom_arduboy_set_cursor(x, y);
    (void)poom_arduboy_print(text);
}
static void footer_(const char *left, const char *middle)
{
    line_(0, 56, left);
    if(middle) line_(44, 56, middle);
    line_(88, 56, "B:BACK");
}
static void option_(int y, const char *text, bool selected)
{
    line_(4, y, text);
    if(selected) poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, ROW_H, INVERT);
}
static void draw_(poom_wifi_spam_mode_t mode, uint8_t channel, uint8_t selected,
                  const poom_wifi_spam_status_t *status, esp_err_t error)
{
    char text[32];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    line_(34, 2, "WIFI SPAM");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
    if(status->running) {
        (void)snprintf(text, sizeof(text), "%.18s", poom_wifi_spam_mode_label(mode));
        line_(4, 14, text);
        (void)snprintf(text, sizeof(text), "Sent: %lu", (unsigned long)status->sent);
        line_(4, 25, text);
        (void)snprintf(text, sizeof(text), "Time: %lu / 30s", (unsigned long)(status->elapsed_ms / 1000U));
        line_(4, 36, text);
        footer_("A:STOP", NULL);
    } else {
        (void)snprintf(text, sizeof(text), "TYPE: %.14s", poom_wifi_spam_mode_label(mode));
        option_(ROW_Y0, text, selected == 0U);
        (void)snprintf(text, sizeof(text), "CHANNEL: %u", (unsigned)channel);
        option_(ROW_Y0 + ROW_STEP, text, selected == 1U);
        if(error != ESP_OK) line_(4, 44, "Start failed");
        footer_("A:RUN", "L/R:CH");
    }
    poom_arduboy_display();
}
static void worker_(void *arg)
{
    QueueHandle_t events = arg;
    poom_wifi_spam_mode_t mode = POOM_WIFI_SPAM_SAVED;
    size_t channel_index = 0;
    uint8_t selected = 0U;
    esp_err_t error = ESP_OK;
    poom_wifi_spam_status_t status = {0};
    for(;;) {
        poom_wifi_spam_get_status(&status);
        if(status.last_error != ESP_OK) error = status.last_error;
        draw_(mode, s_channels[channel_index], selected, &status, error);
        uint8_t key;
        if(xQueueReceive(events, &key, pdMS_TO_TICKS(250)) != pdTRUE) continue;
        if(key == KEY_B) break;
        if(key == KEY_A) {
            if(status.running) error = poom_wifi_spam_stop();
            else error = poom_wifi_spam_start_mode(mode, s_channels[channel_index], 30000U);
        } else if(!status.running) {
            if(key == KEY_UP || key == KEY_DOWN) {
                selected ^= 1U;
                error = ESP_OK;
            } else if(key == KEY_LEFT || key == KEY_RIGHT) {
                if(selected == 0U) {
                    mode = (poom_wifi_spam_mode_t)((mode +
                        (key == KEY_RIGHT ? 1U : POOM_WIFI_SPAM_MODE_COUNT - 1U)) % POOM_WIFI_SPAM_MODE_COUNT);
                } else {
                    const size_t count = sizeof(s_channels) / sizeof(s_channels[0]);
                    for(size_t i = 0; i < count; ++i) {
                        channel_index = (channel_index + (key == KEY_RIGHT ? 1U : count - 1U)) % count;
                        if(poom_wifi_ctrl_channel_is_valid(s_channels[channel_index])) break;
                    }
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
