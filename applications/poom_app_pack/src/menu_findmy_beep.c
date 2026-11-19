// SPDX-License-Identifier: MIT
// Action-first Find My sound requests. A write acknowledgement is not audible proof.
#include "menu_findmy_beep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "poom_ble_detector.h"
#include "poom_ble_gatt_client.h"
#include "poom_ble_scan.h"
#include "poom_ble_spam.h"
#include "poom_ble_tracker.h"
#include "poom_findmy_beep.h"
#include "poom_sbus.h"

enum { KEY_A, KEY_B, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN };
typedef enum { LIST, READY, RUNNING, RESULT, ERROR } view_t;
typedef struct {
    QueueHandle_t input;
    bool subscribed;
    view_t view;
    size_t selected;
    size_t count;
    size_t frozen_count;
    poom_findmy_device_t devices[POOM_FINDMY_MAX_DEVICES];
    poom_findmy_key_t frozen[POOM_FINDMY_MAX_DEVICES];
    poom_findmy_status_t status;
    esp_err_t error;
} session_t;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t input_queue;
static session_t *session;
static bool stop_requested;
static const char subscriber[] = "menu_findmy_beep";

static uint32_t now_(void) { return (uint32_t)(esp_timer_get_time() / 1000LL); }
static void line_(int y, const char *text)
{
    char clipped[22];
    (void)snprintf(clipped, sizeof(clipped), "%.21s", text);
    poom_arduboy_set_cursor(1, y);
    (void)poom_arduboy_print(clipped);
}
static void address_(char *out, size_t length, const poom_findmy_key_t *key)
{
    (void)snprintf(out, length, "%02X:%02X:%02X:%02X:%02X:%02X", key->mac[0], key->mac[1],
                   key->mac[2], key->mac[3], key->mac[4], key->mac[5]);
}
static const char *candidate_(uint8_t flags)
{
    if(flags & POOM_FINDMY_APPLE) return "Find My";
    if(flags & POOM_FINDMY_FMNA) return "FMNA";
    return "DULT";
}
static void draw_(session_t *s)
{
    char text[64];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    line_(1, "FIND MY BEEP");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 10, INVERT);
    if(s->view == LIST) {
        size_t eligible = 0;
        for(size_t i = 0; i < s->count; ++i) if(poom_findmy_eligible(&s->devices[i], now_())) ++eligible;
        if(!s->selected) {
            (void)snprintf(text, sizeof(text), "> BEEP ALL (%u)", (unsigned)eligible);
            line_(15, text);
            (void)snprintf(text, sizeof(text), "%u candidates / %u ready", (unsigned)s->count, (unsigned)eligible);
            line_(27, text);
            line_(39, s->status.phase == POOM_FINDMY_STARTING ? "Starting Bluetooth..." :
                      !s->count ? "Scanning for tags..." : "DOWN: choose one tag");
        } else {
            const poom_findmy_device_t *d = &s->devices[s->selected - 1];
            address_(text, sizeof(text), &d->key);
            line_(13, text);
            (void)snprintf(text, sizeof(text), "%s %ddB type%u", candidate_(d->candidates), d->rssi, d->key.address_type);
            line_(24, text);
            line_(35, d->name[0] ? d->name : "Unnamed candidate");
            line_(45, poom_findmy_eligible(d, now_()) ? "A: choose this tag" :
                       !d->connectable ? "Not connectable now" : "Stale - waiting...");
        }
        line_(56, "UP/DN:PICK A:OK B:BACK");
    } else if(s->view == READY) {
        if(s->frozen_count == 1) {
            address_(text, sizeof(text), &s->frozen[0]);
            line_(14, text);
        } else {
            (void)snprintf(text, sizeof(text), "BEEP ALL: %u tags", (unsigned)s->frozen_count);
            line_(14, text);
        }
        line_(27, "One request per tag");
        line_(39, "Current selection only");
        line_(56, "A:BEEP B:BACK");
    } else if(s->view == RUNNING) {
        static const char *const phases[] = {"Starting Bluetooth", "Scanning", "Connecting", "Discovering services",
            "Enabling replies", "Writing sound request", "Waiting for reply", "Finished"};
        (void)snprintf(text, sizeof(text), "Tag %u / %u", (unsigned)(s->status.current + 1), (unsigned)s->status.count);
        line_(14, text);
        if(s->status.count) {
            address_(text, sizeof(text), &s->status.results[s->status.current].key);
            line_(25, text);
        }
        line_(38, phases[s->status.phase]);
        line_(56, "B:CANCEL ALL");
    } else if(s->view == RESULT) {
        if(s->status.count) {
            const poom_findmy_result_t *r = &s->status.results[s->selected];
            address_(text, sizeof(text), &r->key);
            line_(13, text);
            (void)snprintf(text, sizeof(text), "%u/%u %s", (unsigned)(s->selected + 1), (unsigned)s->status.count,
                           poom_findmy_result_name(r->code));
            line_(24, text);
            if(r->reply == POOM_FINDMY_REPLY_REJECTED) (void)snprintf(text, sizeof(text), "Reply rejected: %u", r->response_status);
            else if(r->att_status) (void)snprintf(text, sizeof(text), "ATT status: 0x%02X", r->att_status);
            else if(r->error) (void)snprintf(text, sizeof(text), "Error: 0x%X", (unsigned)r->error);
            else if(r->reply == POOM_FINDMY_REPLY_COMPLETED) (void)snprintf(text, sizeof(text), "Completion reported");
            else if(r->reply == POOM_FINDMY_REPLY_ACCEPTED) (void)snprintf(text, sizeof(text), "Request accepted");
            else if(r->reply == POOM_FINDMY_REPLY_RECEIVED) (void)snprintf(text, sizeof(text), "Reply received");
            else (void)snprintf(text, sizeof(text), "%s", r->write_acknowledged ?
                    (r->protocol == POOM_FINDMY_AIRTAG ? "Write accepted" : "No reply within 3s") : "No sound ACK");
            line_(35, text);
            line_(45, r->command_submitted && r->code == POOM_FINDMY_CANCELLED ? "Tag may still sound" :
                    r->write_acknowledged ? "Sound not verified" : "UP/DOWN: results");
        } else line_(24, "No sound requested");
        line_(56, "A:SCAN UP/DN B:BACK");
    } else {
        const bool release_failed = s->status.phase == POOM_FINDMY_FINISHED && !s->status.radio_released;
        line_(17, release_failed ? "BT teardown failed" : "Request unavailable");
        line_(29, esp_err_to_name(s->error));
        line_(41, release_failed ? "Restart device" : "No automatic retry");
        line_(56, "A:SCAN B:BACK");
    }
    poom_arduboy_display();
}
static void input_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    button_event_msg_t button;
    if(!msg || msg->len < sizeof(button)) return;
    memcpy(&button, msg->data, sizeof(button));
    if(button.event != BUTTON_SINGLE_CLICK) return;
    const uint8_t key = button.button;
    portENTER_CRITICAL(&input_lock);
    if(input_queue) (void)xQueueSend(input_queue, &key, 0);
    portEXIT_CRITICAL(&input_lock);
}
bool menu_findmy_beep_is_active(void)
{
    portENTER_CRITICAL(&input_lock);
    const bool active = session != NULL;
    portEXIT_CRITICAL(&input_lock);
    return active;
}
void menu_findmy_beep_stop(void)
{
    portENTER_CRITICAL(&input_lock);
    stop_requested = true;
    portEXIT_CRITICAL(&input_lock);
    while(menu_findmy_beep_is_active()) vTaskDelay(pdMS_TO_TICKS(10));
}
static void scan_(session_t *s)
{
    s->error = poom_findmy_beep_stop();
    if(s->error != ESP_OK) { s->view = ERROR; return; }
    s->selected = s->count = s->frozen_count = 0;
    s->error = poom_findmy_beep_scan();
    s->view = s->error == ESP_OK ? LIST : ERROR;
}
static bool key_(session_t *s, uint8_t key)
{
    if(key == KEY_B) {
        if(s->view == READY) s->view = LIST;
        else if(s->view == RUNNING) {
            s->error = poom_findmy_beep_stop();
            poom_findmy_beep_status(&s->status);
            s->selected = 0;
            s->view = s->error == ESP_OK ? RESULT : ERROR;
        } else return false;
    } else if(key == KEY_A) {
        if(s->view == LIST) {
            if(s->selected) {
                const poom_findmy_device_t *d = &s->devices[s->selected - 1];
                if(!poom_findmy_eligible(d, now_())) return true;
                s->frozen[0] = d->key;
                s->frozen_count = 1;
            } else s->frozen_count = poom_findmy_freeze(s->devices, s->count, now_(), s->frozen, POOM_FINDMY_MAX_DEVICES);
            if(s->frozen_count) s->view = READY;
        } else if(s->view == READY) {
            s->error = poom_findmy_beep_begin(s->frozen, s->frozen_count);
            s->view = s->error == ESP_OK ? RUNNING : ERROR;
        } else if(s->view == RESULT || s->view == ERROR) scan_(s);
    } else if(key == KEY_UP || key == KEY_DOWN) {
        const size_t count = s->view == LIST ? s->count + 1 : s->view == RESULT ? s->status.count : 0;
        if(count) s->selected = (s->selected + (key == KEY_DOWN ? 1 : count - 1)) % count;
    }
    return true;
}
static void finish_(session_t *s)
{
    portENTER_CRITICAL(&input_lock);
    input_queue = NULL;
    portEXIT_CRITICAL(&input_lock);
    if(s->subscribed) (void)poom_sbus_unsubscribe_cb("input/button", input_, (void *)subscriber);
    poom_findmy_beep_stop();
    if(s->input) vQueueDelete(s->input);
    portENTER_CRITICAL(&input_lock);
    session = NULL;
    portEXIT_CRITICAL(&input_lock);
    free(s);
    const uint8_t resume = 1;
    (void)poom_sbus_publish("poom/menu/resume", &resume, sizeof(resume), 0);
}
static void task_(void *arg)
{
    session_t *s = arg;
    /* Reset existing owners through their own APIs, before our native callbacks
     * replace the scanner callback. Their start/initialized flags remain honest. */
    poom_ble_spam_app_stop();
    poom_ble_tracker_stop();
    s->error = poom_ble_detector_stop();
    if(s->error == ESP_OK) s->error = poom_ble_scan_stop();
    if(s->error == ESP_OK) s->error = poom_ble_gatt_client_stop();
    poom_ble_gatt_client_set_remote_bda(NULL);
    poom_ble_gatt_client_set_remote_device_name(NULL);
    poom_ble_gatt_client_set_recon_mode(false);
    poom_ble_gatt_client_set_bonding(false);
    const poom_ble_gatt_client_event_cb_t empty_callbacks = {0};
    poom_ble_gatt_client_set_callbacks(empty_callbacks);
    if(s->error == ESP_OK) scan_(s); else s->view = ERROR;
    for(;;) {
        portENTER_CRITICAL(&input_lock);
        const bool stop = stop_requested;
        portEXIT_CRITICAL(&input_lock);
        if(stop) break;
        poom_findmy_beep_status(&s->status);
        if(s->view == LIST) {
            s->count = poom_findmy_beep_devices(s->devices, POOM_FINDMY_MAX_DEVICES);
            if(s->selected > s->count) s->selected = 0;
        }
        if(s->view == RUNNING && !s->status.active) {
            s->selected = 0;
            s->view = s->status.error == ESP_OK ? RESULT : ERROR;
            s->error = s->status.error;
        } else if((s->view == LIST || s->view == READY) && !s->status.active) {
            s->error = s->status.error;
            s->view = ERROR;
        }
        uint8_t key;
        if(xQueueReceive(s->input, &key, pdMS_TO_TICKS(100)) == pdTRUE && !key_(s, key)) break;
        draw_(s);
    }
    finish_(s);
    vTaskDelete(NULL);
}
void menu_findmy_beep_show(void)
{
    if(menu_findmy_beep_is_active()) return;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s) s = calloc(1, sizeof(*s));
    if(!s) {
        const uint8_t resume = 1;
        (void)poom_sbus_publish("poom/menu/resume", &resume, sizeof(resume), 0);
        return;
    }
    portENTER_CRITICAL(&input_lock);
    session = s;
    stop_requested = false;
    portEXIT_CRITICAL(&input_lock);
    s->input = xQueueCreate(12, sizeof(uint8_t));
    if(!s->input) { finish_(s); return; }
    portENTER_CRITICAL(&input_lock);
    input_queue = s->input;
    portEXIT_CRITICAL(&input_lock);
    s->subscribed = poom_sbus_subscribe_cb("input/button", input_, (void *)subscriber);
    if(!s->subscribed || xTaskCreate(task_, "findmy_ui", 4096, s, 4, NULL) != pdPASS) finish_(s);
}
