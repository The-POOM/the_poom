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
enum {
    HEADER_H = 11,
    BOX_Y = 12,
    BOX_H = 41,
    TEXT_X = 4,
    LIST_ROWS = 4,
    LIST_ROW_Y0 = 14,
    LIST_ROW_STEP = 10,
    LIST_ROW_H = 9,
    SCAN_SETTLE_MS = 1500,
};
typedef enum { MENU, SCAN_ALL, SCAN_PICK, PICK, RUNNING, RESULT, EMPTY, ERROR } view_t;
typedef struct {
    QueueHandle_t input;
    bool subscribed;
    view_t view;
    size_t selected;
    size_t list_scroll;
    size_t count;
    size_t frozen_count;
    uint32_t scan_found_ms;
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
    poom_arduboy_set_cursor(TEXT_X, y);
    (void)poom_arduboy_print(clipped);
}
static void header_(void)
{
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("FIND MY BEEP"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}
static void footer_(const char *left, const char *right)
{
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(left);
    if(right != NULL) {
        poom_arduboy_set_cursor(82, 56);
        (void)poom_arduboy_print(right);
    }
}
static size_t list_total_(const session_t *s)
{
    size_t total = 0U;
    const uint32_t now = now_();
    for(size_t i = 0U; i < s->count; ++i)
        if(poom_findmy_eligible(&s->devices[i], now)) ++total;
    return total;
}
static const poom_findmy_device_t *list_device_(const session_t *s, size_t index)
{
    size_t current = 0U;
    const uint32_t now = now_();
    for(size_t i = 0U; i < s->count; ++i) {
        if(!poom_findmy_eligible(&s->devices[i], now)) continue;
        if(current++ == index) return &s->devices[i];
    }
    return NULL;
}
static void list_sync_scroll_(session_t *s)
{
    const size_t total = list_total_(s);
    if(s->selected >= total) s->selected = 0U;
    if(s->selected < s->list_scroll) s->list_scroll = s->selected;
    if(s->selected >= s->list_scroll + LIST_ROWS)
        s->list_scroll = s->selected - LIST_ROWS + 1U;
    if(s->list_scroll + LIST_ROWS > total)
        s->list_scroll = total > LIST_ROWS ? total - LIST_ROWS : 0U;
}
static void list_item_(const session_t *s, size_t index, char *out, size_t out_len)
{
    const poom_findmy_device_t *d = list_device_(s, index);
    if(d) (void)snprintf(out, out_len, "Find My tag %d dBm", d->rssi);
    else out[0] = '\0';
}
static size_t completed_count_(const poom_findmy_status_t *status)
{
    size_t completed = 0U;
    for(size_t i = 0U; i < status->count; ++i)
        if(status->results[i].write_acknowledged) ++completed;
    return completed;
}
static void draw_(session_t *s)
{
    char text[64];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    if(s->view == PICK) {
        poom_arduboy_set_cursor(31, 2);
        (void)poom_arduboy_print(F("SELECT TAGS"));
        poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
    } else {
        header_();
    }
    if(s->view == SCAN_ALL || s->view == SCAN_PICK) {
        poom_arduboy_set_cursor(22, 28);
        (void)poom_arduboy_print(F("Scanning..."));
        footer_(NULL, "B:BACK");
        poom_arduboy_display();
        return;
    }
    if(s->view == MENU || s->view == PICK) {
        poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
    }
    if(s->view == MENU) {
        static const char *const items[] = {"BEEP ALL", "SELECT TAGS"};
        for(size_t i = 0U; i < sizeof(items) / sizeof(items[0]); ++i) {
            const int16_t y = LIST_ROW_Y0 + (int16_t)(i * LIST_ROW_STEP);
            (void)snprintf(text, sizeof(text), "%s", items[i]);
            poom_arduboy_set_cursor(TEXT_X, y);
            (void)poom_arduboy_print(text);
            if(i == s->selected)
                poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, LIST_ROW_H, INVERT);
        }
        footer_("A:SELECT", "B:BACK");
    } else if(s->view == PICK) {
        list_sync_scroll_(s);
        for(size_t row = 0U; row < LIST_ROWS; ++row) {
            const size_t index = s->list_scroll + row;
            const int16_t y = LIST_ROW_Y0 + (int16_t)(row * LIST_ROW_STEP);
            if(index >= list_total_(s)) break;
            list_item_(s, index, text, sizeof(text));
            poom_arduboy_set_cursor(TEXT_X, y);
            (void)poom_arduboy_print(text);
            if(index == s->selected)
                poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, LIST_ROW_H, INVERT);
        }
        footer_("A:SELECT", "B:BACK");
    } else if(s->view == RUNNING) {
        line_(24, "Sending sound...");
        (void)snprintf(text, sizeof(text), "Tag %u / %u", (unsigned)(s->status.current + 1U),
                       (unsigned)s->status.count);
        line_(38, text);
        footer_("B:STOP", NULL);
    } else if(s->view == RESULT) {
        const size_t completed = completed_count_(&s->status);
        line_(24, completed ? "Sound command sent" : "No command sent");
        (void)snprintf(text, sizeof(text), "Tags: %u / %u", (unsigned)completed,
                       (unsigned)s->status.count);
        line_(38, text);
        footer_("A:MENU", "B:BACK");
    } else if(s->view == EMPTY) {
        line_(24, "No Find My tags found");
        line_(38, "Move closer and retry");
        footer_("A:RETRY", "B:BACK");
    } else {
        line_(24, "Sound command failed");
        line_(38, "Try again");
        footer_("A:MENU", "B:BACK");
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
static void scan_(session_t *s, view_t next_view)
{
    s->error = poom_findmy_beep_stop();
    if(s->error != ESP_OK) { s->view = ERROR; return; }
    s->selected = s->count = s->frozen_count = 0;
    s->list_scroll = 0U;
    s->error = poom_findmy_beep_scan();
    s->scan_found_ms = 0U;
    s->view = s->error == ESP_OK ? next_view : ERROR;
}
static bool key_(session_t *s, uint8_t key)
{
    if(key == KEY_B) {
        if(s->view == SCAN_ALL || s->view == SCAN_PICK) {
            s->error = poom_findmy_beep_stop();
            s->view = s->error == ESP_OK ? MENU : ERROR;
        } else if(s->view == PICK) {
            s->view = MENU;
            s->selected = 1U;
        }
        else if(s->view == RUNNING) {
            s->error = poom_findmy_beep_stop();
            poom_findmy_beep_status(&s->status);
            s->selected = 0;
            s->view = s->error == ESP_OK ? RESULT : ERROR;
        } else return false;
    } else if(key == KEY_A) {
        if(s->view == MENU) {
            scan_(s, s->selected == 0U ? SCAN_ALL : SCAN_PICK);
        } else if(s->view == PICK) {
            const poom_findmy_device_t *d = list_device_(s, s->selected);
            if(!d) return true;
            s->frozen[0] = d->key;
            s->frozen_count = 1U;
            s->error = poom_findmy_beep_begin(s->frozen, 1U);
            s->view = s->error == ESP_OK ? RUNNING : ERROR;
        } else if(s->view == EMPTY) scan_(s, SCAN_ALL);
        else if(s->view == RESULT || s->view == ERROR) s->view = MENU;
    } else if(key == KEY_UP || key == KEY_DOWN) {
        const size_t count = s->view == MENU ? 2U : s->view == PICK ? list_total_(s) : 0U;
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
    s->view = s->error == ESP_OK ? MENU : ERROR;
    for(;;) {
        portENTER_CRITICAL(&input_lock);
        const bool stop = stop_requested;
        portEXIT_CRITICAL(&input_lock);
        if(stop) break;
        poom_findmy_beep_status(&s->status);
        if(s->view == SCAN_ALL || s->view == SCAN_PICK || s->view == PICK) {
            s->count = poom_findmy_beep_devices(s->devices, POOM_FINDMY_MAX_DEVICES);
            if(s->view == PICK) list_sync_scroll_(s);
        }
        if((s->view == SCAN_ALL || s->view == SCAN_PICK) && !s->status.active) {
            s->error = s->status.error;
            s->view = ERROR;
        } else if((s->view == SCAN_ALL || s->view == SCAN_PICK) &&
                  s->status.phase == POOM_FINDMY_SCANNING && list_total_(s)) {
            if(!s->scan_found_ms) s->scan_found_ms = now_();
            if((uint32_t)(now_() - s->scan_found_ms) >= SCAN_SETTLE_MS) {
                if(s->view == SCAN_PICK) {
                    s->selected = s->list_scroll = 0U;
                    s->view = PICK;
                } else {
                    s->frozen_count = poom_findmy_freeze(s->devices, s->count, now_(),
                                                          s->frozen, POOM_FINDMY_MAX_DEVICES);
                    if(s->frozen_count) {
                        s->error = poom_findmy_beep_begin(s->frozen, s->frozen_count);
                        s->view = s->error == ESP_OK ? RUNNING : ERROR;
                    } else {
                        s->view = EMPTY;
                    }
                }
            }
        } else if(s->view == SCAN_ALL || s->view == SCAN_PICK) {
            s->scan_found_ms = 0U;
        }
        if(s->view == RUNNING && !s->status.active) {
            s->selected = 0;
            s->view = s->status.error == ESP_OK ? RESULT : ERROR;
            s->error = s->status.error;
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
