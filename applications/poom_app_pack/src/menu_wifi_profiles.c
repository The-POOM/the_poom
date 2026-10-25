// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

// WiFi Profiles: save multiple SSID+password and connect by selecting one.
// Modeled on menu_poom_wifi_scan.c (same task/button/draw scaffolding), but
// stores many profiles in NVS (poom_secrets record blobs + a count) and adds a
// saved-networks list screen.

#include "menu_wifi_profiles.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_scanner.h"
#include "poom_wifi_profile_store.h"
#include "poom_ui_keyboard.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

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

/* Profile model + persistence are shared with Connection Test via
 * poom_wifi_profile_store; these keep the original local names. */
#define WIFI_PROF_MAX            POOM_WIFI_PROFILE_MAX
#define WIFI_PROF_SSID_CAP       POOM_WIFI_PROFILE_SSID_CAP
#define WIFI_PROF_PASS_CAP       POOM_WIFI_PROFILE_PASS_CAP
#define WIFI_PROF_VISIBLE_ROWS   (4U)
#define WIFI_PROF_REFRESH_MS     (250U)
#define WIFI_PROF_STACK          (3584U)
#define WIFI_PROF_PRIO           (4U)
#define WIFI_PROF_CONNECT_TIMEOUT_MS (15000U)
#define WIFI_PROF_INFO_HOLD_MS   (1400U)
#define WIFI_PROF_AP_TEXT_CHARS  (19U)
#define WIFI_PROF_SCROLL_STEP_MS (350U)
#define WIFI_PROF_SCROLL_GAP     (3U)

typedef poom_wifi_profile_t wifi_profile_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} wifi_prof_button_msg_t;

typedef enum
{
    WIFI_PROF_SCR_LIST = 0,
    WIFI_PROF_SCR_SCAN,
    WIFI_PROF_SCR_SELECT_AP,
    WIFI_PROF_SCR_EDIT_PASSWORD,
    WIFI_PROF_SCR_CONNECTING,
    WIFI_PROF_SCR_DELETE_CONFIRM,
    WIFI_PROF_SCR_INFO,
} wifi_prof_state_t;

static bool s_active = false;
static bool s_buttons_subscribed = false;
static bool s_exit_requested = false;
static bool s_scan_request_pending = false;
static bool s_connect_request_pending = false;
static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_task_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_sbus_user[] = "menu_wifi_profiles";

static wifi_prof_state_t s_state = WIFI_PROF_SCR_LIST;

/* This menu is opened infrequently. Keep its profile and editor buffers out of
 * permanent internal RAM and release them as soon as the menu closes. */
typedef struct
{
    wifi_profile_t profiles[WIFI_PROF_MAX];
    char edit_ssid[WIFI_PROF_SSID_CAP];
    char edit_pass[WIFI_PROF_PASS_CAP];
    poom_ui_keyboard_t keyboard;
    char connect_ssid[WIFI_PROF_SSID_CAP];
    char connect_pass[WIFI_PROF_PASS_CAP];
    char connected_ssid[WIFI_PROF_SSID_CAP];
    char status[22];
    uint32_t scan_selection_tick_ms;
} wifi_prof_session_t;

static wifi_prof_session_t *s_session = NULL;
#define s_profiles       (s_session->profiles)
#define s_edit_ssid      (s_session->edit_ssid)
#define s_edit_pass      (s_session->edit_pass)
#define s_keyboard       (s_session->keyboard)
#define s_connect_ssid   (s_session->connect_ssid)
#define s_connect_pass   (s_session->connect_pass)
#define s_connected_ssid (s_session->connected_ssid)
#define s_status         (s_session->status)
#define s_scan_selection_tick_ms (s_session->scan_selection_tick_ms)

static uint16_t s_profile_count = 0U;
static uint16_t s_sel = 0U;           /* 0..s_profile_count (last = "+ ADD") */
static uint16_t s_window_start = 0U;
static uint16_t s_scan_sel = 0U;
static uint16_t s_scan_window_start = 0U;
static uint32_t s_connect_start_ms = 0U;
static uint32_t s_info_until_ms = 0U;
static volatile bool s_scan_cancel_requested = false;

static esp_err_t wifi_prof_exit_(void);
static void wifi_prof_task_(void *arg);
static void wifi_prof_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void wifi_prof_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx);

static uint32_t wifi_prof_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void wifi_prof_wake_(void)
{
    portENTER_CRITICAL(&s_task_lock);
    if(s_task != NULL)
    {
        (void)xTaskNotifyGive(s_task);
    }
    portEXIT_CRITICAL(&s_task_lock);
}

static bool wifi_prof_alloc_session_(void)
{
    if(s_session != NULL)
    {
        return true;
    }
    s_session = heap_caps_calloc(1U, sizeof(*s_session),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_session == NULL)
    {
        s_session = calloc(1U, sizeof(*s_session));
    }
    return (s_session != NULL);
}

static void wifi_prof_free_session_(void)
{
    if(s_session != NULL)
    {
        (void)memset(s_session, 0, sizeof(*s_session));
        free(s_session);
        s_session = NULL;
    }
}

/* ---- persistence --------------------------------------------------------- */

/* Thin wrappers over the shared store so the rest of this menu is unchanged. */
static void wifi_prof_load_all_(void)
{
    poom_wifi_profile_store_load(s_profiles, WIFI_PROF_MAX, &s_profile_count);
}

static esp_err_t wifi_prof_add_or_update_(const char *ssid, const char *pass)
{
    return poom_wifi_profile_store_add_or_update(s_profiles, &s_profile_count,
                                                 WIFI_PROF_MAX, ssid, pass);
}

static esp_err_t wifi_prof_delete_(uint16_t slot)
{
    return poom_wifi_profile_store_delete(s_profiles, &s_profile_count, slot);
}

/* ---- small helpers ------------------------------------------------------- */

static void wifi_prof_copy_ssid_(char *out, size_t out_len, const uint8_t *raw)
{
    size_t n;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if(raw == NULL)
    {
        return;
    }
    n = strnlen((const char *)raw, 32U);
    if(n >= out_len)
    {
        n = out_len - 1U;
    }
    if(n > 0U)
    {
        (void)memcpy(out, raw, n);
    }
    out[n] = '\0';
}

static void wifi_prof_begin_info_(const char *text)
{
    (void)snprintf(s_status, sizeof(s_status), "%.21s", (text != NULL) ? text : "");
    s_state = WIFI_PROF_SCR_INFO;
    s_info_until_ms = wifi_prof_now_ms_() + WIFI_PROF_INFO_HOLD_MS;
    if(s_info_until_ms == 0U)
    {
        s_info_until_ms = 1U;
    }
}

static bool wifi_prof_info_elapsed_(void)
{
    if(s_info_until_ms == 0U)
    {
        return false;
    }
    return ((int32_t)(wifi_prof_now_ms_() - s_info_until_ms) >= 0);
}

/* ---- scanner access (reuse the dedup helpers' simple form) --------------- */

static uint16_t wifi_prof_ap_count_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();

    if(records == NULL)
    {
        return 0U;
    }
    if(records->count > POOM_WIFI_SCANNER_MAX_AP)
    {
        return POOM_WIFI_SCANNER_MAX_AP;
    }
    return records->count;
}

/* ---- drawing ------------------------------------------------------------- */

static void wifi_prof_header_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(16, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void wifi_prof_footer_(const char *left, const char *right)
{
    if(left != NULL)
    {
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(left);
    }
    if(right != NULL)
    {
        poom_arduboy_set_cursor(84, 56);
        (void)poom_arduboy_print(right);
    }
}

/* Match Settings > WIFI SCAN: only the highlighted long SSID scrolls. */
static void wifi_prof_format_ap_row_(char *out, size_t out_len,
                                     const char *ssid, bool selected)
{
    size_t ssid_len;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if(ssid == NULL)
    {
        return;
    }

    ssid_len = strlen(ssid);
    if(!selected || (ssid_len <= WIFI_PROF_AP_TEXT_CHARS))
    {
        (void)snprintf(out, out_len, "%.*s", (int)WIFI_PROF_AP_TEXT_CHARS, ssid);
        return;
    }

    const size_t cycle = ssid_len + WIFI_PROF_SCROLL_GAP;
    const size_t phase = (size_t)(((wifi_prof_now_ms_() - s_scan_selection_tick_ms) /
                                   WIFI_PROF_SCROLL_STEP_MS) % cycle);
    for(size_t i = 0U; (i < WIFI_PROF_AP_TEXT_CHARS) && (i + 1U < out_len); ++i)
    {
        const size_t src = (phase + i) % cycle;
        out[i] = (src < ssid_len) ? ssid[src] : ' ';
        out[i + 1U] = '\0';
    }
}

static void wifi_prof_draw_list_(void)
{
    uint16_t total = (uint16_t)(s_profile_count + 1U); /* + "ADD" row */

    wifi_prof_header_("WIFI PROFILES");

    if(s_sel < s_window_start)
    {
        s_window_start = s_sel;
    }
    if(s_sel >= (uint16_t)(s_window_start + WIFI_PROF_VISIBLE_ROWS))
    {
        s_window_start = (uint16_t)(s_sel - WIFI_PROF_VISIBLE_ROWS + 1U);
    }

    for(uint16_t row = 0U; row < WIFI_PROF_VISIBLE_ROWS; row++)
    {
        uint16_t idx = (uint16_t)(s_window_start + row);
        int16_t y;
        char line[22];

        if(idx >= total)
        {
            break;
        }
        y = (int16_t)(14 + (int16_t)row * 10);

        if(idx == s_profile_count)
        {
            (void)snprintf(line, sizeof(line), "+ ADD NETWORK");
        }
        else
        {
            bool connected = (s_connected_ssid[0] != '\0') &&
                             (strncmp(s_connected_ssid, s_profiles[idx].ssid, WIFI_PROF_SSID_CAP) == 0);
            (void)snprintf(line, sizeof(line), "%c%.18s",
                           connected ? '*' : ' ', s_profiles[idx].ssid);
        }

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);
        if(idx == s_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    if(s_sel == s_profile_count)
    {
        wifi_prof_footer_("A:ADD", "B:EXIT");
    }
    else
    {
        wifi_prof_footer_("A:CONN L:DEL", "B:EXIT");
    }
    poom_arduboy_display();
}

static void wifi_prof_draw_select_ap_(void)
{
    uint16_t count = wifi_prof_ap_count_();

    wifi_prof_header_("ADD NETWORK");

    if(count == 0U)
    {
        poom_arduboy_set_cursor(22, 28);
        (void)poom_arduboy_print(F("No AP found"));
        wifi_prof_footer_("A:RESCAN", "B:BACK");
        poom_arduboy_display();
        return;
    }

    if(s_scan_sel < s_scan_window_start)
    {
        s_scan_window_start = s_scan_sel;
    }
    if(s_scan_sel >= (uint16_t)(s_scan_window_start + WIFI_PROF_VISIBLE_ROWS))
    {
        s_scan_window_start = (uint16_t)(s_scan_sel - WIFI_PROF_VISIBLE_ROWS + 1U);
    }

    for(uint16_t row = 0U; row < WIFI_PROF_VISIBLE_ROWS; row++)
    {
        uint16_t idx = (uint16_t)(s_scan_window_start + row);
        wifi_ap_record_t *ap;
        char ssid[WIFI_PROF_SSID_CAP];
        char line[22];
        int16_t y;

        if(idx >= count)
        {
            break;
        }
        ap = poom_wifi_scanner_get_ap_record(idx);
        if(ap == NULL)
        {
            break;
        }
        wifi_prof_copy_ssid_(ssid, sizeof(ssid), ap->ssid);
        if(ssid[0] == '\0')
        {
            (void)snprintf(ssid, sizeof(ssid), "<hidden>");
        }
        wifi_prof_format_ap_row_(line, sizeof(line), ssid, idx == s_scan_sel);
        y = (int16_t)(14 + (int16_t)row * 10);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);
        if(idx == s_scan_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    wifi_prof_footer_("A:PICK", "B:BACK");
    poom_arduboy_display();
}

static void wifi_prof_draw_(void)
{
    char line[22];

    switch(s_state)
    {
        case WIFI_PROF_SCR_LIST:
            wifi_prof_draw_list_();
            return;

        case WIFI_PROF_SCR_SCAN:
            wifi_prof_header_("ADD NETWORK");
            poom_arduboy_set_cursor(22, 28);
            (void)poom_arduboy_print(F("Scanning..."));
            wifi_prof_footer_(NULL, "B:EXIT");
            poom_arduboy_display();
            return;

        case WIFI_PROF_SCR_SELECT_AP:
            wifi_prof_draw_select_ap_();
            return;

        case WIFI_PROF_SCR_EDIT_PASSWORD:
            poom_ui_keyboard_draw(&s_keyboard);
            return;

        case WIFI_PROF_SCR_CONNECTING:
            wifi_prof_header_("CONNECTING");
            (void)snprintf(line, sizeof(line), "%.20s", s_connect_ssid);
            poom_arduboy_set_cursor(4, 26);
            (void)poom_arduboy_print(line);
            poom_arduboy_set_cursor(22, 38);
            (void)poom_arduboy_print(F("Please wait..."));
            wifi_prof_footer_(NULL, "B:CANCEL");
            poom_arduboy_display();
            return;

        case WIFI_PROF_SCR_DELETE_CONFIRM:
            wifi_prof_header_("DELETE?");
            if(s_sel < s_profile_count)
            {
                (void)snprintf(line, sizeof(line), "%.20s", s_profiles[s_sel].ssid);
                poom_arduboy_set_cursor(4, 28);
                (void)poom_arduboy_print(line);
            }
            wifi_prof_footer_("A:DELETE", "B:KEEP");
            poom_arduboy_display();
            return;

        case WIFI_PROF_SCR_INFO:
        default:
            wifi_prof_header_("WIFI PROFILES");
            (void)snprintf(line, sizeof(line), "%.20s", s_status);
            poom_arduboy_set_cursor(6, 30);
            (void)poom_arduboy_print(line);
            poom_arduboy_display();
            return;
    }
}

/* ---- actions (task context) ---------------------------------------------- */

static void wifi_prof_run_scan_(void)
{
    esp_err_t status;

    s_scan_cancel_requested = false;
    s_state = WIFI_PROF_SCR_SCAN;
    (void)wifi_prof_draw_();

    (void)poom_wifi_scanner_clear_ap_records();
    status = poom_wifi_scanner_scan();
    if(s_scan_cancel_requested || s_exit_requested)
    {
        s_scan_cancel_requested = false;
        s_state = WIFI_PROF_SCR_LIST;
        return;
    }
    if(status != ESP_OK)
    {
        wifi_prof_begin_info_("Scan failed");
        return;
    }
    s_scan_sel = 0U;
    s_scan_window_start = 0U;
    s_scan_selection_tick_ms = wifi_prof_now_ms_();
    s_state = WIFI_PROF_SCR_SELECT_AP;
}

static void wifi_prof_update_connect_(void)
{
    if(poom_wifi_ctrl_sta_has_ip())
    {
        (void)snprintf(s_connected_ssid, sizeof(s_connected_ssid), "%s", s_connect_ssid);
        wifi_prof_begin_info_("Connected");
        return;
    }
    if((wifi_prof_now_ms_() - s_connect_start_ms) >= WIFI_PROF_CONNECT_TIMEOUT_MS)
    {
        wifi_prof_begin_info_("Connect timeout");
    }
}

/* ---- enter password for the picked AP ------------------------------------ */

static void wifi_prof_enter_password_(void)
{
    wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record(s_scan_sel);

    if(ap == NULL)
    {
        wifi_prof_begin_info_("Invalid AP");
        return;
    }
    wifi_prof_copy_ssid_(s_edit_ssid, sizeof(s_edit_ssid), ap->ssid);
    (void)memset(s_edit_pass, 0, sizeof(s_edit_pass));
    poom_ui_keyboard_init(&s_keyboard, s_edit_pass, sizeof(s_edit_pass), s_edit_ssid);
    s_state = WIFI_PROF_SCR_EDIT_PASSWORD;
}

/* Start a connection to the given ssid/pass (task will poll for IP). */
static void wifi_prof_request_connect_(const char *ssid, const char *pass)
{
    (void)snprintf(s_connect_ssid, sizeof(s_connect_ssid), "%.*s",
                   (int)(WIFI_PROF_SSID_CAP - 1U), ssid);
    (void)snprintf(s_connect_pass, sizeof(s_connect_pass), "%.*s",
                   (int)(WIFI_PROF_PASS_CAP - 1U), (pass != NULL) ? pass : "");
    s_connect_request_pending = true;
    s_state = WIFI_PROF_SCR_CONNECTING;
}

/* ---- buttons ------------------------------------------------------------- */

static void wifi_prof_handle_list_(uint8_t button)
{
    uint16_t total = (uint16_t)(s_profile_count + 1U);

    if(button == BTN_UP)
    {
        if(s_sel > 0U) { s_sel--; }
    }
    else if(button == BTN_DOWN)
    {
        if(s_sel < (uint16_t)(total - 1U)) { s_sel++; }
    }
    else if(button == BTN_A)
    {
        if(s_sel == s_profile_count)
        {
            s_scan_request_pending = true; /* ADD */
        }
        else
        {
            wifi_prof_request_connect_(s_profiles[s_sel].ssid, s_profiles[s_sel].pass);
        }
    }
    else if(button == BTN_LEFT)
    {
        if(s_sel < s_profile_count)
        {
            s_state = WIFI_PROF_SCR_DELETE_CONFIRM;
        }
    }
}

static void wifi_prof_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    wifi_prof_button_msg_t ev;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }
    (void)memcpy(&ev, msg->data, sizeof(ev));
    if(ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    /* B handling per screen. */
    if(ev.button == BTN_B)
    {
        switch(s_state)
        {
            case WIFI_PROF_SCR_SCAN:
                s_scan_cancel_requested = true;
                s_exit_requested = true;
                (void)esp_wifi_scan_stop();
                break;
            case WIFI_PROF_SCR_LIST:
                if(s_task == NULL)
                {
                    (void)wifi_prof_exit_();
                }
                else
                {
                    s_exit_requested = true;
                }
                break;
            case WIFI_PROF_SCR_SELECT_AP:
            case WIFI_PROF_SCR_EDIT_PASSWORD:
            case WIFI_PROF_SCR_DELETE_CONFIRM:
            case WIFI_PROF_SCR_CONNECTING:
                s_state = WIFI_PROF_SCR_LIST;
                break;
            default:
                break;
        }
        wifi_prof_wake_();
        return;
    }

    switch(s_state)
    {
        case WIFI_PROF_SCR_LIST:
            wifi_prof_handle_list_(ev.button);
            break;

        case WIFI_PROF_SCR_SELECT_AP:
        {
            uint16_t count = wifi_prof_ap_count_();
            if(count == 0U)
            {
                if(ev.button == BTN_A) { s_scan_request_pending = true; }
                break;
            }
            if(ev.button == BTN_UP)
            {
                if(s_scan_sel > 0U)
                {
                    s_scan_sel--;
                    s_scan_selection_tick_ms = wifi_prof_now_ms_();
                }
            }
            else if(ev.button == BTN_DOWN)
            {
                if(s_scan_sel < (uint16_t)(count - 1U))
                {
                    s_scan_sel++;
                    s_scan_selection_tick_ms = wifi_prof_now_ms_();
                }
            }
            else if((ev.button == BTN_A) || (ev.button == BTN_RIGHT))
            {
                wifi_prof_enter_password_();
            }
            break;
        }

        case WIFI_PROF_SCR_EDIT_PASSWORD:
        {
            poom_ui_keyboard_action_t action =
                poom_ui_keyboard_handle_button(&s_keyboard, ev.button);
            if((action == POOM_UI_KEYBOARD_ACTION_ACCEPT) && !s_connect_request_pending)
            {
                /* Save the profile, then connect. */
                (void)wifi_prof_add_or_update_(s_edit_ssid, s_edit_pass);
                wifi_prof_request_connect_(s_edit_ssid, s_edit_pass);
            }
            break;
        }

        case WIFI_PROF_SCR_DELETE_CONFIRM:
            if(ev.button == BTN_A)
            {
                (void)wifi_prof_delete_(s_sel);
                if(s_sel > s_profile_count) { s_sel = s_profile_count; }
                wifi_prof_begin_info_("Deleted");
            }
            break;

        default:
            break;
    }

    wifi_prof_wake_();
}

/* ---- wifi event cb (logging only) ---------------------------------------- */

static void wifi_prof_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;
    if(info == NULL)
    {
        return;
    }
    if(info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        /* If we lose the link to the one we marked connected, clear the marker. */
        s_connected_ssid[0] = '\0';
    }
}

/* ---- teardown ------------------------------------------------------------ */

static esp_err_t wifi_prof_exit_(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t to_delete = NULL;

    s_active = false;
    s_exit_requested = false;
    s_scan_request_pending = false;
    s_connect_request_pending = false;
    s_scan_cancel_requested = false;
    if(s_state == WIFI_PROF_SCR_SCAN)
    {
        (void)esp_wifi_scan_stop();
    }

    portENTER_CRITICAL(&s_task_lock);
    if((s_task != NULL) && (s_task != current))
    {
        to_delete = s_task;
    }
    s_task = NULL;
    portEXIT_CRITICAL(&s_task_lock);

    if(to_delete != NULL)
    {
        vTaskDelete(to_delete);
    }

    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", wifi_prof_button_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }

    (void)poom_wifi_ctrl_unregister_cb();
    wifi_prof_free_session_();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/* ---- task ---------------------------------------------------------------- */

static void wifi_prof_task_(void *arg)
{
    (void)arg;

    while(s_active)
    {
        if(s_exit_requested)
        {
            (void)wifi_prof_exit_();
            break;
        }

        if(s_scan_request_pending)
        {
            s_scan_request_pending = false;
            wifi_prof_run_scan_();
            if(s_exit_requested)
            {
                continue;
            }
        }

        if(s_connect_request_pending)
        {
            esp_err_t status;

            s_connect_request_pending = false;
            status = poom_wifi_ctrl_sta_connect(
                s_connect_ssid,
                (s_connect_pass[0] != '\0') ? s_connect_pass : NULL);
            if(status != ESP_OK)
            {
                wifi_prof_begin_info_("Connect failed");
            }
            else
            {
                s_connect_start_ms = wifi_prof_now_ms_();
                s_state = WIFI_PROF_SCR_CONNECTING;
            }
        }

        if(s_state == WIFI_PROF_SCR_CONNECTING)
        {
            wifi_prof_update_connect_();
        }

        (void)wifi_prof_draw_();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_PROF_REFRESH_MS));

        if((s_state == WIFI_PROF_SCR_INFO) && wifi_prof_info_elapsed_())
        {
            s_state = WIFI_PROF_SCR_LIST;
        }
    }

    portENTER_CRITICAL(&s_task_lock);
    if(s_task == xTaskGetCurrentTaskHandle())
    {
        s_task = NULL;
    }
    portEXIT_CRITICAL(&s_task_lock);
    vTaskDelete(NULL);
}

void menu_wifi_profiles_show(void)
{
    if(s_active)
    {
        return;
    }
    if(!wifi_prof_alloc_session_())
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    s_active = true;
    s_exit_requested = false;
    s_scan_request_pending = false;
    s_connect_request_pending = false;
    s_state = WIFI_PROF_SCR_LIST;
    s_sel = 0U;
    s_window_start = 0U;
    s_connected_ssid[0] = '\0';
    s_status[0] = '\0';

    (void)poom_wifi_ctrl_init_sta();
    wifi_prof_load_all_();
    (void)poom_wifi_ctrl_register_cb(wifi_prof_wifi_evt_cb_, NULL);

    if(!s_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", wifi_prof_button_cb_, s_sbus_user))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            s_active = false;
            wifi_prof_free_session_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    bool create;
    portENTER_CRITICAL(&s_task_lock);
    create = (s_task == NULL);
    portEXIT_CRITICAL(&s_task_lock);

    if(create)
    {
        TaskHandle_t created = NULL;
        if(xTaskCreate(wifi_prof_task_, "menu_wifi_prof", WIFI_PROF_STACK, NULL,
                       WIFI_PROF_PRIO, &created) == pdPASS)
        {
            portENTER_CRITICAL(&s_task_lock);
            if(s_task == NULL)
            {
                s_task = created;
                created = NULL;
            }
            portEXIT_CRITICAL(&s_task_lock);
            if(created != NULL)
            {
                vTaskDelete(created);
            }
        }
        else
        {
            s_active = false;
            if(s_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", wifi_prof_button_cb_, s_sbus_user);
                s_buttons_subscribed = false;
            }
            (void)poom_wifi_ctrl_unregister_cb();
            wifi_prof_free_session_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        }
    }
}
