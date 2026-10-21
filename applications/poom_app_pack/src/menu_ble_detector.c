// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#include "menu_ble_detector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button_driver.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "menu_detector_view.h"
#include "menu_fox_hunt.h"
#include "menu_wifi_detector_oui.h"
#include "poom_ble_detector.h"
#include "poom_sbus.h"
#include "poom_secrets_store.h"
#include "poom_ui_keyboard.h"

#define POOM_MENU_RESUME_TOPIC        "poom/menu/resume"
#define MENU_BLE_DETECTOR_REFRESH_MS  (1200U)
#define MENU_BLE_DETECTOR_TASK_STACK  (4096U)
#define MENU_BLE_DETECTOR_TASK_PRIO   (5U)
#define MENU_BLE_DETECTOR_ALIAS_LEN   (24U)
#define MENU_BLE_DETECTOR_LIST_CHARS  (13U)
#define MENU_BLE_DETECTOR_SCROLL_MS   (350U)
#define MENU_BLE_DETECTOR_SCROLL_GAP  (3U)
#define MENU_BLE_ALIAS_UNCHECKED      (0U)
#define MENU_BLE_ALIAS_NONE           (1U)
#define MENU_BLE_ALIAS_PRESENT        (2U)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_BLE_DETECTOR_SCREEN_SELECT = 0,
    MENU_BLE_DETECTOR_SCREEN_SCAN,
    MENU_BLE_DETECTOR_SCREEN_DETAIL,
    MENU_BLE_DETECTOR_SCREEN_ALIAS,
} menu_ble_detector_screen_t;

static const char *const s_filter_labels[] = {
    "BLE DEVICES",
    "TRACKERS",
    "WEARABLES",
};

static poom_ble_detector_record_t *s_records[MENU_DETECTOR_VIEW_MAX_ITEMS];
static poom_ble_detector_record_t *s_record_storage;
static char (*s_names)[MENU_DETECTOR_VIEW_NAME_LEN];
static uint8_t s_alias_state[MENU_DETECTOR_VIEW_MAX_ITEMS];
static int s_record_count;
static int s_selected_index;
static int s_scroll;
static int s_filter_index;
static int s_detail_index = -1;
/* LEFT toggles the whole discovery list between labels and MAC addresses. */
static bool s_show_mac_list;
static menu_ble_detector_screen_t s_screen = MENU_BLE_DETECTOR_SCREEN_SELECT;
static volatile bool s_active;
static bool s_exit_requested;
static bool s_hunt_launch;
static uint8_t s_hunt_address[6];
static char s_hunt_name[MENU_DETECTOR_VIEW_NAME_LEN];
static bool s_start_requested;
static bool s_scan_dirty;
static bool s_input_dirty;
static bool s_buttons_subscribed;
static bool s_secrets_ready;
static TaskHandle_t s_task;
static portMUX_TYPE s_records_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_selection_tick_ms;
static uint32_t s_last_scroll_slot;
static poom_ui_keyboard_t s_alias_keyboard;
static char s_alias_buffer[MENU_BLE_DETECTOR_ALIAS_LEN];

static void menu_ble_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_ble_detector_free_(void);

static uint32_t menu_ble_detector_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void menu_ble_detector_reset_text_scroll_(void)
{
    s_selection_tick_ms = menu_ble_detector_now_ms_();
    s_last_scroll_slot = UINT32_MAX;
}

static bool menu_ble_detector_text_scroll_active_(void)
{
    return (s_screen == MENU_BLE_DETECTOR_SCREEN_SCAN) &&
           (s_selected_index >= 0) &&
           (s_selected_index < s_record_count) &&
           (strnlen(s_names[s_selected_index], MENU_DETECTOR_VIEW_NAME_LEN) >
            MENU_BLE_DETECTOR_LIST_CHARS);
}

static void menu_ble_detector_format_row_(char *out,
                                           size_t out_len,
                                           const char *label,
                                           bool selected)
{
    size_t label_len;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }

    out[0] = '\0';
    if(label == NULL)
    {
        return;
    }

    label_len = strnlen(label, MENU_DETECTOR_VIEW_NAME_LEN);
    if(!selected || (label_len <= MENU_BLE_DETECTOR_LIST_CHARS))
    {
        (void)snprintf(out, out_len, "%.13s", label);
        return;
    }

    size_t cycle_width = label_len + MENU_BLE_DETECTOR_SCROLL_GAP;
    size_t phase = (size_t)(((menu_ble_detector_now_ms_() - s_selection_tick_ms) /
                              MENU_BLE_DETECTOR_SCROLL_MS) % cycle_width);

    for(size_t out_index = 0U;
        (out_index < MENU_BLE_DETECTOR_LIST_CHARS) && (out_index + 1U < out_len);
        ++out_index)
    {
        size_t source_index = (phase + out_index) % cycle_width;
        out[out_index] = (source_index < label_len) ? label[source_index] : ' ';
        out[out_index + 1U] = '\0';
    }
}

static uint64_t menu_ble_detector_hash_bytes_(uint64_t hash,
                                               const uint8_t *data,
                                               size_t data_len)
{
    size_t i;
    for(i = 0U; (data != NULL) && (i < data_len); ++i)
    {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void menu_ble_detector_alias_id_(char *out,
                                        size_t out_len,
                                        const poom_ble_detector_record_t *record)
{
    uint64_t hash = 1469598103934665603ULL;
    uint8_t normalized[POOM_BLE_DETECTOR_ADV_LEN];
    size_t normalized_len;

    if((out == NULL) || (out_len == 0U) || (record == NULL))
    {
        return;
    }

    hash = menu_ble_detector_hash_bytes_(hash,
                                         (const uint8_t *)"ble_detector_alias_v1",
                                         sizeof("ble_detector_alias_v1") - 1U);
    hash = menu_ble_detector_hash_bytes_(hash,
                                         (const uint8_t *)&record->device_class,
                                         sizeof(record->device_class));
    hash = menu_ble_detector_hash_bytes_(hash,
                                         (const uint8_t *)record->name,
                                         strnlen(record->name, sizeof(record->name)));
    hash = menu_ble_detector_hash_bytes_(hash,
                                         (const uint8_t *)record->vendor,
                                         strnlen(record->vendor, sizeof(record->vendor)));

    normalized_len = record->adv_data_length;
    if(normalized_len > sizeof(normalized))
    {
        normalized_len = sizeof(normalized);
    }
    (void)memcpy(normalized, record->adv_data, normalized_len);
    if((record->device_class == POOM_BLE_DETECTOR_CLASS_AIRTAG) && (normalized_len > 0U))
    {
        if(normalized_len > 6U)
        {
            normalized[6] = 0U;
        }
        normalized[normalized_len - 1U] = 0U;
    }
    hash = menu_ble_detector_hash_bytes_(hash, normalized, normalized_len);
    (void)snprintf(out, out_len, "ble_det/%016llX", (unsigned long long)hash);
}

static bool menu_ble_detector_alias_load_(const poom_ble_detector_record_t *record,
                                           char *out,
                                           size_t out_len)
{
    char id[64];
    void *blob = NULL;
    size_t blob_len = 0U;
    size_t copy_len;

    if((record == NULL) || (out == NULL) || (out_len == 0U) || !s_secrets_ready)
    {
        return false;
    }

    out[0] = '\0';
    menu_ble_detector_alias_id_(id, sizeof(id), record);
    if(poom_secrets_get_record_blob_alloc(id, &blob, &blob_len) != ESP_OK)
    {
        return false;
    }
    if((blob == NULL) || (blob_len == 0U))
    {
        free(blob);
        return false;
    }

    copy_len = (blob_len < out_len) ? blob_len : (out_len - 1U);
    (void)memcpy(out, blob, copy_len);
    out[copy_len] = '\0';
    free(blob);
    return out[0] != '\0';
}

static void menu_ble_detector_alias_save_(const poom_ble_detector_record_t *record,
                                           const char *alias)
{
    char id[64];
    size_t len;

    if((record == NULL) || (alias == NULL) || !s_secrets_ready)
    {
        return;
    }

    menu_ble_detector_alias_id_(id, sizeof(id), record);
    if(alias[0] == '\0')
    {
        (void)poom_secrets_erase_record(id);
        return;
    }

    len = strnlen(alias, sizeof(s_alias_buffer) - 1U);
    (void)poom_secrets_set_record_blob(id, alias, len + 1U);
}

static bool menu_ble_detector_alloc_(void)
{
    int i;

    (void)memset(s_records, 0, sizeof(s_records));
    s_record_storage = heap_caps_malloc(
        MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_record_storage),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(s_record_storage == NULL)
    {
        return false;
    }
    (void)memset(s_record_storage,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_record_storage));
    for(i = 0; i < MENU_DETECTOR_VIEW_MAX_ITEMS; ++i)
    {
        s_records[i] = &s_record_storage[i];
    }

    s_names = heap_caps_malloc(MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_names),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(s_names == NULL)
    {
        menu_ble_detector_free_();
        return false;
    }
    (void)memset(s_names,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_names));
    return true;
}

static void menu_ble_detector_free_(void)
{
    free(s_names);
    s_names = NULL;
    free(s_record_storage);
    s_record_storage = NULL;
    (void)memset(s_records, 0, sizeof(s_records));
}

static int menu_ble_detector_find_(const uint8_t mac[6])
{
    int i;
    for(i = 0; i < s_record_count; ++i)
    {
        if(memcmp(s_records[i]->mac_address, mac, 6U) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void menu_ble_detector_copy_name_(char out[MENU_DETECTOR_VIEW_NAME_LEN],
                                          const char *name)
{
    size_t copy_len;

    if((out == NULL) || (name == NULL))
    {
        return;
    }

    copy_len = strnlen(name, MENU_DETECTOR_VIEW_NAME_LEN - 1U);
    (void)memcpy(out, name, copy_len);
    out[copy_len] = '\0';
}

static void menu_ble_detector_record_cb_(const poom_ble_detector_record_t *record)
{
    int index;

    if((record == NULL) || !s_active || (s_records[0] == NULL) || (s_names == NULL))
    {
        return;
    }

    portENTER_CRITICAL(&s_records_lock);
    index = menu_ble_detector_find_(record->mac_address);
    if(index < 0)
    {
        if(s_record_count >= MENU_DETECTOR_VIEW_MAX_ITEMS)
        {
            portEXIT_CRITICAL(&s_records_lock);
            return;
        }
        index = s_record_count++;
        *s_records[index] = *record;
        menu_ble_detector_copy_name_(s_names[index], record->name);
        s_alias_state[index] = MENU_BLE_ALIAS_UNCHECKED;
    }
    else
    {
        *s_records[index] = *record;
        if((s_alias_state[index] != MENU_BLE_ALIAS_PRESENT) &&
           ((strcmp(record->name, "UNKNOWN") != 0) ||
            (strcmp(s_names[index], "UNKNOWN") == 0)))
        {
            menu_ble_detector_copy_name_(s_names[index], record->name);
        }
    }
    s_scan_dirty = true;
    portEXIT_CRITICAL(&s_records_lock);
}

static void menu_ble_detector_load_aliases_(void)
{
    int i;

    for(i = 0; i < s_record_count; ++i)
    {
        poom_ble_detector_record_t record;
        char alias[MENU_BLE_DETECTOR_ALIAS_LEN];
        bool alias_loaded;

        if(s_alias_state[i] != MENU_BLE_ALIAS_UNCHECKED)
        {
            continue;
        }

        portENTER_CRITICAL(&s_records_lock);
        record = *s_records[i];
        portEXIT_CRITICAL(&s_records_lock);

        alias_loaded = menu_ble_detector_alias_load_(&record, alias, sizeof(alias));
        portENTER_CRITICAL(&s_records_lock);
        s_alias_state[i] = alias_loaded ? MENU_BLE_ALIAS_PRESENT : MENU_BLE_ALIAS_NONE;
        if(alias_loaded)
        {
            (void)snprintf(s_names[i], sizeof(s_names[i]), "%.23s", alias);
        }
        portEXIT_CRITICAL(&s_records_lock);
    }
}

static void menu_ble_detector_normalize_selection_(int count)
{
    int max_scroll;

    if(count <= 0)
    {
        s_selected_index = 0;
        s_scroll = 0;
        return;
    }
    if(s_selected_index < 0)
    {
        s_selected_index = 0;
    }
    if(s_selected_index >= count)
    {
        s_selected_index = count - 1;
    }
    if(s_selected_index < s_scroll)
    {
        s_scroll = s_selected_index;
    }
    if(s_selected_index >= (s_scroll + MENU_DETECTOR_VIEW_VISIBLE_ROWS))
    {
        s_scroll = s_selected_index - MENU_DETECTOR_VIEW_VISIBLE_ROWS + 1;
    }
    max_scroll = count - MENU_DETECTOR_VIEW_VISIBLE_ROWS;
    if(max_scroll < 0)
    {
        max_scroll = 0;
    }
    if(s_scroll > max_scroll)
    {
        s_scroll = max_scroll;
    }
    if(s_scroll < 0)
    {
        s_scroll = 0;
    }
}

static void menu_ble_detector_render_scan_(void)
{
    menu_detector_view_row_t rows[MENU_DETECTOR_VIEW_MAX_ITEMS];
    int count;
    int i;

    portENTER_CRITICAL(&s_records_lock);
    count = s_record_count;
    portEXIT_CRITICAL(&s_records_lock);

    if(count == 0)
    {
        menu_detector_view_draw_waiting(poom_ble_detector_filter_label(
            (poom_ble_detector_filter_t)s_filter_index));
        return;
    }

    menu_ble_detector_normalize_selection_(count);
    portENTER_CRITICAL(&s_records_lock);
    for(i = 0; i < count; ++i)
    {
        char mac_label[18];
        const char *display_name = s_names[i];
        const uint8_t *address = s_records[i]->mac_address;
        if(!s_show_mac_list &&
           ((display_name[0] == '\0') || (strcmp(display_name, "UNKNOWN") == 0)))
        {
            const char *vendor = NULL;
            if(menu_wifi_detector_oui_is_random(address))
            {
                display_name = "PRIVATE";
            }
            else
            {
                vendor = menu_wifi_detector_oui_vendor(address);
                if((vendor == NULL) && (s_records[i]->vendor[0] != '\0') &&
                   (strcmp(s_records[i]->vendor, "UNKNOWN") != 0))
                {
                    vendor = s_records[i]->vendor;
                }
                display_name = (vendor != NULL) ? vendor : "UNKNOWN";
            }
        }
        if(s_show_mac_list)
        {
            const uint8_t *mac = s_records[i]->mac_address;
            (void)snprintf(mac_label, sizeof(mac_label),
                           "%02X:%02X:%02X:%02X:%02X:%02X",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            display_name = mac_label;
        }
        menu_ble_detector_format_row_(rows[i].name,
                                      sizeof(rows[i].name),
                                      display_name,
                                      i == s_selected_index);
        rows[i].rssi = s_records[i]->rssi;
        rows[i].rssi_known = true;
    }
    portEXIT_CRITICAL(&s_records_lock);
    menu_detector_view_draw_list_actions(
        poom_ble_detector_filter_label((poom_ble_detector_filter_t)s_filter_index),
        rows,
        count,
        s_selected_index,
        s_scroll,
        s_show_mac_list ? "A:SEL L:OUI" : "A:SEL L:MAC",
        "B:EXIT");
}

static bool menu_ble_detector_record_snapshot_(int index,
                                                poom_ble_detector_record_t *record,
                                                char name[MENU_DETECTOR_VIEW_NAME_LEN],
                                                int *count)
{
    bool valid = false;

    portENTER_CRITICAL(&s_records_lock);
    *count = s_record_count;
    if((index >= 0) && (index < s_record_count))
    {
        *record = *s_records[index];
        (void)snprintf(name, MENU_DETECTOR_VIEW_NAME_LEN, "%.23s", s_names[index]);
        valid = true;
    }
    portEXIT_CRITICAL(&s_records_lock);
    return valid;
}

static void menu_ble_detector_render_detail_(void)
{
    poom_ble_detector_record_t record;
    char name[MENU_DETECTOR_VIEW_NAME_LEN];
    char line0[22];
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];
    const char *lines[5] = {line0, line1, line2, line3, line4};
    int count = 0;

    if(!menu_ble_detector_record_snapshot_(s_detail_index, &record, name, &count))
    {
        s_screen = MENU_BLE_DETECTOR_SCREEN_SCAN;
        menu_ble_detector_render_scan_();
        return;
    }

    (void)snprintf(line0, sizeof(line0), "%.21s", name);
    (void)snprintf(line1,
                   sizeof(line1),
                   "%.9s/%.10s",
                   poom_ble_detector_class_label(record.device_class),
                   poom_detect_confidence_label(record.confidence));
    (void)snprintf(line2, sizeof(line2), "d=%.2fm", (double)record.distance_m);
    (void)snprintf(line3, sizeof(line3), "RSSI=%ddBm", record.rssi);
    if(s_show_mac_list)
    {
        (void)snprintf(line4, sizeof(line4),
                       "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                       record.mac_address[0], record.mac_address[1], record.mac_address[2],
                       record.mac_address[3], record.mac_address[4], record.mac_address[5]);
    }
    else
    {
        const char *vendor = NULL;
        if(menu_wifi_detector_oui_is_random(record.mac_address))
        {
            vendor = "PRIVATE";
        }
        else
        {
            vendor = menu_wifi_detector_oui_vendor(record.mac_address);
            if((vendor == NULL) && (record.vendor[0] != '\0') &&
               (strcmp(record.vendor, "UNKNOWN") != 0))
            {
                vendor = record.vendor;
            }
        }
        (void)snprintf(line4, sizeof(line4), "OUI:%.17s",
                       (vendor != NULL) ? vendor : "UNKNOWN");
    }
    menu_detector_view_draw_detail(lines, s_detail_index, count);
}

static void menu_ble_detector_render_(void)
{
    if(s_screen == MENU_BLE_DETECTOR_SCREEN_SELECT)
    {
        menu_detector_view_draw_selector("BLE DETECT",
                                         s_filter_labels,
                                         POOM_BLE_DETECTOR_FILTER_COUNT,
                                         s_filter_index);
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_ALIAS)
    {
        poom_ui_keyboard_draw(&s_alias_keyboard);
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_DETAIL)
    {
        menu_ble_detector_render_detail_();
    }
    else
    {
        menu_ble_detector_render_scan_();
    }
}

static void menu_ble_detector_enter_alias_(void)
{
    poom_ble_detector_record_t record;
    char name[MENU_DETECTOR_VIEW_NAME_LEN];
    int count;

    if(!menu_ble_detector_record_snapshot_(s_detail_index, &record, name, &count))
    {
        return;
    }
    (void)count;
    (void)memset(s_alias_buffer, 0, sizeof(s_alias_buffer));
    (void)menu_ble_detector_alias_load_(&record, s_alias_buffer, sizeof(s_alias_buffer));
    poom_ui_keyboard_init(&s_alias_keyboard,
                          s_alias_buffer,
                          sizeof(s_alias_buffer),
                          record.name);
    s_screen = MENU_BLE_DETECTOR_SCREEN_ALIAS;
    poom_ui_keyboard_draw(&s_alias_keyboard);
}

static void menu_ble_detector_accept_alias_(void)
{
    poom_ble_detector_record_t record;
    char name[MENU_DETECTOR_VIEW_NAME_LEN];
    int count;

    if(menu_ble_detector_record_snapshot_(s_detail_index, &record, name, &count))
    {
        (void)count;
        menu_ble_detector_alias_save_(&record, s_alias_buffer);
        portENTER_CRITICAL(&s_records_lock);
        if((s_detail_index >= 0) && (s_detail_index < s_record_count))
        {
            (void)snprintf(s_names[s_detail_index],
                           sizeof(s_names[s_detail_index]),
                           "%.23s",
                           (s_alias_buffer[0] != '\0') ? s_alias_buffer : record.name);
            s_alias_state[s_detail_index] = (s_alias_buffer[0] != '\0') ?
                MENU_BLE_ALIAS_PRESENT : MENU_BLE_ALIAS_NONE;
        }
        portEXIT_CRITICAL(&s_records_lock);
    }
    s_screen = MENU_BLE_DETECTOR_SCREEN_DETAIL;
    s_input_dirty = true;
}

static void menu_ble_detector_cleanup_(void)
{
    const uint8_t token = 1U;

    s_active = false;
    s_start_requested = false;
    poom_ble_detector_register_cb(NULL);
    (void)poom_ble_detector_stop();
    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button",
                                       menu_ble_detector_button_cb_,
                                       "menu_ble_detector");
        s_buttons_subscribed = false;
    }
    menu_ble_detector_free_();
    s_task = NULL;
    /* Let the detector task fully relinquish its BLE scan and callbacks
     * before Fox Hunt registers its own scanner. */
    if(s_hunt_launch)
    {
        s_hunt_launch = false;
        vTaskDelay(pdMS_TO_TICKS(100U));
        app_fox_hunt_ble_device(s_hunt_address, s_hunt_name);
    }
    else
    {
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}

static void menu_ble_detector_task_(void *arg)
{
    TickType_t last_scan_render = 0;
    (void)arg;

    while(s_active)
    {
        if(s_exit_requested)
        {
            break;
        }

        if(s_start_requested)
        {
            esp_err_t status;
            s_start_requested = false;
            s_record_count = 0;
            s_selected_index = 0;
            s_scroll = 0;
            menu_ble_detector_reset_text_scroll_();
            (void)memset(s_alias_state, 0, sizeof(s_alias_state));
            menu_detector_view_draw_waiting(poom_ble_detector_filter_label(
                (poom_ble_detector_filter_t)s_filter_index));
            poom_ble_detector_register_cb(menu_ble_detector_record_cb_);
            status = poom_ble_detector_start((poom_ble_detector_filter_t)s_filter_index);
            if(status != ESP_OK)
            {
                poom_ble_detector_register_cb(NULL);
                menu_detector_view_draw_error("BLE DETECT", "Start failed", esp_err_to_name(status));
                vTaskDelay(pdMS_TO_TICKS(1200U));
                s_exit_requested = true;
                continue;
            }
            s_screen = MENU_BLE_DETECTOR_SCREEN_SCAN;
            s_input_dirty = true;
        }

        menu_ble_detector_load_aliases_();

        if(menu_ble_detector_text_scroll_active_())
        {
            uint32_t scroll_slot =
                (menu_ble_detector_now_ms_() - s_selection_tick_ms) /
                MENU_BLE_DETECTOR_SCROLL_MS;
            if(scroll_slot != s_last_scroll_slot)
            {
                s_last_scroll_slot = scroll_slot;
                s_input_dirty = true;
            }
        }

        if(s_input_dirty)
        {
            s_input_dirty = false;
            menu_ble_detector_render_();
            last_scan_render = xTaskGetTickCount();
        }
        else if(s_scan_dirty &&
                ((last_scan_render == 0U) ||
                 ((xTaskGetTickCount() - last_scan_render) >=
                  pdMS_TO_TICKS(MENU_BLE_DETECTOR_REFRESH_MS))))
        {
            s_scan_dirty = false;
            menu_ble_detector_render_();
            last_scan_render = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(100U));
    }

    menu_ble_detector_cleanup_();
    vTaskDelete(NULL);
}

static void menu_ble_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    button_event_msg_t event;
    (void)user_ctx;

    if(!s_active || (msg == NULL) || (msg->len < sizeof(event)))
    {
        return;
    }
    (void)memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(s_screen == MENU_BLE_DETECTOR_SCREEN_ALIAS)
    {
        poom_ui_keyboard_action_t action;
        if(event.button == BUTTON_B)
        {
            s_screen = MENU_BLE_DETECTOR_SCREEN_DETAIL;
            s_input_dirty = true;
            return;
        }
        action = poom_ui_keyboard_handle_button(&s_alias_keyboard, event.button);
        if(action == POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            menu_ble_detector_accept_alias_();
        }
        else
        {
            poom_ui_keyboard_draw(&s_alias_keyboard);
        }
        return;
    }

    if(s_screen == MENU_BLE_DETECTOR_SCREEN_SELECT)
    {
        if(event.button == BUTTON_B)
        {
            s_exit_requested = true;
        }
        else if((event.button == BUTTON_UP) && (s_filter_index > 0))
        {
            --s_filter_index;
            s_input_dirty = true;
        }
        else if((event.button == BUTTON_DOWN) &&
                (s_filter_index < (POOM_BLE_DETECTOR_FILTER_COUNT - 1)))
        {
            ++s_filter_index;
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_A)
        {
            s_start_requested = true;
            s_input_dirty = true;
        }
        return;
    }

    if(s_screen == MENU_BLE_DETECTOR_SCREEN_DETAIL)
    {
        if(event.button == BUTTON_B)
        {
            s_screen = MENU_BLE_DETECTOR_SCREEN_SCAN;
            s_detail_index = -1;
            menu_ble_detector_reset_text_scroll_();
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_LEFT)
        {
            s_show_mac_list = !s_show_mac_list;
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_RIGHT)
        {
            poom_ble_detector_record_t record;
            char name[MENU_DETECTOR_VIEW_NAME_LEN];
            int count;
            if(menu_ble_detector_record_snapshot_(s_detail_index, &record, name, &count))
            {
                (void)memcpy(s_hunt_address, record.mac_address, sizeof(s_hunt_address));
                (void)snprintf(s_hunt_name, sizeof(s_hunt_name), "%.23s", name);
                s_hunt_launch = true;
                s_exit_requested = true;
            }
        }
        else if(event.button == BUTTON_A)
        {
            menu_ble_detector_enter_alias_();
        }
        return;
    }

    if(s_record_count == 0)
    {
        if(event.button == BUTTON_B)
        {
            s_exit_requested = true;
        }
        return;
    }

    if(event.button == BUTTON_B)
    {
        s_exit_requested = true;
    }
    else if(event.button == BUTTON_LEFT)
    {
        s_show_mac_list = !s_show_mac_list;
        menu_ble_detector_reset_text_scroll_();
        s_input_dirty = true;
    }
    else if((event.button == BUTTON_UP) && (s_selected_index > 0))
    {
        --s_selected_index;
        menu_ble_detector_reset_text_scroll_();
        s_input_dirty = true;
    }
    else if((event.button == BUTTON_DOWN) &&
            (s_selected_index < (s_record_count - 1)))
    {
        ++s_selected_index;
        menu_ble_detector_reset_text_scroll_();
        s_input_dirty = true;
    }
    else if(event.button == BUTTON_A)
    {
        s_detail_index = s_selected_index;
        s_screen = MENU_BLE_DETECTOR_SCREEN_DETAIL;
        s_input_dirty = true;
    }
}

void menu_ble_detector_show(void)
{
    if(s_active)
    {
        return;
    }

    if(!menu_ble_detector_alloc_())
    {
        const uint8_t token = 1U;
        menu_detector_view_draw_error("BLE DETECT", "No memory", "Try again");
        vTaskDelay(pdMS_TO_TICKS(1200U));
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    s_record_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_filter_index = 0;
    s_detail_index = -1;
    s_screen = MENU_BLE_DETECTOR_SCREEN_SELECT;
    s_exit_requested = false;
    s_hunt_launch = false;
    s_start_requested = false;
    s_scan_dirty = false;
    s_input_dirty = true;
    menu_ble_detector_reset_text_scroll_();
    s_secrets_ready = (poom_secrets_init() == ESP_OK);
    (void)memset(s_alias_state, 0, sizeof(s_alias_state));

    if(!s_buttons_subscribed)
    {
        if(!poom_sbus_subscribe_cb("input/button",
                                   menu_ble_detector_button_cb_,
                                   "menu_ble_detector"))
        {
            const uint8_t token = 1U;
            menu_ble_detector_free_();
            menu_detector_view_draw_error("BLE DETECT", "Button setup", "failed");
            vTaskDelay(pdMS_TO_TICKS(1200U));
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
        s_buttons_subscribed = true;
    }

    s_active = true;
    if(xTaskCreate(menu_ble_detector_task_,
                   "menu_ble_detector",
                   MENU_BLE_DETECTOR_TASK_STACK,
                   NULL,
                   MENU_BLE_DETECTOR_TASK_PRIO,
                   &s_task) != pdPASS)
    {
        const uint8_t token = 1U;
        s_active = false;
        if(s_buttons_subscribed)
        {
            (void)poom_sbus_unsubscribe_cb("input/button",
                                           menu_ble_detector_button_cb_,
                                           "menu_ble_detector");
            s_buttons_subscribed = false;
        }
        menu_ble_detector_free_();
        menu_detector_view_draw_error("BLE DETECT", "Task create", "failed");
        vTaskDelay(pdMS_TO_TICKS(1200U));
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}
