// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#include "menu_ble_detector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button_driver.h"
#include "Arduboy2.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "menu_detector_view.h"
#include "menu_fox_hunt.h"
#include "menu_wifi_detector_oui.h"
#include "poom_ble_detector.h"
#include "poom_ble_gatt_client.h"
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
#define MENU_BLE_GATT_MAX_SERVICES    (8U)
#define MENU_BLE_GATT_MAX_CHARS       (18U)
#define MENU_BLE_GATT_VALUE_BYTES     (8U)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_BLE_DETECTOR_SCREEN_SELECT = 0,
    MENU_BLE_DETECTOR_SCREEN_SCAN,
    MENU_BLE_DETECTOR_SCREEN_DETAIL,
    MENU_BLE_DETECTOR_SCREEN_ALIAS,
    MENU_BLE_DETECTOR_SCREEN_ACTIONS,
    MENU_BLE_DETECTOR_SCREEN_GATT_WAIT,
    MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES,
    MENU_BLE_DETECTOR_SCREEN_GATT_CHARS,
    MENU_BLE_DETECTOR_SCREEN_GATT_VALUE,
} menu_ble_detector_screen_t;

typedef struct
{
    esp_bt_uuid_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} menu_ble_gatt_service_t;

typedef struct
{
    esp_bt_uuid_t uuid;
    uint16_t handle;
    uint8_t properties;
    uint8_t service_index;
} menu_ble_gatt_char_t;

/* This is overlaid on the detector's record block only while scanning is
 * stopped for GATT. It keeps recon data off permanent HP SRAM. */
typedef struct
{
    bool start_requested;
    bool stop_requested;
    bool gatt_started;
    bool discovery_done;
    bool read_ready;
    uint8_t target_mac[6];
    char target_name[MENU_DETECTOR_VIEW_NAME_LEN];
    char status[22];
    char value_hex[(MENU_BLE_GATT_VALUE_BYTES * 3U) + 1U];
    uint8_t service_count;
    uint8_t char_count;
    uint8_t selected_service;
    uint8_t selected_char;
    menu_ble_gatt_service_t services[MENU_BLE_GATT_MAX_SERVICES];
    menu_ble_gatt_char_t chars[MENU_BLE_GATT_MAX_CHARS];
} menu_ble_gatt_recon_t;

_Static_assert(sizeof(menu_ble_gatt_recon_t) <=
                   (MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(poom_ble_detector_record_t)),
               "GATT recon state must fit the existing detector session block");

static const char *const s_filter_labels[] = {
    "BLE DEVICES",
    "TRACKERS",
    "WEARABLES",
};

static poom_ble_detector_record_t *s_records[MENU_DETECTOR_VIEW_MAX_ITEMS];
static poom_ble_detector_record_t *s_record_storage;
#define s_gatt_recon ((menu_ble_gatt_recon_t *)s_record_storage)
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
    if((s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES) ||
       (s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_CHARS))
    {
        return true;
    }
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

static void menu_ble_detector_gatt_uuid_(char *out, size_t out_len, const esp_bt_uuid_t *uuid)
{
    if((out == NULL) || (out_len == 0U) || (uuid == NULL))
    {
        return;
    }
    if(uuid->len == ESP_UUID_LEN_16)
    {
        (void)snprintf(out, out_len, "%04X", uuid->uuid.uuid16);
    }
    else if(uuid->len == ESP_UUID_LEN_32)
    {
        (void)snprintf(out, out_len, "%08lX", (unsigned long)uuid->uuid.uuid32);
    }
    else
    {
        (void)snprintf(out, out_len, "128-BIT");
    }
}

static const char *menu_ble_detector_gatt_service_label_(const esp_bt_uuid_t *uuid)
{
    if((uuid == NULL) || (uuid->len != ESP_UUID_LEN_16))
    {
        return "Custom service";
    }
    switch(uuid->uuid.uuid16)
    {
        case 0x1800U: return "Generic Access";
        case 0x1801U: return "Generic Attribute";
        case 0x180AU: return "Device Information";
        case 0x180FU: return "Battery Service";
        case 0x1812U: return "Human Interface";
        case 0x181AU: return "Environment";
        default: return "GATT service";
    }
}

static void menu_ble_detector_gatt_props_(char *out, size_t out_len, uint8_t props)
{
    size_t at = 0U;
    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if((props & ESP_GATT_CHAR_PROP_BIT_READ) != 0U && (at + 1U < out_len)) out[at++] = 'R';
    if((props & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0U && (at + 1U < out_len)) out[at++] = 'W';
    if((props & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0U && (at + 2U < out_len)) { out[at++] = 'W'; out[at++] = 'N'; }
    if((props & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0U && (at + 1U < out_len)) out[at++] = 'N';
    if((props & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0U && (at + 1U < out_len)) out[at++] = 'I';
    out[at] = '\0';
    if(at == 0U) (void)snprintf(out, out_len, "-");
}

static void menu_ble_detector_gatt_format_row_(char *out, size_t out_len,
                                                const char *label, bool selected)
{
    const size_t width = 20U;
    const size_t label_len = (label != NULL) ? strnlen(label, MENU_DETECTOR_VIEW_NAME_LEN - 1U) : 0U;
    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if((label == NULL) || !selected || (label_len <= width))
    {
        (void)snprintf(out, out_len, "%.*s", (int)width, (label != NULL) ? label : "");
        return;
    }
    const size_t cycle = label_len + MENU_BLE_DETECTOR_SCROLL_GAP;
    const size_t phase = (size_t)(((menu_ble_detector_now_ms_() - s_selection_tick_ms) /
                                   MENU_BLE_DETECTOR_SCROLL_MS) % cycle);
    for(size_t i = 0U; (i < width) && (i + 1U < out_len); ++i)
    {
        const size_t source = (phase + i) % cycle;
        out[i] = (source < label_len) ? label[source] : ' ';
        out[i + 1U] = '\0';
    }
}

static void menu_ble_detector_gatt_draw_list_(const char *title,
                                              const char labels[][MENU_DETECTOR_VIEW_NAME_LEN],
                                              int count,
                                              const char *action)
{
    const int max_scroll = (count > MENU_DETECTOR_VIEW_VISIBLE_ROWS) ?
        (count - MENU_DETECTOR_VIEW_VISIBLE_ROWS) : 0;
    const int title_x = ((int)ARDUBOY_WIDTH - ((int)strlen(title) * 6)) / 2;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor((title_x > 0) ? title_x : 0, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    for(int row = 0; row < MENU_DETECTOR_VIEW_VISIBLE_ROWS; ++row)
    {
        const int index = s_scroll + row;
        char line[MENU_DETECTOR_VIEW_NAME_LEN];
        const int16_t y = (int16_t)(14 + row * 10);
        if(index >= count)
        {
            break;
        }
        menu_ble_detector_gatt_format_row_(line, sizeof(line), labels[index], index == s_selected_index);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);
        if(index == s_selected_index)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }
    if(s_scroll > 0)
    {
        poom_arduboy_fill_triangle(124, 11, 120, 15, 127, 15, WHITE);
    }
    if(s_scroll < max_scroll)
    {
        poom_arduboy_fill_triangle(120, 50, 127, 50, 124, 54, WHITE);
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(action);
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_ble_detector_gatt_render_wait_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(30, 2);
    (void)poom_arduboy_print(F("GATT RECON"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(s_gatt_recon->target_name);
    poom_arduboy_set_cursor(4, 32);
    (void)poom_arduboy_print(s_gatt_recon->status);
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_ble_detector_gatt_render_services_(void)
{
    char labels[MENU_BLE_GATT_MAX_SERVICES][MENU_DETECTOR_VIEW_NAME_LEN];
    int count = s_gatt_recon->service_count;

    for(int i = 0; i < count; ++i)
    {
        char uuid[12];
        menu_ble_detector_gatt_uuid_(uuid, sizeof(uuid), &s_gatt_recon->services[i].uuid);
        (void)snprintf(labels[i], sizeof(labels[i]), "%.8s %.14s", uuid,
                       menu_ble_detector_gatt_service_label_(&s_gatt_recon->services[i].uuid));
    }
    if(count == 0)
    {
        menu_detector_view_draw_error("GATT RECON", "No services", "B:BACK");
        return;
    }
    menu_ble_detector_gatt_draw_list_("GATT SERVICES", labels, count, "A:OPEN");
}

static void menu_ble_detector_gatt_render_chars_(void)
{
    char labels[MENU_BLE_GATT_MAX_CHARS][MENU_DETECTOR_VIEW_NAME_LEN];
    int count = 0;

    for(uint8_t i = 0U; i < s_gatt_recon->char_count; ++i)
    {
        char uuid[12];
        char props[8];
        if(s_gatt_recon->chars[i].service_index != s_gatt_recon->selected_service)
        {
            continue;
        }
        menu_ble_detector_gatt_uuid_(uuid, sizeof(uuid), &s_gatt_recon->chars[i].uuid);
        menu_ble_detector_gatt_props_(props, sizeof(props), s_gatt_recon->chars[i].properties);
        (void)snprintf(labels[count], sizeof(labels[count]), "%s %s", uuid, props);
        ++count;
    }
    if(count == 0)
    {
        menu_detector_view_draw_error("GATT CHARS", "No characteristics", "B:BACK");
        return;
    }
    menu_ble_detector_gatt_draw_list_("GATT CHARS", labels, count, "A:INFO");
}

static int menu_ble_detector_gatt_char_at_selection_(void)
{
    int selected = s_selected_index;
    for(uint8_t i = 0U; i < s_gatt_recon->char_count; ++i)
    {
        if(s_gatt_recon->chars[i].service_index != s_gatt_recon->selected_service)
        {
            continue;
        }
        if(selected-- == 0)
        {
            return i;
        }
    }
    return -1;
}

static void menu_ble_detector_gatt_render_value_(void)
{
    const int index = s_gatt_recon->selected_char;
    char uuid[12];
    char props[8];
    if(index < 0 || index >= s_gatt_recon->char_count)
    {
        s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_CHARS;
        s_input_dirty = true;
        return;
    }
    menu_ble_detector_gatt_uuid_(uuid, sizeof(uuid), &s_gatt_recon->chars[index].uuid);
    menu_ble_detector_gatt_props_(props, sizeof(props), s_gatt_recon->chars[index].properties);
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("GATT CHAR"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_set_cursor(4, 17);
    (void)poom_arduboy_print(uuid);
    poom_arduboy_set_cursor(48, 17);
    (void)poom_arduboy_print(props);
    poom_arduboy_set_cursor(4, 31);
    (void)poom_arduboy_print(s_gatt_recon->read_ready ? s_gatt_recon->value_hex : "No value read");
    poom_arduboy_set_cursor(0, 56);
    if((s_gatt_recon->chars[index].properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0U)
    {
        (void)poom_arduboy_print(F("A:READ"));
    }
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
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
    menu_detector_view_draw_detail_actions(lines, s_detail_index, count, "A:ACTIONS");
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
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_ACTIONS)
    {
        static const char *const actions[] = {"GATT RECON", "RENAME"};
        menu_detector_view_draw_selector("BLE ACTIONS", actions,
                                         sizeof(actions) / sizeof(actions[0]), s_scroll);
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_WAIT)
    {
        menu_ble_detector_gatt_render_wait_();
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES)
    {
        menu_ble_detector_gatt_render_services_();
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_CHARS)
    {
        menu_ble_detector_gatt_render_chars_();
    }
    else if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_VALUE)
    {
        menu_ble_detector_gatt_render_value_();
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

static void menu_ble_detector_gatt_event_(esp_gattc_cb_event_t event,
                                          esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param)
{
    menu_ble_gatt_recon_t *recon = s_gatt_recon;

    if((recon == NULL) || (param == NULL) || !recon->gatt_started)
    {
        return;
    }

    switch(event)
    {
        case ESP_GATTC_OPEN_EVT:
            if(param->open.status != ESP_GATT_OK)
            {
                (void)snprintf(recon->status, sizeof(recon->status), "Connect failed");
            }
            break;

        case ESP_GATTC_CONNECT_EVT:
            (void)snprintf(recon->status, sizeof(recon->status), "Discovering...");
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if(recon->service_count < MENU_BLE_GATT_MAX_SERVICES)
            {
                menu_ble_gatt_service_t *service = &recon->services[recon->service_count++];
                service->uuid = param->search_res.srvc_id.uuid;
                service->start_handle = param->search_res.start_handle;
                service->end_handle = param->search_res.end_handle;
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            if(param->search_cmpl.status != ESP_GATT_OK)
            {
                (void)snprintf(recon->status, sizeof(recon->status), "Discovery failed");
                break;
            }
            for(uint8_t svc = 0U; svc < recon->service_count; ++svc)
            {
                uint16_t count = 0U;
                esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                    gattc_if, param->search_cmpl.conn_id, ESP_GATT_DB_CHARACTERISTIC,
                    recon->services[svc].start_handle, recon->services[svc].end_handle,
                    0U, &count);
                if((status != ESP_GATT_OK) || (count == 0U))
                {
                    continue;
                }
                const uint16_t remaining = (uint16_t)(MENU_BLE_GATT_MAX_CHARS - recon->char_count);
                if(remaining == 0U)
                {
                    break;
                }
                if(count > remaining)
                {
                    count = remaining;
                }
                esp_gattc_char_elem_t *elements = heap_caps_malloc(
                    (size_t)count * sizeof(*elements), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if(elements == NULL)
                {
                    (void)snprintf(recon->status, sizeof(recon->status), "Memory limited");
                    break;
                }
                if(esp_ble_gattc_get_all_char(gattc_if, param->search_cmpl.conn_id,
                                               recon->services[svc].start_handle,
                                               recon->services[svc].end_handle,
                                               elements, &count, 0U) == ESP_GATT_OK)
                {
                    for(uint16_t i = 0U;
                        (i < count) && (recon->char_count < MENU_BLE_GATT_MAX_CHARS);
                        ++i)
                    {
                        menu_ble_gatt_char_t *ch = &recon->chars[recon->char_count++];
                        ch->uuid = elements[i].uuid;
                        ch->handle = elements[i].char_handle;
                        ch->properties = elements[i].properties;
                        ch->service_index = svc;
                    }
                }
                free(elements);
            }
            recon->discovery_done = true;
            (void)snprintf(recon->status, sizeof(recon->status), "%u services", recon->service_count);
            break;

        case ESP_GATTC_READ_CHAR_EVT:
            if(param->read.status == ESP_GATT_OK)
            {
                size_t at = 0U;
                const size_t n = (param->read.value_len < MENU_BLE_GATT_VALUE_BYTES) ?
                    param->read.value_len : MENU_BLE_GATT_VALUE_BYTES;
                for(size_t i = 0U; i < n && (at + 3U < sizeof(recon->value_hex)); ++i)
                {
                    at += (size_t)snprintf(&recon->value_hex[at],
                                           sizeof(recon->value_hex) - at,
                                           "%02X ", param->read.value[i]);
                }
                recon->read_ready = true;
            }
            else
            {
                (void)snprintf(recon->value_hex, sizeof(recon->value_hex), "Read denied");
                recon->read_ready = true;
            }
            break;

        case ESP_GATTC_DISCONNECT_EVT:
            (void)snprintf(recon->status, sizeof(recon->status), "Disconnected");
            break;

        default:
            break;
    }
    s_input_dirty = true;
}

static void menu_ble_detector_gatt_start_(void)
{
    poom_ble_gatt_client_scan_params_t scan_params;
    poom_ble_gatt_client_event_cb_t event_cb = {0};
    esp_err_t status;

    if((s_record_storage == NULL) || !s_gatt_recon->start_requested)
    {
        return;
    }
    s_gatt_recon->start_requested = false;
    (void)poom_ble_detector_stop();

    scan_params.remote_filter_service_uuid = poom_ble_gatt_client_default_service_uuid();
    scan_params.remote_filter_char_uuid = poom_ble_gatt_client_default_char_uuid();
    scan_params.notify_descr_uuid = poom_ble_gatt_client_default_notify_descr_uuid();
    scan_params.ble_scan_params = poom_ble_gatt_client_default_scan_params();
    scan_params.ble_scan_params.scan_type = BLE_SCAN_TYPE_ACTIVE;
    event_cb.handler_client_cb = menu_ble_detector_gatt_event_;

    poom_ble_gatt_client_set_remote_device_name(NULL);
    poom_ble_gatt_client_set_remote_bda(s_gatt_recon->target_mac);
    poom_ble_gatt_client_set_bonding(false);
    poom_ble_gatt_client_set_scan_params(&scan_params);
    poom_ble_gatt_client_set_recon_mode(true);
    poom_ble_gatt_client_set_callbacks(event_cb);
    s_gatt_recon->gatt_started = true;
    status = poom_ble_gatt_client_start();
    if(status != ESP_OK)
    {
        s_gatt_recon->gatt_started = false;
        (void)snprintf(s_gatt_recon->status, sizeof(s_gatt_recon->status), "Start failed");
    }
    else
    {
        (void)snprintf(s_gatt_recon->status, sizeof(s_gatt_recon->status), "Connecting...");
    }
    s_input_dirty = true;
}

static void menu_ble_detector_gatt_leave_(void)
{
    poom_ble_gatt_client_event_cb_t empty_cb = {0};

    if(s_record_storage == NULL)
    {
        return;
    }
    if(s_gatt_recon->gatt_started)
    {
        poom_ble_gatt_client_set_callbacks(empty_cb);
        (void)poom_ble_gatt_client_stop();
    }
    poom_ble_gatt_client_set_remote_bda(NULL);
    poom_ble_gatt_client_set_recon_mode(false);
    s_gatt_recon->gatt_started = false;
    s_record_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_detail_index = -1;
    s_screen = MENU_BLE_DETECTOR_SCREEN_SCAN;
    s_start_requested = true;
    s_input_dirty = true;
}

static void menu_ble_detector_enter_gatt_(void)
{
    poom_ble_detector_record_t record;
    char name[MENU_DETECTOR_VIEW_NAME_LEN];
    int count;

    if(!menu_ble_detector_record_snapshot_(s_detail_index, &record, name, &count) ||
       (s_record_storage == NULL))
    {
        return;
    }
    (void)count;
    poom_ble_detector_register_cb(NULL);
    (void)memset(s_gatt_recon, 0, sizeof(*s_gatt_recon));
    (void)memcpy(s_gatt_recon->target_mac, record.mac_address, sizeof(s_gatt_recon->target_mac));
    (void)snprintf(s_gatt_recon->target_name, sizeof(s_gatt_recon->target_name), "%.23s", name);
    (void)snprintf(s_gatt_recon->status, sizeof(s_gatt_recon->status), "Preparing...");
    s_gatt_recon->start_requested = true;
    s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_WAIT;
    s_input_dirty = true;
}

static void menu_ble_detector_cleanup_(void)
{
    const uint8_t token = 1U;

    s_active = false;
    s_start_requested = false;
    poom_ble_detector_register_cb(NULL);
    /* The GATT session overlays record storage. Only inspect that overlay
     * while a GATT screen is actually active; a normal BLE record may have a
     * non-zero byte at the same offset. */
    if((s_record_storage != NULL) &&
       (s_screen >= MENU_BLE_DETECTOR_SCREEN_GATT_WAIT) &&
       s_gatt_recon->gatt_started)
    {
        poom_ble_gatt_client_event_cb_t empty_cb = {0};
        poom_ble_gatt_client_set_callbacks(empty_cb);
        (void)poom_ble_gatt_client_stop();
        poom_ble_gatt_client_set_remote_bda(NULL);
        poom_ble_gatt_client_set_recon_mode(false);
    }
    else
    {
        (void)poom_ble_detector_stop();
    }
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

        if((s_record_storage != NULL) &&
           (s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_WAIT) &&
           s_gatt_recon->start_requested)
        {
            menu_ble_detector_gatt_start_();
        }
        if((s_record_storage != NULL) &&
           (s_screen >= MENU_BLE_DETECTOR_SCREEN_GATT_WAIT) &&
           s_gatt_recon->stop_requested)
        {
            menu_ble_detector_gatt_leave_();
            continue;
        }
        if((s_record_storage != NULL) &&
           (s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_WAIT) &&
           s_gatt_recon->discovery_done)
        {
            s_selected_index = 0;
            s_scroll = 0;
            menu_ble_detector_reset_text_scroll_();
            s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES;
            s_input_dirty = true;
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

        if(s_screen < MENU_BLE_DETECTOR_SCREEN_GATT_WAIT)
        {
            menu_ble_detector_load_aliases_();
        }

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
            s_scroll = 0;
            s_screen = MENU_BLE_DETECTOR_SCREEN_ACTIONS;
            s_input_dirty = true;
        }
        return;
    }

    if(s_screen == MENU_BLE_DETECTOR_SCREEN_ACTIONS)
    {
        if(event.button == BUTTON_B)
        {
            s_screen = MENU_BLE_DETECTOR_SCREEN_DETAIL;
            s_input_dirty = true;
        }
        else if((event.button == BUTTON_UP) && (s_scroll > 0))
        {
            --s_scroll;
            s_input_dirty = true;
        }
        else if((event.button == BUTTON_DOWN) && (s_scroll < 1))
        {
            ++s_scroll;
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_A)
        {
            if(s_scroll == 0)
            {
                menu_ble_detector_enter_gatt_();
            }
            else if(s_scroll == 1)
            {
                menu_ble_detector_enter_alias_();
            }
        }
        return;
    }

    if((s_screen >= MENU_BLE_DETECTOR_SCREEN_GATT_WAIT) && (s_record_storage != NULL))
    {
        if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_WAIT)
        {
            if(event.button == BUTTON_B)
            {
                s_gatt_recon->stop_requested = true;
            }
            return;
        }
        if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES)
        {
            const int count = s_gatt_recon->service_count;
            if(event.button == BUTTON_B)
            {
                s_gatt_recon->stop_requested = true;
            }
            else if((event.button == BUTTON_UP) && (s_selected_index > 0))
            {
                --s_selected_index;
                menu_ble_detector_normalize_selection_(count);
                menu_ble_detector_reset_text_scroll_();
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_DOWN) && (s_selected_index < (count - 1)))
            {
                ++s_selected_index;
                menu_ble_detector_normalize_selection_(count);
                menu_ble_detector_reset_text_scroll_();
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_A) && (count > 0))
            {
                s_gatt_recon->selected_service = (uint8_t)s_selected_index;
                s_selected_index = 0;
                s_scroll = 0;
                menu_ble_detector_reset_text_scroll_();
                s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_CHARS;
                s_input_dirty = true;
            }
            return;
        }
        if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_CHARS)
        {
            int count = 0;
            for(uint8_t i = 0U; i < s_gatt_recon->char_count; ++i)
            {
                if(s_gatt_recon->chars[i].service_index == s_gatt_recon->selected_service)
                {
                    ++count;
                }
            }
            if(event.button == BUTTON_B)
            {
                s_selected_index = s_gatt_recon->selected_service;
                menu_ble_detector_normalize_selection_(s_gatt_recon->service_count);
                menu_ble_detector_reset_text_scroll_();
                s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_SERVICES;
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_UP) && (s_selected_index > 0))
            {
                --s_selected_index;
                menu_ble_detector_normalize_selection_(count);
                menu_ble_detector_reset_text_scroll_();
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_DOWN) && (s_selected_index < (count - 1)))
            {
                ++s_selected_index;
                menu_ble_detector_normalize_selection_(count);
                menu_ble_detector_reset_text_scroll_();
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_A) && (count > 0))
            {
                const int index = menu_ble_detector_gatt_char_at_selection_();
                if(index >= 0)
                {
                    s_gatt_recon->selected_char = (uint8_t)index;
                    s_gatt_recon->read_ready = false;
                    s_gatt_recon->value_hex[0] = '\0';
                    s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_VALUE;
                    s_input_dirty = true;
                }
            }
            return;
        }
        if(s_screen == MENU_BLE_DETECTOR_SCREEN_GATT_VALUE)
        {
            const uint8_t index = s_gatt_recon->selected_char;
            if(event.button == BUTTON_B)
            {
                s_screen = MENU_BLE_DETECTOR_SCREEN_GATT_CHARS;
                s_input_dirty = true;
            }
            else if((event.button == BUTTON_A) && (index < s_gatt_recon->char_count) &&
                    ((s_gatt_recon->chars[index].properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0U))
            {
                s_gatt_recon->read_ready = false;
                (void)snprintf(s_gatt_recon->value_hex, sizeof(s_gatt_recon->value_hex), "Reading...");
                (void)poom_ble_gatt_client_read_char(s_gatt_recon->chars[index].handle);
                s_input_dirty = true;
            }
            return;
        }
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
