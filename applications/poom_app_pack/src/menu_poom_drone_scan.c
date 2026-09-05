// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_drone_scan.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_drone.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_DRONE_SCAN_REFRESH_MS (200U)
#define MENU_DRONE_SCAN_STACK (4096U)
#define MENU_DRONE_SCAN_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (42)
#define LIST_Y0 (14)
#define ROW_STEP (8)
#define ROW_HILITE_H (8)
#define VISIBLE_ROWS (5)

#define MENU_DRONE_SCAN_MAX_DEVICES (8U)
#define MENU_DRONE_SCAN_DETAIL_MAX_LINES (20)

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
#endif
#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif
#ifndef BTN_UP
#define BTN_UP (4U)
#endif
#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_DRONE_SCAN_SCREEN_LIST = 0,
    MENU_DRONE_SCAN_SCREEN_DETAIL,
} menu_drone_scan_screen_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
} menu_drone_scan_btn_msg_t;

typedef struct
{
    bool used;
    uint8_t mac[6];
    uint32_t first_seen_seq;
    uint32_t last_seen_ms;
    poom_drone_uav_data_t uav;
} menu_drone_scan_device_t;

typedef struct
{
    uint8_t mac[6];
    uint32_t first_seen_seq;
    uint32_t last_seen_ms;
    int8_t rssi;
    uint16_t channel;
    char uav_id[ODID_ID_SIZE + 1];
} menu_drone_scan_snapshot_t;

static bool s_menu_active = false;
static bool s_buttons_subscribed = false;
static TaskHandle_t s_menu_task = NULL;
static QueueHandle_t s_btn_q = NULL;
static char s_sbus_user[] = "menu_poom_drone_scan";

static char s_status[18] = "START";
static menu_drone_scan_screen_t s_screen = MENU_DRONE_SCAN_SCREEN_LIST;

static portMUX_TYPE s_devs_mux = portMUX_INITIALIZER_UNLOCKED;
static menu_drone_scan_device_t *s_devs = NULL;
static uint32_t s_report_count = 0;
static uint32_t s_first_seen_seq = 0;

static bool s_has_selected = false;
static uint8_t s_selected_mac[6] = {0};
static int s_selected_index = 0;
static int s_scroll = 0;

static bool s_has_detail = false;
static uint8_t s_detail_mac[6] = {0};
static int s_detail_scroll = 0;
static bool s_ble_scan_enabled = false;

static void menu_btn_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_task_(void *arg);
static void menu_exit_(void);
static void menu_report_cb_(const poom_drone_uav_data_t *report, void *user_ctx);

/**
 * @brief Resets the tracked device state while the caller holds `s_devs_mux`.
 *
 * @return void
 */
static void menu_reset_devices_locked_(void)
{
    if (s_devs != NULL)
    {
        memset(s_devs, 0, sizeof(*s_devs) * MENU_DRONE_SCAN_MAX_DEVICES);
    }

    s_report_count = 0;
    s_first_seen_seq = 0;
    s_has_selected = false;
    memset(s_selected_mac, 0, sizeof(s_selected_mac));
    s_has_detail = false;
    memset(s_detail_mac, 0, sizeof(s_detail_mac));
}

/**
 * @brief Allocates the runtime device table if needed.
 *
 * @return bool
 */
static bool menu_alloc_devices_(void)
{
    if (s_devs != NULL)
    {
        return true;
    }

    s_devs = calloc(MENU_DRONE_SCAN_MAX_DEVICES, sizeof(*s_devs));
    return s_devs != NULL;
}

/**
 * @brief Frees the runtime device table and clears related state.
 *
 * @return void
 */
static void menu_free_devices_(void)
{
    menu_drone_scan_device_t *devs = NULL;

    portENTER_CRITICAL(&s_devs_mux);
    devs = s_devs;
    s_devs = NULL;
    menu_reset_devices_locked_();
    portEXIT_CRITICAL(&s_devs_mux);

    free(devs);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_restart_scanner_(void)
{
    if (s_devs == NULL)
    {
        (void)snprintf(s_status, sizeof(s_status), "NO MEM");
        return;
    }

    poom_drone_register_report_cb(NULL, NULL);
    (void)poom_drone_stop();

    portENTER_CRITICAL(&s_devs_mux);
    menu_reset_devices_locked_();
    portEXIT_CRITICAL(&s_devs_mux);

    poom_drone_register_report_cb(menu_report_cb_, NULL);

    poom_drone_config_t cfg;
    poom_drone_config_default(&cfg);
    cfg.enable_cli_print = false;
    cfg.enable_pcap_to_sd = false;
    cfg.scan_mask = POOM_DRONE_SCAN_WIFI | (s_ble_scan_enabled ? POOM_DRONE_SCAN_BLE : 0U);

    const esp_err_t err = poom_drone_start_ex(&cfg);
    if (err != ESP_OK)
    {
        (void)snprintf(s_status, sizeof(s_status), "ERR %d", (int)err);
    }
    else
    {
        (void)snprintf(s_status, sizeof(s_status), s_ble_scan_enabled ? "W+B" : "WIFI");
    }
}

/**
 * @brief Internal helper for `menu_now_ms`.
 *
 * @return uint32_t
 */
static uint32_t menu_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * @brief Internal helper for `mac_eq`.
 *
 * @param[in] a Parameter passed to the helper.
 * @param[in] b Parameter passed to the helper.
 * @return bool
 */
static bool mac_eq_(const uint8_t a[6], const uint8_t b[6])
{
    return (a != NULL) && (b != NULL) && (memcmp(a, b, 6) == 0);
}

/**
 * @brief Compares internal items for sorting.
 *
 * @param[in] a Parameter passed to the helper.
 * @param[in] b Parameter passed to the helper.
 * @return int
 */
static int snapshot_cmp_first_seen_asc_(const void *a, const void *b)
{
    const menu_drone_scan_snapshot_t *sa = (const menu_drone_scan_snapshot_t *)a;
    const menu_drone_scan_snapshot_t *sb = (const menu_drone_scan_snapshot_t *)b;
    if (sa->first_seen_seq == sb->first_seen_seq)
    {
        return 0;
    }
    return (sa->first_seen_seq < sb->first_seen_seq) ? -1 : 1;
}

/**
 * @brief Sorts internal items for this menu module.
 *
 * @param[in] list Parameter passed to the helper.
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void snapshot_sort_(menu_drone_scan_snapshot_t *list, int count)
{
    if ((list == NULL) || (count <= 1))
    {
        return;
    }

    for (int i = 0; i < (count - 1); i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (snapshot_cmp_first_seen_asc_(&list[i], &list[j]) > 0)
            {
                menu_drone_scan_snapshot_t tmp = list[i];
                list[i] = list[j];
                list[j] = tmp;
            }
        }
    }
}

/**
 * @brief Internal helper for `snapshot_find_mac`.
 *
 * @param[in] list Parameter passed to the helper.
 * @param[in] count Parameter passed to the helper.
 * @param[in] mac Parameter passed to the helper.
 * @return int
 */
static int snapshot_find_mac_(const menu_drone_scan_snapshot_t *list, int count, const uint8_t mac[6])
{
    if ((list == NULL) || (mac == NULL) || (count <= 0))
    {
        return -1;
    }

    for (int i = 0; i < count; i++)
    {
        if (memcmp(list[i].mac, mac, 6) == 0)
        {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Internal helper for `snapshot_build`.
 *
 * @param[in] out_list Parameter passed to the helper.
 * @param[in] cap Parameter passed to the helper.
 * @return int
 */
static int snapshot_build_(menu_drone_scan_snapshot_t *out_list, int cap)
{
    if ((out_list == NULL) || (cap <= 0))
    {
        return 0;
    }

    int out = 0;
    portENTER_CRITICAL(&s_devs_mux);
    if (s_devs == NULL)
    {
        portEXIT_CRITICAL(&s_devs_mux);
        return 0;
    }
    for (uint32_t i = 0; i < MENU_DRONE_SCAN_MAX_DEVICES; i++)
    {
        if (!s_devs[i].used)
        {
            continue;
        }
        if (out >= cap)
        {
            break;
        }

        menu_drone_scan_snapshot_t *dst = &out_list[out++];
        memset(dst, 0, sizeof(*dst));
        memcpy(dst->mac, s_devs[i].mac, sizeof(dst->mac));
        dst->first_seen_seq = s_devs[i].first_seen_seq;
        dst->last_seen_ms = s_devs[i].last_seen_ms;
        dst->rssi = s_devs[i].uav.rssi;
        dst->channel = s_devs[i].uav.channel;
        (void)snprintf(dst->uav_id, sizeof(dst->uav_id), "%s", s_devs[i].uav.uav_id);
    }
    portEXIT_CRITICAL(&s_devs_mux);

    snapshot_sort_(out_list, out);
    return out;
}

/**
 * @brief Internal helper for `dev_is_pinned`.
 *
 * @param[in] mac Parameter passed to the helper.
 * @return bool
 */
static bool dev_is_pinned_(const uint8_t mac[6])
{
    if (mac == NULL)
    {
        return false;
    }

    if (s_has_detail && mac_eq_(mac, s_detail_mac))
    {
        return true;
    }
    if (!s_has_detail && s_has_selected && mac_eq_(mac, s_selected_mac))
    {
        return true;
    }
    return false;
}

/**
 * @brief Internal helper for dev_get_or_alloc_locked.
 *
 * @param[in] mac Parameter passed to the helper.
 * @return menu_drone_scan_device_t *
 */
static menu_drone_scan_device_t *dev_get_or_alloc_locked_(const uint8_t mac[6])
{
    if ((mac == NULL) || (s_devs == NULL))
    {
        return NULL;
    }

    for (uint32_t i = 0; i < MENU_DRONE_SCAN_MAX_DEVICES; i++)
    {
        if (s_devs[i].used && (memcmp(s_devs[i].mac, mac, 6) == 0))
        {
            return &s_devs[i];
        }
    }

    for (uint32_t i = 0; i < MENU_DRONE_SCAN_MAX_DEVICES; i++)
    {
        if (!s_devs[i].used)
        {
            memset(&s_devs[i], 0, sizeof(s_devs[i]));
            s_devs[i].used = true;
            memcpy(s_devs[i].mac, mac, 6);
            s_devs[i].first_seen_seq = ++s_first_seen_seq;
            return &s_devs[i];
        }
    }

    uint32_t oldest_ts = UINT32_MAX;
    int oldest_idx = -1;
    for (uint32_t i = 0; i < MENU_DRONE_SCAN_MAX_DEVICES; i++)
    {
        if (dev_is_pinned_(s_devs[i].mac))
        {
            continue;
        }
        if (s_devs[i].last_seen_ms < oldest_ts)
        {
            oldest_ts = s_devs[i].last_seen_ms;
            oldest_idx = (int)i;
        }
    }
    if (oldest_idx < 0)
    {
        oldest_idx = 0;
    }

    memset(&s_devs[oldest_idx], 0, sizeof(s_devs[oldest_idx]));
    s_devs[oldest_idx].used = true;
    memcpy(s_devs[oldest_idx].mac, mac, 6);
    s_devs[oldest_idx].first_seen_seq = ++s_first_seen_seq;
    return &s_devs[oldest_idx];
}

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] report Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_report_cb_(const poom_drone_uav_data_t *report, void *user_ctx)
{
    (void)user_ctx;
    if (report == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_devs_mux);
    menu_drone_scan_device_t *dev = dev_get_or_alloc_locked_(report->mac);
    if (dev != NULL)
    {
        s_report_count++;
        dev->last_seen_ms = menu_now_ms_();
        memcpy(&dev->uav, report, sizeof(dev->uav));
        memcpy(dev->mac, report->mac, sizeof(dev->mac));

        if (!s_has_selected && !s_has_detail)
        {
            s_has_selected = true;
            memcpy(s_selected_mac, report->mac, sizeof(s_selected_mac));
        }
    }
    portEXIT_CRITICAL(&s_devs_mux);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_draw_frame_(const char *title)
{

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(20, 2);
    (void)poom_arduboy_print(title ? title : "DRONE SCAN");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_set_cursor(92, 2);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void list_adjust_scroll_(int count)
{
    if (count <= 0)
    {
        s_selected_index = 0;
        s_scroll = 0;
        return;
    }

    if (s_selected_index < 0)
    {
        s_selected_index = 0;
    }
    if (s_selected_index > (count - 1))
    {
        s_selected_index = count - 1;
    }

    if (s_selected_index < s_scroll)
    {
        s_scroll = s_selected_index;
    }
    if (s_selected_index >= (s_scroll + VISIBLE_ROWS))
    {
        s_scroll = s_selected_index - VISIBLE_ROWS + 1;
    }

    int max_scroll = count - VISIBLE_ROWS;
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
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_list_(void)
{
    menu_drone_scan_snapshot_t snaps[MENU_DRONE_SCAN_MAX_DEVICES];
    const int count = snapshot_build_(snaps, (int)MENU_DRONE_SCAN_MAX_DEVICES);

    if (count <= 0)
    {
        menu_draw_frame_("DRONE SCAN");

        poom_arduboy_set_cursor(20, 30);
        (void)poom_arduboy_print(F("Scanning..."));

        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:EXIT"));
        poom_arduboy_display();
        return;
    }

    uint32_t reports = 0;
    portENTER_CRITICAL(&s_devs_mux);
    reports = s_report_count;
    portEXIT_CRITICAL(&s_devs_mux);
    if (strncmp(s_status, "ERR", 3) != 0)
    {
        (void)snprintf(s_status, sizeof(s_status), "D%d R%lu", count, (unsigned long)reports);
    }

    bool has_selected = false;
    uint8_t selected_mac[6] = {0};
    portENTER_CRITICAL(&s_devs_mux);
    has_selected = s_has_selected;
    memcpy(selected_mac, s_selected_mac, sizeof(selected_mac));
    portEXIT_CRITICAL(&s_devs_mux);

    if (has_selected)
    {
        const int idx = snapshot_find_mac_(snaps, count, selected_mac);
        if (idx >= 0)
        {
            s_selected_index = idx;
        }
        else
        {
            s_selected_index = 0;
            portENTER_CRITICAL(&s_devs_mux);
            s_has_selected = true;
            memcpy(s_selected_mac, snaps[0].mac, sizeof(s_selected_mac));
            portEXIT_CRITICAL(&s_devs_mux);
        }
    }
    else
    {
        s_selected_index = 0;
        portENTER_CRITICAL(&s_devs_mux);
        s_has_selected = true;
        memcpy(s_selected_mac, snaps[0].mac, sizeof(s_selected_mac));
        portEXIT_CRITICAL(&s_devs_mux);
    }

    list_adjust_scroll_(count);

    menu_draw_frame_("DRONE SCAN");

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_scroll + row;
        if (idx >= count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);

        char left[21] = {0};
        if (snaps[idx].uav_id[0] != '\0')
        {
            (void)snprintf(left, sizeof(left), "%.20s", snaps[idx].uav_id);
        }
        else
        {
            (void)snprintf(left, sizeof(left), "%02X:%02X:%02X",
                           snaps[idx].mac[3],
                           snaps[idx].mac[4],
                           snaps[idx].mac[5]);
        }

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(left);

        if (idx == s_selected_index)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:VIEW"));
    poom_arduboy_set_cursor(40, 56);
    (void)poom_arduboy_print(F("LR:WF/BL"));
    poom_arduboy_set_cursor(91, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `detail_build_lines`.
 *
 * @param[in] uav Parameter passed to the helper.
 * @param[in] cap Parameter passed to the helper.
 * @return int
 */
static int detail_build_lines_(const poom_drone_uav_data_t *uav, char lines[][22], int cap)
{
    if ((uav == NULL) || (lines == NULL) || (cap <= 0))
    {
        return 0;
    }

    int n = 0;

    (void)snprintf(lines[n++], 22,
                   "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                   uav->mac[0], uav->mac[1], uav->mac[2], uav->mac[3], uav->mac[4], uav->mac[5]);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "RSSI:%dd CH:%u", (int)uav->rssi, (unsigned)uav->channel);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "UAV:%.17s", (uav->uav_id[0] != '\0') ? uav->uav_id : "<none>");
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "OP:%.18s", (uav->op_id[0] != '\0') ? uav->op_id : "<none>");
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "LAT:%.5f", uav->lat_d);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "LON:%.5f", uav->long_d);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "ALT:%dm H:%dm", uav->altitude_msl, uav->height_agl);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "SPD:%dm/s HDG:%d", uav->speed, uav->heading);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "VSPD:%dm/s", uav->speed_vertical);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "B.LAT:%.5f", uav->base_lat_d);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "B.LON:%.5f", uav->base_long_d);
    if (n >= cap)
    {
        return n;
    }

    if (uav->description[0] != '\0')
    {
        (void)snprintf(lines[n++], 22, "DESC:%.16s", uav->description);
        if (n >= cap)
        {
            return n;
        }
    }

    (void)snprintf(lines[n++], 22, "STAT:%d TS:%d", uav->status, uav->timestamp);
    if (n >= cap)
    {
        return n;
    }

    (void)snprintf(lines[n++], 22, "AUTH:%u LEN:%u", (unsigned)uav->auth_type, (unsigned)uav->auth_length);
    if (n >= cap)
    {
        return n;
    }

    if (uav->auth_data[0] != '\0')
    {
        (void)snprintf(lines[n++], 22, "A:%.19s", uav->auth_data);
        if (n >= cap)
        {
            return n;
        }
    }

    return n;
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] line_count Parameter passed to the helper.
 * @return void
 */
static void detail_adjust_scroll_(int line_count)
{
    if (line_count <= VISIBLE_ROWS)
    {
        s_detail_scroll = 0;
        return;
    }

    int max_scroll = line_count - VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_detail_scroll < 0)
    {
        s_detail_scroll = 0;
    }
    if (s_detail_scroll > max_scroll)
    {
        s_detail_scroll = max_scroll;
    }
}

/**
 * @brief Copies internal data into the destination buffer.
 *
 * @param[in] mac Parameter passed to the helper.
 * @param[in] out_uav Parameter passed to the helper.
 * @return bool
 */
static bool dev_copy_uav_by_mac_(const uint8_t mac[6], poom_drone_uav_data_t *out_uav)
{
    if ((mac == NULL) || (out_uav == NULL))
    {
        return false;
    }

    bool ok = false;
    portENTER_CRITICAL(&s_devs_mux);
    if (s_devs != NULL)
    {
        for (uint32_t i = 0; i < MENU_DRONE_SCAN_MAX_DEVICES; i++)
        {
            if (s_devs[i].used && (memcmp(s_devs[i].mac, mac, 6) == 0))
            {
                memcpy(out_uav, &s_devs[i].uav, sizeof(*out_uav));
                ok = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_devs_mux);
    return ok;
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_detail_(void)
{
    if (!s_has_detail)
    {
        s_screen = MENU_DRONE_SCAN_SCREEN_LIST;
        return;
    }

    poom_drone_uav_data_t uav = {0};
    if (!dev_copy_uav_by_mac_(s_detail_mac, &uav))
    {
        s_screen = MENU_DRONE_SCAN_SCREEN_LIST;
        s_has_detail = false;
        return;
    }

    uint32_t reports = 0;
    portENTER_CRITICAL(&s_devs_mux);
    reports = s_report_count;
    portEXIT_CRITICAL(&s_devs_mux);
    if (strncmp(s_status, "ERR", 3) != 0)
    {
        (void)snprintf(s_status, sizeof(s_status), "D1 R%lu", (unsigned long)reports);
    }

    char lines[MENU_DRONE_SCAN_DETAIL_MAX_LINES][22];
    memset(lines, 0, sizeof(lines));
    int line_count = detail_build_lines_(&uav, lines, MENU_DRONE_SCAN_DETAIL_MAX_LINES);

    detail_adjust_scroll_(line_count);

    menu_draw_frame_("DRONE INFO");

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_detail_scroll + row;
        if (idx >= line_count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(lines[idx]);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("UP/DN:MORE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_render_(void)
{
    if (s_screen == MENU_DRONE_SCAN_SCREEN_DETAIL)
    {
        menu_render_detail_();
    }
    else
    {
        menu_render_list_();
    }
}

/**
 * @brief Internal helper for `menu_open_detail`.
 *
 * @return void
 */
static void menu_open_detail_(void)
{
    if (!s_has_selected)
    {
        return;
    }

    portENTER_CRITICAL(&s_devs_mux);
    s_has_detail = true;
    memcpy(s_detail_mac, s_selected_mac, sizeof(s_detail_mac));
    portEXIT_CRITICAL(&s_devs_mux);
    s_detail_scroll = 0;
    s_screen = MENU_DRONE_SCAN_SCREEN_DETAIL;
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_handle_button_(uint8_t button)
{
    if (s_screen == MENU_DRONE_SCAN_SCREEN_DETAIL)
    {
        if (button == BTN_UP)
        {
            s_detail_scroll--;
        }
        else if (button == BTN_DOWN)
        {
            s_detail_scroll++;
        }
        else if (button == BTN_B)
        {
            s_screen = MENU_DRONE_SCAN_SCREEN_LIST;
            portENTER_CRITICAL(&s_devs_mux);
            s_has_detail = false;
            portEXIT_CRITICAL(&s_devs_mux);
        }
        return;
    }

    if (button == BTN_B)
    {
        menu_exit_();
        return;
    }
    if ((button == BTN_LEFT) || (button == BTN_RIGHT))
    {
        s_ble_scan_enabled = !s_ble_scan_enabled;
        menu_restart_scanner_();
        return;
    }

    menu_drone_scan_snapshot_t snaps[MENU_DRONE_SCAN_MAX_DEVICES];
    const int count = snapshot_build_(snaps, (int)MENU_DRONE_SCAN_MAX_DEVICES);
    if (count <= 0)
    {
        return;
    }

    int idx = s_selected_index;
    if (s_has_selected)
    {
        const int found = snapshot_find_mac_(snaps, count, s_selected_mac);
        if (found >= 0)
        {
            idx = found;
        }
    }

    if (button == BTN_UP)
    {
        idx--;
        if (idx < 0)
        {
            idx = count - 1;
        }
    }
    else if (button == BTN_DOWN)
    {
        idx++;
        if (idx >= count)
        {
            idx = 0;
        }
    }
    else if (button == BTN_A)
    {
        menu_open_detail_();
        return;
    }
    else
    {
        return;
    }

    s_selected_index = idx;
    portENTER_CRITICAL(&s_devs_mux);
    s_has_selected = true;
    memcpy(s_selected_mac, snaps[idx].mac, sizeof(s_selected_mac));
    portEXIT_CRITICAL(&s_devs_mux);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_btn_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;
    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }
    if (!s_menu_active || (s_btn_q == NULL))
    {
        return;
    }

    button_event_msg_t ev = {0};
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    menu_drone_scan_btn_msg_t out = {.button = ev.button, .event = ev.event};
    (void)xQueueSend(s_btn_q, &out, 0U);
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_task_(void *arg)
{
    (void)arg;

    while (s_menu_active)
    {
        menu_drone_scan_btn_msg_t btn = {0};
        if ((s_btn_q != NULL) && (xQueueReceive(s_btn_q, &btn, pdMS_TO_TICKS(MENU_DRONE_SCAN_REFRESH_MS)) == pdPASS))
        {
            menu_handle_button_(btn.button);
            if (!s_menu_active)
            {
                break;
            }
        }

        menu_render_();
    }

    s_menu_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_active = false;
    poom_drone_register_report_cb(NULL, NULL);
    (void)poom_drone_stop();

    if (s_menu_task != NULL)
    {
        if (s_menu_task != current_task)
        {
            TaskHandle_t t = s_menu_task;
            s_menu_task = NULL;
            vTaskDelete(t);
        }
        else
        {
            s_menu_task = NULL;
        }
    }

    if (s_btn_q != NULL)
    {
        QueueHandle_t q = s_btn_q;
        s_btn_q = NULL;
        vQueueDelete(q);
    }

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_btn_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }

    menu_free_devices_();
    s_selected_index = 0;
    s_scroll = 0;
    s_detail_scroll = 0;
    s_screen = MENU_DRONE_SCAN_SCREEN_LIST;

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

void menu_poom_drone_scan_show(void)
{
    if (s_menu_active)
    {
        return;
    }

    if (!menu_alloc_devices_())
    {
        (void)snprintf(s_status, sizeof(s_status), "NO MEM");
        menu_exit_();
        return;
    }

    poom_drone_register_report_cb(NULL, NULL);
    (void)poom_drone_stop();

    portENTER_CRITICAL(&s_devs_mux);
    menu_reset_devices_locked_();
    portEXIT_CRITICAL(&s_devs_mux);

    s_selected_index = 0;
    s_scroll = 0;
    s_detail_scroll = 0;
    s_screen = MENU_DRONE_SCAN_SCREEN_LIST;
    (void)snprintf(s_status, sizeof(s_status), "SCAN");
    menu_restart_scanner_();

    s_btn_q = xQueueCreate(8, sizeof(menu_drone_scan_btn_msg_t));
    if (s_btn_q == NULL)
    {
        menu_exit_();
        return;
    }

    s_menu_active = true;

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_btn_cb_, s_sbus_user);
        s_buttons_subscribed = true;
    }

    if (xTaskCreate(menu_task_, "menu_poom_drone_scan", MENU_DRONE_SCAN_STACK, NULL, MENU_DRONE_SCAN_PRIO, &s_menu_task) != pdPASS)
    {
        s_menu_active = false;
        menu_exit_();
        return;
    }
}
