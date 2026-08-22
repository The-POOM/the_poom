// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_tracker.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_ble_tracker.h"
#include "poom_secrets_store.h"
#include "poom_sbus.h"
#include "poom_ui_keyboard.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_TRACKER_MAX_ITEMS (12)
#define MENU_TRACKER_MAC_LEN (6U)
#define MENU_TRACKER_NAME_BUF_LEN (24)
#define MENU_TRACKER_SCAN_REFRESH_MS (1200U)

#define VISIBLE_ROWS (4)
#define LIST_Y0 (14)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

static poom_ble_tracker_profile_t* tracker_list = NULL;
static char (*name_buf)[MENU_TRACKER_NAME_BUF_LEN] = NULL;

static int s_tracker_count = 0;
static int selected_index = 0;
static int s_scroll = 0;
static bool showing_detail = false;
static const poom_ble_tracker_profile_t *current_tracker = NULL;
static int s_detail_index = -1;

static bool s_secrets_ready = false;

static bool s_editing_alias = false;
static poom_ui_keyboard_t s_alias_keyboard;
static char s_alias_edit_buf[MENU_TRACKER_NAME_BUF_LEN] = {0};

static bool tracker_buttons_subscribed = false;
static bool s_tracker_menu_active = false;
static bool s_tracker_exit_requested = false;
static bool s_tracker_scan_dirty = false;
static bool s_tracker_input_dirty = false;
static TaskHandle_t s_tracker_ui_task = NULL;

static void on_button_tracker(const poom_sbus_msg_t* msg, void* user);
static void tracker_request_render_from_scan_(void);
static void tracker_request_render_from_input_(void);
static void tracker_ui_task_(void* arg);
static int tracker_find_index_by_mac_(const uint8_t mac_address[MENU_TRACKER_MAC_LEN]);
static void tracker_alias_record_id_(char *out_id, size_t out_id_len, const poom_ble_tracker_profile_t *tracker);
static bool tracker_alias_load_(const poom_ble_tracker_profile_t *tracker, char *out_alias, size_t out_alias_len);
static void tracker_alias_save_(const poom_ble_tracker_profile_t *tracker, const char *alias);
static void tracker_enter_alias_edit_(const poom_ble_tracker_profile_t *t);
static bool tracker_alloc_buffers_(void);
static void tracker_free_buffers_(void);
static void tracker_draw_start_error_(const char* line1, const char* line2);

/**
 * @brief Internal helper for `tracker_alloc_buffers`.
 *
 * @return bool
 */
static bool tracker_alloc_buffers_(void)
{
    tracker_free_buffers_();

    tracker_list = calloc(MENU_TRACKER_MAX_ITEMS, sizeof(*tracker_list));
    if (tracker_list == NULL)
    {
        return false;
    }

    name_buf = calloc(MENU_TRACKER_MAX_ITEMS, sizeof(*name_buf));
    if (name_buf == NULL)
    {
        tracker_free_buffers_();
        return false;
    }

    return true;
}

/**
 * @brief Internal helper for `tracker_free_buffers`.
 *
 * @return void
 */
static void tracker_free_buffers_(void)
{
    free(name_buf);
    name_buf = NULL;

    free(tracker_list);
    tracker_list = NULL;
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] line1 Parameter passed to the helper.
 * @param[in] line2 Parameter passed to the helper.
 * @return void
 */
static void tracker_draw_start_error_(const char* line1, const char* line2)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("BLE TRACKERS"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(10, 24);
    (void)poom_arduboy_print((line1 != NULL) ? line1 : "");
    poom_arduboy_set_cursor(10, 34);
    (void)poom_arduboy_print((line2 != NULL) ? line2 : "");

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void tracker_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_tracker_menu_active = false;
    s_tracker_exit_requested = false;
    s_tracker_scan_dirty = false;
    s_tracker_input_dirty = false;
    s_editing_alias = false;
    s_secrets_ready = false;
    showing_detail = false;
    current_tracker = NULL;
    s_detail_index = -1;

    poom_ble_tracker_stop();
    poom_ble_tracker_register_scan_callback(NULL);
    tracker_free_buffers_();

    if (s_tracker_ui_task != NULL)
    {
        if (s_tracker_ui_task != current_task)
        {
            TaskHandle_t ui_task = s_tracker_ui_task;
            s_tracker_ui_task = NULL;
            vTaskDelete(ui_task);
        }
        else
        {
            s_tracker_ui_task = NULL;
        }
    }

    if (tracker_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", on_button_tracker, "menu_tracker");
        tracker_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void tracker_request_render_from_scan_(void)
{
    s_tracker_scan_dirty = true;
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void tracker_request_render_from_input_(void)
{
    s_tracker_input_dirty = true;
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void tracker_draw_waiting_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("BLE TRACKERS"));

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(22, 28);
    (void)poom_arduboy_print(F("Scanning..."));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void tracker_render_list_(void)
{
    if (selected_index < 0)
    {
        selected_index = 0;
    }
    if (selected_index > (s_tracker_count - 1))
    {
        selected_index = (s_tracker_count - 1);
    }

    if (selected_index < s_scroll)
    {
        s_scroll = selected_index;
    }
    if (selected_index >= (s_scroll + VISIBLE_ROWS))
    {
        s_scroll = selected_index - VISIBLE_ROWS + 1;
    }

    int max_scroll = s_tracker_count - VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_scroll < 0)
    {
        s_scroll = 0;
    }
    if (s_scroll > max_scroll)
    {
        s_scroll = max_scroll;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("BLE TRACKERS"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:DETAIL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_scroll + row;
        if (idx >= s_tracker_count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);

        char name_line[16];
        (void)snprintf(name_line, sizeof(name_line), "%.14s", name_buf[idx]);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(name_line);

        char rssi_small[10];
        (void)snprintf(rssi_small, sizeof(rssi_small), "%ddBm", tracker_list[idx].rssi);
        poom_arduboy_set_cursor(83, y);
        (void)poom_arduboy_print(rssi_small);

        if (idx == selected_index)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    if (s_scroll > 0)
    {
        poom_arduboy_fill_triangle(124, (int16_t)(LIST_Y0 - 3), 120, (int16_t)(LIST_Y0 + 1), 127, (int16_t)(LIST_Y0 + 1), WHITE);
    }
    if (s_scroll < max_scroll)
    {
        const int16_t y = (int16_t)(LIST_Y0 + (VISIBLE_ROWS - 1) * ROW_STEP + 6);
        poom_arduboy_fill_triangle(120, y, 127, y, 124, (int16_t)(y + 4), WHITE);
    }

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @param[in] t Parameter passed to the helper.
 * @return void
 */
static void tracker_render_detail_(const poom_ble_tracker_profile_t* t)
{
    if ((t == NULL) || (!t->is_tracker))
    {
        tracker_draw_waiting_();
        return;
    }

    char line0[22];
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];

    int idx = (s_detail_index >= 0) ? s_detail_index : tracker_find_index_by_mac_(t->mac_address);
    const char* name =
        ((idx >= 0) && (idx < s_tracker_count) && (name_buf[idx][0] != '\0')) ? name_buf[idx] :
        ((t->name && t->name[0]) ? t->name : "Unknown");
    const char* vendor = (t->vendor && t->vendor[0]) ? t->vendor : "Tracker";

    (void)snprintf(line0, sizeof(line0), "%.21s", name);
    (void)snprintf(line1, sizeof(line1), "%.21s", vendor);
    (void)snprintf(line2, sizeof(line2), "d=%.2fm", (double)t->distance_m);
    (void)snprintf(line3, sizeof(line3), "RSSI=%ddBm", t->rssi);
    (void)snprintf(line4,
                   sizeof(line4),
                   "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                   t->mac_address[5],
                   t->mac_address[4],
                   t->mac_address[3],
                   t->mac_address[2],
                   t->mac_address[1],
                   t->mac_address[0]);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(36, 2);
    (void)poom_arduboy_print(F("DETAIL"));

    char idx_buf[24];
    (void)snprintf(idx_buf, sizeof(idx_buf), "%d/%d", selected_index + 1, s_tracker_count);
    poom_arduboy_set_cursor(92, 2);
    (void)poom_arduboy_print(idx_buf);

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(0, 14);
    (void)poom_arduboy_print(line0);

    poom_arduboy_set_cursor(0, 22);
    (void)poom_arduboy_print(line1);

    poom_arduboy_set_cursor(0, 30);
    (void)poom_arduboy_print(line2);

    poom_arduboy_set_cursor(0, 38);
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 46);
    (void)poom_arduboy_print(line4);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:NAME"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `tracker_alias_hash_bytes`.
 *
 * @param[in] hash Parameter passed to the helper.
 * @param[in] data Parameter passed to the helper.
 * @param[in] data_len Parameter passed to the helper.
 * @return uint64_t
 */
static uint64_t tracker_alias_hash_bytes_(uint64_t hash, const uint8_t *data, size_t data_len)
{
    if ((data == NULL) || (data_len == 0U))
    {
        return hash;
    }

    for (size_t i = 0; i < data_len; ++i)
    {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

/**
 * @brief Internal helper for `tracker_alias_record_id`.
 *
 * @param[in] out_id Parameter passed to the helper.
 * @param[in] out_id_len Parameter passed to the helper.
 * @param[in] tracker Parameter passed to the helper.
 * @return void
 */
static void tracker_alias_record_id_(char *out_id, size_t out_id_len, const poom_ble_tracker_profile_t *tracker)
{
    uint64_t hash = 1469598103934665603ULL;
    uint8_t normalized_adv[sizeof(tracker->adv_data)] = {0};
    size_t normalized_adv_len = 0U;

    if ((out_id == NULL) || (out_id_len == 0U) || (tracker == NULL))
    {
        return;
    }

    hash = tracker_alias_hash_bytes_(hash, (const uint8_t *)"ble_trk_alias_v2", sizeof("ble_trk_alias_v2") - 1U);

    if (tracker->name != NULL)
    {
        hash = tracker_alias_hash_bytes_(hash, (const uint8_t *)tracker->name, strlen(tracker->name));
    }

    if (tracker->vendor != NULL)
    {
        hash = tracker_alias_hash_bytes_(hash, (const uint8_t *)tracker->vendor, strlen(tracker->vendor));
    }

    normalized_adv_len = tracker->adv_data_length;
    if (normalized_adv_len > sizeof(normalized_adv))
    {
        normalized_adv_len = sizeof(normalized_adv);
    }

    if (normalized_adv_len > 0U)
    {
        (void)memcpy(normalized_adv, tracker->adv_data, normalized_adv_len);

        if ((tracker->vendor != NULL) && (strcmp(tracker->vendor, "Apple") == 0))
        {
            if (normalized_adv_len > 6U)
            {
                normalized_adv[6] = 0U;
            }
            normalized_adv[normalized_adv_len - 1U] = 0U;
        }

        hash = tracker_alias_hash_bytes_(hash, normalized_adv, normalized_adv_len);
    }

    (void)snprintf(out_id, out_id_len, "ble_trk_alias/%016llX", (unsigned long long)hash);
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @param[in] tracker Parameter passed to the helper.
 * @param[in] out_alias Parameter passed to the helper.
 * @param[in] out_alias_len Parameter passed to the helper.
 * @return bool
 */
static bool tracker_alias_load_(const poom_ble_tracker_profile_t *tracker, char *out_alias, size_t out_alias_len)
{
    char id[64];
    void *blob = NULL;
    size_t blob_len = 0U;

    if ((tracker == NULL) || (out_alias == NULL) || (out_alias_len == 0U))
    {
        return false;
    }

    out_alias[0] = '\0';

    if (!s_secrets_ready)
    {
        return false;
    }

    tracker_alias_record_id_(id, sizeof(id), tracker);

    if (poom_secrets_get_record_blob_alloc(id, &blob, &blob_len) != ESP_OK)
    {
        return false;
    }

    if ((blob == NULL) || (blob_len == 0U))
    {
        free(blob);
        return false;
    }

    size_t copy_len = blob_len;
    if (copy_len >= out_alias_len)
    {
        copy_len = out_alias_len - 1U;
    }

    (void)memcpy(out_alias, blob, copy_len);
    out_alias[copy_len] = '\0';
    free(blob);
    return (out_alias[0] != '\0');
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @param[in] tracker Parameter passed to the helper.
 * @param[in] alias Parameter passed to the helper.
 * @return void
 */
static void tracker_alias_save_(const poom_ble_tracker_profile_t *tracker, const char *alias)
{
    char id[64];
    size_t len;

    if ((tracker == NULL) || (alias == NULL) || (!s_secrets_ready))
    {
        return;
    }

    tracker_alias_record_id_(id, sizeof(id), tracker);

    if (alias[0] == '\0')
    {
        (void)poom_secrets_erase_record(id);
        return;
    }

    len = strnlen(alias, MENU_TRACKER_NAME_BUF_LEN - 1U);
    (void)poom_secrets_set_record_blob(id, alias, len + 1U);
}

/**
 * @brief Internal helper for `tracker_enter_alias_edit`.
 *
 * @param[in] t Parameter passed to the helper.
 * @return void
 */
static void tracker_enter_alias_edit_(const poom_ble_tracker_profile_t *t)
{
    if ((t == NULL) || (!t->is_tracker))
    {
        return;
    }

    (void)memset(s_alias_edit_buf, 0, sizeof(s_alias_edit_buf));
    (void)tracker_alias_load_(t, s_alias_edit_buf, sizeof(s_alias_edit_buf));

    poom_ui_keyboard_init(&s_alias_keyboard, s_alias_edit_buf, sizeof(s_alias_edit_buf), (t->name && t->name[0]) ? t->name : "Tracker");
    s_editing_alias = true;
    poom_ui_keyboard_draw(&s_alias_keyboard);
}

/**
 * @brief Internal helper for `tracker_find_index_by_mac`.
 *
 * @param[in] mac_address Parameter passed to the helper.
 * @return int
 */
static int tracker_find_index_by_mac_(const uint8_t mac_address[MENU_TRACKER_MAC_LEN])
{
    for (int i = 0; i < s_tracker_count; ++i)
    {
        if (memcmp(tracker_list[i].mac_address, mac_address, MENU_TRACKER_MAC_LEN) == 0)
        {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Internal helper for `tracker_update_item_strings`.
 *
 * @param[in] index Parameter passed to the helper.
 * @return void
 */
static void tracker_update_item_strings_(int index)
{
    char alias[MENU_TRACKER_NAME_BUF_LEN] = {0};

    if ((tracker_list == NULL) || (name_buf == NULL) || (index < 0) || (index >= s_tracker_count))
    {
        return;
    }

    const poom_ble_tracker_profile_t* tracker = &tracker_list[index];

    if (tracker_alias_load_(tracker, alias, sizeof(alias)))
    {
        (void)snprintf(name_buf[index], sizeof(name_buf[index]), "%.23s", alias);
        return;
    }

    (void)snprintf(name_buf[index],
                   sizeof(name_buf[index]),
                   "%.23s",
                   (tracker->name && tracker->name[0]) ? tracker->name : "(unknown)");
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void tracker_ui_task_(void* arg)
{
    TickType_t last_scan_render_tick = 0;

    (void)arg;

    while (s_tracker_menu_active)
    {
        if (s_tracker_exit_requested)
        {
            tracker_exit_();
            break;
        }

        if (s_tracker_input_dirty)
        {
            s_tracker_input_dirty = false;

            if (s_editing_alias)
            {
                poom_ui_keyboard_draw(&s_alias_keyboard);
            }
            else if (showing_detail && (current_tracker != NULL))
            {
                tracker_render_detail_(current_tracker);
            }
            else if (s_tracker_count > 0)
            {
                tracker_render_list_();
            }
            else
            {
                tracker_draw_waiting_();
            }

            last_scan_render_tick = xTaskGetTickCount();
        }
        else if (s_tracker_scan_dirty &&
                 ((last_scan_render_tick == 0) ||
                  ((xTaskGetTickCount() - last_scan_render_tick) >= pdMS_TO_TICKS(MENU_TRACKER_SCAN_REFRESH_MS))))
        {
            s_tracker_scan_dirty = false;

            if (s_editing_alias)
            {
                poom_ui_keyboard_draw(&s_alias_keyboard);
            }
            else if (showing_detail && (current_tracker != NULL))
            {
                tracker_render_detail_(current_tracker);
            }
            else if (s_tracker_count > 0)
            {
                tracker_render_list_();
            }
            else
            {
                tracker_draw_waiting_();
            }

            last_scan_render_tick = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_tracker_ui_task != NULL)
    {
        s_tracker_ui_task = NULL;
    }
    vTaskDelete(NULL);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void on_button_tracker(const poom_sbus_msg_t* msg, void* user)
{
    (void)user;

    if (!msg || msg->len < sizeof(button_event_msg_t))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));
    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (showing_detail)
    {
        if (s_editing_alias)
        {
            if (ev.button == BUTTON_B)
            {
                s_editing_alias = false;
                tracker_request_render_from_input_();
                return;
            }

            poom_ui_keyboard_action_t action = poom_ui_keyboard_handle_button(&s_alias_keyboard, ev.button);
            if (action == POOM_UI_KEYBOARD_ACTION_ACCEPT)
            {
                if ((current_tracker != NULL) && (s_detail_index >= 0) && (s_detail_index < s_tracker_count))
                {
                    tracker_alias_save_(current_tracker, s_alias_edit_buf);
                    tracker_update_item_strings_(s_detail_index);
                }

                s_editing_alias = false;
                tracker_request_render_from_input_();
                return;
            }

            poom_ui_keyboard_draw(&s_alias_keyboard);
            return;
        }

        if (ev.button == BUTTON_B)
        {
            showing_detail = false;
            current_tracker = NULL;
            s_detail_index = -1;
            tracker_request_render_from_input_();
        }
        else if (ev.button == BUTTON_A)
        {
            tracker_enter_alias_edit_(current_tracker);
        }
        return;
    }

    if (s_tracker_count == 0)
    {
        if (ev.button == BUTTON_B)
        {
            s_tracker_exit_requested = true;
        }
        return;
    }

    if (ev.button == BUTTON_B)
    {
        s_tracker_exit_requested = true;
        return;
    }
    else if ((ev.button == BUTTON_UP) && (selected_index > 0))
    {
        selected_index--;
    }
    else if ((ev.button == BUTTON_DOWN) && (selected_index < (s_tracker_count - 1)))
    {
        selected_index++;
    }
    else if (ev.button == BUTTON_A)
    {
        current_tracker = &tracker_list[selected_index];
        s_detail_index = selected_index;
        showing_detail = true;
        tracker_request_render_from_input_();
        return;
    }

    tracker_request_render_from_input_();
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] record Parameter passed to the helper.
 * @return void
 */
static void module_handle_trackers(poom_ble_tracker_profile_t record)
{
    if (!record.is_tracker || (tracker_list == NULL) || (name_buf == NULL))
    {
        return;
    }

    int index = tracker_find_index_by_mac_(record.mac_address);
    if (index < 0)
    {
        if (s_tracker_count >= MENU_TRACKER_MAX_ITEMS)
        {
            return;
        }

        index = s_tracker_count;
        tracker_list[index] = record;
        s_tracker_count++;

        tracker_update_item_strings_(index);
    }
    else
    {
        tracker_list[index] = record;
    }

    if (selected_index >= s_tracker_count)
    {
        selected_index = s_tracker_count - 1;
    }

    if (showing_detail)
    {
        if ((current_tracker != NULL) &&
            (memcmp(current_tracker->mac_address, record.mac_address, MENU_TRACKER_MAC_LEN) == 0))
        {
            current_tracker = &tracker_list[index];
            s_detail_index = index;
            tracker_request_render_from_scan_();
        }
        return;
    }

    tracker_request_render_from_scan_();
}

void app_tracker_menu(void)
{
    s_tracker_count = 0;
    selected_index = 0;
    s_scroll = 0;
    showing_detail = false;
    current_tracker = NULL;
    s_detail_index = -1;
    s_editing_alias = false;

    s_tracker_menu_active = true;
    s_tracker_exit_requested = false;
    s_tracker_scan_dirty = false;
    s_tracker_input_dirty = true;

    if (!tracker_alloc_buffers_())
    {
        s_tracker_menu_active = false;
        s_tracker_exit_requested = false;
        s_tracker_scan_dirty = false;
        s_tracker_input_dirty = false;
        tracker_draw_start_error_("No memory", "Try again");
        vTaskDelay(pdMS_TO_TICKS(1200));
        const uint8_t token = 1;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    if (!tracker_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", on_button_tracker, "menu_tracker");
        tracker_buttons_subscribed = true;
    }

    s_secrets_ready = (poom_secrets_init() == ESP_OK);

    poom_ble_tracker_register_scan_callback(module_handle_trackers);

    if (!poom_ble_tracker_is_active())
    {
        poom_ble_tracker_start();
    }

    if (s_tracker_ui_task == NULL)
    {
        if (xTaskCreate(tracker_ui_task_,
                        "menu_tracker_ui",
                        4096,
                        NULL,
                        5,
                        &s_tracker_ui_task) != pdPASS)
        {
            s_tracker_ui_task = NULL;
            tracker_draw_start_error_("Task create", "failed");
            vTaskDelay(pdMS_TO_TICKS(1200));
            tracker_exit_();
        }
    }
}
