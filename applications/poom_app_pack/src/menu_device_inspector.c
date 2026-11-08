// SPDX-License-Identifier: MIT
/* Passive, time-sliced IoT observation summary.  No SD or PCAP output. */
#include "menu_device_inspector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_ble_scan.h"
#include "poom_sbus.h"
#include "poom_scanner_core.h"
#include "poom_wifi_detector.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define IOT_TICK_MS            (150U)
/* The C5 Wi-Fi monitor needs about 5.3 seconds for one full 2.4/5 GHz
 * channel round.  A 3-second slice stopped it mid-sweep, while BLE devices
 * with a slow advertising interval often had not announced yet. */
#define IOT_PHASE_MS           (12000U)
#define IOT_STACK              (3328U)
#define IOT_PRIORITY           (4U)
#define IOT_MAX_DEVICES        (24U)
#define IOT_WIFI_WINDOW        (12U)

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
#endif
#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum {
    IOT_PHASE_WIFI = 0,
    IOT_PHASE_BLE,
    IOT_PHASE_154,
} iot_phase_t;

typedef struct { uint8_t button; } iot_button_t;

typedef struct {
    uint8_t wifi_macs[IOT_MAX_DEVICES][6];
    uint8_t ble_macs[IOT_MAX_DEVICES][6];
    poom_wifi_detector_record_t wifi_records[IOT_WIFI_WINDOW];
    poom_wifi_detector_record_t *wifi_ptrs[IOT_WIFI_WINDOW];
    uint8_t wifi_count;
    uint8_t ble_count;
    uint8_t ieee154_active_channels;
    iot_phase_t phase;
    int64_t phase_started_us;
    bool active;
    bool paused;
    bool radio_running;
    bool subscribed;
    QueueHandle_t buttons;
    TaskHandle_t task;
    portMUX_TYPE lock;
} iot_survey_state_t;

/* Keep the sole persistent app pointer out of HP SRAM.  The session itself is
 * allocated in PSRAM only while the survey is open. */
EXT_RAM_BSS_ATTR static iot_survey_state_t *s_iot = NULL;
static const char s_iot_user[] = "menu_device_inspector";

static void iot_exit_(void);
static void iot_button_cb_(const poom_sbus_msg_t *msg, void *user);

static bool iot_add_mac_(uint8_t macs[IOT_MAX_DEVICES][6], uint8_t *count,
                         const uint8_t mac[6])
{
    if(macs == NULL || count == NULL || mac == NULL || (mac[0] & 0x01U) != 0U)
    {
        return false;
    }
    for(uint8_t i = 0U; i < *count; ++i)
    {
        if(memcmp(macs[i], mac, 6U) == 0) return false;
    }
    if(*count >= IOT_MAX_DEVICES) return false;
    memcpy(macs[*count], mac, 6U);
    ++*count;
    return true;
}

static void iot_ble_cb_(const esp_ble_gap_cb_param_t *result)
{
    if(s_iot == NULL || !s_iot->active || s_iot->phase != IOT_PHASE_BLE ||
       result == NULL)
    {
        return;
    }
    portENTER_CRITICAL(&s_iot->lock);
    (void)iot_add_mac_(s_iot->ble_macs, &s_iot->ble_count, result->scan_rst.bda);
    portEXIT_CRITICAL(&s_iot->lock);
}

static void iot_finish_wifi_(void)
{
    size_t count = 0U;
    if(s_iot != NULL && s_iot->radio_running)
    {
        if(poom_wifi_detector_monitor_snapshot(s_iot->wifi_ptrs, IOT_WIFI_WINDOW, &count) == ESP_OK)
        {
            for(size_t i = 0U; i < count; ++i)
            {
                (void)iot_add_mac_(s_iot->wifi_macs, &s_iot->wifi_count,
                                   s_iot->wifi_records[i].bssid);
            }
        }
        (void)poom_wifi_detector_monitor_stop();
    }
}

static void iot_finish_154_(void)
{
    poom_scanner_core_ieee802154_stats_t stats;
    if(s_iot == NULL || !s_iot->radio_running) return;
    if(poom_scanner_core_get_ieee802154_stats(&stats))
    {
        uint8_t active = 0U;
        for(uint8_t i = 0U; i < POOM_SCANNER_CORE_IEEE802154_CH_COUNT; ++i)
        {
            if(stats.packet_count[i] != 0U) ++active;
        }
        s_iot->ieee154_active_channels = active;
    }
    (void)poom_scanner_core_stop();
}

static void iot_stop_phase_(void)
{
    if(s_iot == NULL || !s_iot->radio_running) return;
    if(s_iot->phase == IOT_PHASE_WIFI)
    {
        iot_finish_wifi_();
    }
    else if(s_iot->phase == IOT_PHASE_BLE)
    {
        poom_ble_scan_register_cb(NULL);
        (void)poom_ble_scan_stop();
    }
    else
    {
        iot_finish_154_();
    }
    s_iot->radio_running = false;
}

static void iot_start_phase_(void)
{
    esp_err_t err = ESP_FAIL;
    if(s_iot == NULL || !s_iot->active || s_iot->paused) return;
    if(s_iot->phase == IOT_PHASE_WIFI)
    {
        err = poom_wifi_detector_monitor_start(POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS,
                                               NULL, 0U, IOT_WIFI_WINDOW);
    }
    else if(s_iot->phase == IOT_PHASE_BLE)
    {
        poom_ble_scan_set_uart_forward_enabled(false);
        poom_ble_scan_set_scan_type(BLE_SCAN_TYPE_PASSIVE);
        poom_ble_scan_set_filter_type(BLE_SCAN_FILTER_ALLOW_ALL);
        poom_ble_scan_register_cb(iot_ble_cb_);
        err = poom_ble_scan_start();
        if(err != ESP_OK) poom_ble_scan_register_cb(NULL);
    }
    else
    {
        (void)poom_scanner_core_stop();
        err = poom_scanner_core_start_ieee802154(180U);
    }
    s_iot->radio_running = (err == ESP_OK);
    s_iot->phase_started_us = esp_timer_get_time();
}

static void iot_draw_(void)
{
    uint8_t wifi = 0U;
    uint8_t ble = 0U;
    uint8_t ieee154 = 0U;
    iot_phase_t phase;
    bool scanning;
    bool blink;
    char line[22];
    if(s_iot == NULL) return;
    portENTER_CRITICAL(&s_iot->lock);
    wifi = s_iot->wifi_count;
    ble = s_iot->ble_count;
    ieee154 = s_iot->ieee154_active_channels;
    phase = s_iot->phase;
    scanning = s_iot->radio_running && !s_iot->paused;
    portEXIT_CRITICAL(&s_iot->lock);
    blink = ((esp_timer_get_time() / 350000LL) & 1LL) == 0LL;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    /* The menu keeps the complete name; the OLED header uses the compact
     * form so it remains inside the 128-pixel inverted title bar. */
    poom_arduboy_set_cursor(31, 2);
    (void)poom_arduboy_print(F("DEV INSPECT"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    (void)snprintf(line, sizeof(line), "WIFI AP: %u", (unsigned)wifi);
    poom_arduboy_set_cursor(3, 16);
    (void)poom_arduboy_print(line);
    (void)snprintf(line, sizeof(line), "BLE DEV: %u", (unsigned)ble);
    poom_arduboy_set_cursor(3, 29);
    (void)poom_arduboy_print(line);
    (void)snprintf(line, sizeof(line), "154 ACT: %u CH", (unsigned)ieee154);
    poom_arduboy_set_cursor(3, 42);
    (void)poom_arduboy_print(line);

    /* The outlined marker identifies the radio being visited; its fill
     * blinks while that receiver is live. */
    if(scanning)
    {
        const int16_t marker_y = (phase == IOT_PHASE_WIFI) ? 19 :
                                 (phase == IOT_PHASE_BLE) ? 32 : 45;
        poom_arduboy_draw_circle(117, marker_y, 3, WHITE);
        if(blink) poom_arduboy_fill_rect(116, marker_y - 1, 3, 3, WHITE);
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(s_iot->paused ? F("A:RESUME") : F("A:PAUSE"));
    poom_arduboy_set_cursor(82, 56);
    (void)poom_arduboy_print(F("B:STOP"));
    poom_arduboy_display();
}

static void iot_exit_(void)
{
    iot_survey_state_t *state = s_iot;
    if(state == NULL) return;
    state->active = false;
    iot_stop_phase_();
    if(state->subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", iot_button_cb_, (void *)s_iot_user);
    }
    if(state->buttons != NULL) vQueueDelete(state->buttons);
    s_iot = NULL;
    free(state);
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static void iot_button_cb_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t event;
    iot_button_t button;
    (void)user;
    if(s_iot == NULL || msg == NULL || msg->len < sizeof(event) || s_iot->buttons == NULL) return;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    button.button = event.button;
    (void)xQueueSend(s_iot->buttons, &button, 0U);
}

static void iot_task_(void *arg)
{
    (void)arg;
    while(s_iot != NULL && s_iot->active)
    {
        iot_button_t button;
        if(s_iot->buttons != NULL && xQueueReceive(s_iot->buttons, &button,
                                                    pdMS_TO_TICKS(IOT_TICK_MS)) == pdTRUE)
        {
            if(button.button == BTN_B)
            {
                iot_exit_();
                continue;
            }
            if(button.button == BTN_A)
            {
                s_iot->paused = !s_iot->paused;
                if(s_iot->paused) iot_stop_phase_();
                else iot_start_phase_();
            }
        }
        if(s_iot != NULL && s_iot->active && !s_iot->paused &&
           (esp_timer_get_time() - s_iot->phase_started_us >= (int64_t)IOT_PHASE_MS * 1000LL))
        {
            iot_stop_phase_();
            s_iot->phase = (iot_phase_t)((s_iot->phase + 1U) % 3U);
            iot_start_phase_();
        }
        iot_draw_();
    }
    vTaskDelete(NULL);
}

void menu_device_inspector_show(void)
{
    iot_exit_();
    s_iot = heap_caps_calloc(1U, sizeof(*s_iot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_iot == NULL)
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    s_iot->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    for(uint8_t i = 0U; i < IOT_WIFI_WINDOW; ++i) s_iot->wifi_ptrs[i] = &s_iot->wifi_records[i];
    s_iot->buttons = xQueueCreate(8U, sizeof(iot_button_t));
    s_iot->active = true;
    if(s_iot->buttons == NULL ||
       !poom_sbus_subscribe_cb("input/button", iot_button_cb_, (void *)s_iot_user))
    {
        iot_exit_();
        return;
    }
    s_iot->subscribed = true;
    iot_draw_();
    iot_start_phase_();
    if(xTaskCreate(iot_task_, "menu_device_inspector", IOT_STACK, NULL, IOT_PRIORITY,
                   &s_iot->task) != pdPASS)
    {
        iot_exit_();
    }
}
