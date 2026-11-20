// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Handshake Vault: THE BEAST > Handshake Vault.
//   CRACK HANDSHAKE  -> hands off to the existing on-device cracker.
//   EXPORT HANDSHAKE -> joins a saved Wi-Fi profile and serves the captured
//                       .22000 / .pcap files over HTTP (another device on the
//                       LAN downloads them with a browser + the shown code).
//
// The export transport reuses the proven wardrive "Transfer over Wi-Fi" model:
// a private station link (poom_hs_link) + a small HTTP server (poom_hs_transfer).

#include "menu_handshake_vault.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "menu_handshake_crack.h"
#include "esp_wifi.h"
#include "poom_hs_link.h"
#include "poom_hs_transfer.h"
#include "poom_sbus.h"
#include "poom_ui_keyboard.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_profile_store.h"
#include "sd_card.h"

#ifndef POOM_MENU_RESUME_TOPIC
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#endif

#define HV2_DIR                SD_CARD_PATH "/pcaps/handshakes"
#define HV2_TASK_STACK         (8192U) /* room for esp_http_server + TLS-free HTTP */
#define HV2_TASK_PRIO          (5U)
#define HV2_REFRESH_MS         (250U)
#define HV2_CONNECT_TIMEOUT_MS (20000U)
#define HV2_INFO_MS            (2600U)
#define HV2_VISIBLE_ROWS       (4U)
#define HV2_SCAN_MAX           (16U)   /* networks shown by Scan for Network */

typedef enum
{
    HV2_MENU = 0,
    HV2_EXPORT_MENU,  /* Saved Profiles vs Scan for Network */
    HV2_PICK,         /* saved-profile picker */
    HV2_SCANNING,     /* live Wi-Fi scan in progress */
    HV2_SCAN_LIST,    /* scan results picker */
    HV2_PASSWORD,     /* on-device keyboard for the network password */
    HV2_SAVE,         /* offer to save the scanned network */
    HV2_CONNECTING,
    HV2_SERVING,
    HV2_INFO,
} hv2_screen_t;

static bool s_active = false;
static volatile bool s_exit_requested = false;
static TaskHandle_t s_task = NULL;
static bool s_buttons_subscribed = false;

static volatile hv2_screen_t s_screen = HV2_MENU;
static uint8_t s_menu_sel = 0U;

static poom_wifi_profile_t s_profiles[POOM_WIFI_PROFILE_MAX];
static uint16_t s_profile_count = 0U;
static uint16_t s_profile_sel = 0U;
static uint16_t s_profile_window = 0U;

static volatile bool s_connect_pending = false;
static char s_connect_ssid[POOM_WIFI_PROFILE_SSID_CAP];
static char s_connect_pass[POOM_WIFI_PROFILE_PASS_CAP];
static uint32_t s_connect_start_ms = 0U;
static bool s_link_registered = false;

static volatile bool s_have_ip = false;
static char s_ip[20] = {0};
static char s_token[33] = {0};
static bool s_serving = false;
static unsigned s_hs_count = 0U; /* cached at serve start; avoids re-scanning the SD each redraw */

static volatile bool s_launch_crack = false;

/* Export connect-source chooser + Scan for Network state. */
static uint8_t s_export_sel = 0U; /* 0 = Saved Profiles, 1 = Scan for Network */
static volatile bool s_scan_pending = false;
static char s_scan_ssid[HV2_SCAN_MAX][POOM_WIFI_PROFILE_SSID_CAP];
static bool s_scan_secure[HV2_SCAN_MAX];
static uint16_t s_scan_count = 0U;
static uint16_t s_scan_sel = 0U;
static uint16_t s_scan_window = 0U;
static poom_ui_keyboard_t s_kb;
static char s_pending_ssid[POOM_WIFI_PROFILE_SSID_CAP];
static char s_pending_pass[POOM_WIFI_PROFILE_PASS_CAP];

static char s_info[40] = {0};
static uint32_t s_info_until_ms = 0U;

static void hv2_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void hv2_draw_(void);

/* ---- helpers ------------------------------------------------------------- */

static uint32_t hv2_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void hv2_wake_(void)
{
    if (s_task != NULL)
    {
        (void)xTaskNotifyGive(s_task);
    }
}

/* Handshakes live on the SD card; make sure it is mounted before we read or
 * serve them. A fresh boot leaves the card unmounted until something uses it,
 * which is why the folder read empty until a capture "nudged" it. Mirrors the
 * cracker's hs_scan_dir_ mount. */
static void hv2_ensure_sd_(void)
{
    if (sd_card_is_not_mounted())
    {
        sd_card_begin();
        (void)sd_card_mount();
    }
}

/* Count the .22000 files available to export (shown on the serving screen). */
static unsigned hv2_count_22000_(void)
{
    DIR *d = opendir(HV2_DIR);
    if (d == NULL)
    {
        return 0U;
    }
    unsigned count = 0U;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        size_t n = strlen(e->d_name);
        if (n > 6U && strcmp(e->d_name + n - 6, ".22000") == 0)
        {
            count++;
        }
    }
    (void)closedir(d);
    return count;
}

/* Runs in the esp_event task context when the private link gets an IP. */
static void hv2_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;
    if ((info != NULL) && (info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP))
    {
        (void)snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info->ip));
        s_have_ip = true;
        hv2_wake_();
    }
}

static void hv2_info_(const char *msg)
{
    (void)snprintf(s_info, sizeof(s_info), "%.39s", (msg != NULL) ? msg : "");
    s_info_until_ms = hv2_now_ms_() + HV2_INFO_MS;
    s_screen = HV2_INFO;
}

/* Stop the server and fully release the private Wi-Fi link. */
static void hv2_teardown_link_(void)
{
    if (s_serving)
    {
        poom_hs_transfer_stop();
        s_serving = false;
    }
    (void)poom_hs_link_deinit();
    if (s_link_registered)
    {
        (void)poom_hs_link_unregister_cb();
        s_link_registered = false;
    }
    s_have_ip = false;
    s_ip[0] = '\0';
    s_token[0] = '\0';
}

static void hv2_request_connect_(const char *ssid, const char *pass)
{
    (void)snprintf(s_connect_ssid, sizeof(s_connect_ssid), "%.*s",
                   (int)(POOM_WIFI_PROFILE_SSID_CAP - 1U), (ssid != NULL) ? ssid : "");
    (void)snprintf(s_connect_pass, sizeof(s_connect_pass), "%.*s",
                   (int)(POOM_WIFI_PROFILE_PASS_CAP - 1U), (pass != NULL) ? pass : "");
    s_have_ip = false;
    s_connect_pending = true;
    s_screen = HV2_CONNECTING;
}

/* EXPORT selected: offer Saved Profiles or Scan for Network (like Wardrive's
 * transfer-over-Wi-Fi join). */
static void hv2_begin_export_(void)
{
    poom_wifi_profile_store_load(s_profiles, POOM_WIFI_PROFILE_MAX, &s_profile_count);
    s_export_sel = 0U;
    s_screen = HV2_EXPORT_MENU;
}

/* Saved Profiles chosen: open the picker, or warn if there are none. */
static void hv2_start_saved_(void)
{
    if (s_profile_count == 0U)
    {
        hv2_info_("No saved profiles");
        return;
    }
    s_profile_sel = 0U;
    s_profile_window = 0U;
    s_screen = HV2_PICK;
}

/* Bring the private station up (clean, from the stock driver released) and run
 * one blocking scan. Fills s_scan_* with up to HV2_SCAN_MAX unique SSIDs. Runs
 * on the task (shows "Scanning..." first). */
static void hv2_scan_run_(void)
{
    if (!poom_hs_link_owned())
    {
        (void)poom_wifi_ctrl_set_promiscuous(false);
        (void)poom_wifi_ctrl_sta_disconnect();
        (void)poom_wifi_ctrl_deinit();
    }
    if (!s_link_registered)
    {
        (void)poom_hs_link_register_cb(hv2_wifi_evt_cb_, NULL);
        s_link_registered = true;
    }
    if (poom_hs_link_init_sta() != ESP_OK)
    {
        hv2_teardown_link_();
        hv2_info_("Scan init failed");
        return;
    }

    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK)
    {
        hv2_teardown_link_();
        hv2_info_("Scan failed");
        return;
    }
    uint16_t found = HV2_SCAN_MAX;
    wifi_ap_record_t recs[HV2_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&found, recs) != ESP_OK)
    {
        (void)esp_wifi_clear_ap_list();
        hv2_teardown_link_();
        hv2_info_("Scan read failed");
        return;
    }

    s_scan_count = 0U;
    for (uint16_t i = 0U; (i < found) && (s_scan_count < HV2_SCAN_MAX); i++)
    {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0')
        {
            continue; /* hidden */
        }
        bool dup = false;
        for (uint16_t j = 0U; j < s_scan_count; j++)
        {
            if (strcmp(s_scan_ssid[j], ssid) == 0)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }
        (void)snprintf(s_scan_ssid[s_scan_count], POOM_WIFI_PROFILE_SSID_CAP, "%s", ssid);
        s_scan_secure[s_scan_count] = (recs[i].authmode != WIFI_AUTH_OPEN);
        s_scan_count++;
    }

    if (s_scan_count == 0U)
    {
        hv2_teardown_link_();
        hv2_info_("No networks found");
        return;
    }
    s_scan_sel = 0U;
    s_scan_window = 0U;
    s_screen = HV2_SCAN_LIST;
}

/* A scanned network was chosen: ask for a password (secured) or go straight to
 * the save prompt (open). */
static void hv2_pick_scanned_(void)
{
    (void)snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", s_scan_ssid[s_scan_sel]);
    s_pending_pass[0] = '\0';
    if (s_scan_secure[s_scan_sel])
    {
        poom_ui_keyboard_init(&s_kb, s_pending_pass, sizeof(s_pending_pass), "WIFI PASSWORD");
        s_screen = HV2_PASSWORD;
    }
    else
    {
        s_screen = HV2_SAVE;
    }
}

/* Leaving the save prompt: optionally persist the network, then connect. */
static void hv2_finish_scanned_(bool save)
{
    if (save)
    {
        (void)poom_wifi_profile_store_add_or_update(
            s_profiles, &s_profile_count, POOM_WIFI_PROFILE_MAX,
            s_pending_ssid, (s_pending_pass[0] != '\0') ? s_pending_pass : NULL);
    }
    hv2_request_connect_(s_pending_ssid, s_pending_pass);
}

/* ---- render -------------------------------------------------------------- */

static void hv2_draw_header_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void hv2_draw_menu_(void)
{
    static const char *const items[2] = {"CRACK HANDSHAKE", "EXPORT HANDSHAKE"};
    hv2_draw_header_("HANDSHAKE VAULT");
    for (uint8_t i = 0U; i < 2U; i++)
    {
        int16_t y = (int16_t)(18 + (i * 12U));
        poom_arduboy_set_cursor(6, y);
        (void)poom_arduboy_print(items[i]);
        if (i == s_menu_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 2), ARDUBOY_WIDTH, 11, INVERT);
        }
    }
    poom_arduboy_set_cursor(2, 54);
    (void)poom_arduboy_print(F("A:SELECT  B:EXIT"));
    poom_arduboy_display();
}

static void hv2_draw_pick_(void)
{
    char l[40];
    hv2_draw_header_("EXPORT: PICK AP");
    for (uint32_t i = 0U; i < HV2_VISIBLE_ROWS; i++)
    {
        uint32_t idx = s_profile_window + i;
        if (idx >= s_profile_count)
        {
            break;
        }
        int16_t y = (int16_t)(14 + (i * 10U));
        poom_arduboy_set_cursor(3, y);
        (void)snprintf(l, sizeof(l), "%.20s", s_profiles[idx].ssid);
        (void)poom_arduboy_print(l);
        if (idx == s_profile_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }
    poom_arduboy_set_cursor(2, 56);
    (void)snprintf(l, sizeof(l), "%u/%u A:JOIN B:BACK",
                   (unsigned)(s_profile_sel + 1U), (unsigned)s_profile_count);
    (void)poom_arduboy_print(l);
    poom_arduboy_display();
}

static void hv2_draw_connecting_(void)
{
    char l[40];
    hv2_draw_header_("EXPORT");
    poom_arduboy_set_cursor(2, 20);
    (void)poom_arduboy_print(F("Joining AP:"));
    poom_arduboy_set_cursor(2, 32);
    (void)snprintf(l, sizeof(l), "%.20s", s_connect_ssid);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 44);
    (void)poom_arduboy_print(F("Please wait..."));
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("B:CANCEL"));
    poom_arduboy_display();
}

static void hv2_draw_serving_(void)
{
    char l[40];

    /* Too many wrong codes were entered on the browser: tell the operator to
     * restart the export (the server stays locked until they do). */
    if (poom_hs_transfer_locked())
    {
        hv2_draw_header_("EXPORT LOCKED");
        poom_arduboy_set_cursor(2, 20);
        (void)poom_arduboy_print(F("Wrong code entered"));
        poom_arduboy_set_cursor(2, 30);
        (void)poom_arduboy_print(F("too many times."));
        poom_arduboy_set_cursor(2, 42);
        (void)poom_arduboy_print(F("B: restart export"));
        poom_arduboy_display();
        return;
    }

    hv2_draw_header_("EXPORT READY");
    poom_arduboy_set_cursor(2, 16);
    (void)snprintf(l, sizeof(l), "http://%s/", s_ip);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 28);
    (void)snprintf(l, sizeof(l), "Code: %.10s", s_token);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 40);
    (void)snprintf(l, sizeof(l), "%u handshakes", s_hs_count);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("B:STOP"));
    poom_arduboy_display();
}

static void hv2_draw_info_(void)
{
    hv2_draw_header_("HANDSHAKE VAULT");
    poom_arduboy_set_cursor(2, 28);
    (void)poom_arduboy_print((s_info[0] != '\0') ? s_info : "");
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void hv2_draw_export_menu_(void)
{
    static const char *const items[2] = {"SAVED PROFILES", "SCAN FOR NETWORK"};
    hv2_draw_header_("EXPORT: CONNECT");
    for (uint8_t i = 0U; i < 2U; i++)
    {
        int16_t y = (int16_t)(18 + (i * 12U));
        poom_arduboy_set_cursor(6, y);
        (void)poom_arduboy_print(items[i]);
        if (i == s_export_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 2), ARDUBOY_WIDTH, 11, INVERT);
        }
    }
    poom_arduboy_set_cursor(2, 54);
    (void)poom_arduboy_print(F("A:SELECT  B:BACK"));
    poom_arduboy_display();
}

static void hv2_draw_scanning_(void)
{
    hv2_draw_header_("SCAN");
    poom_arduboy_set_cursor(2, 26);
    (void)poom_arduboy_print(F("Scanning WiFi..."));
    poom_arduboy_display();
}

static void hv2_draw_scan_list_(void)
{
    char l[40];
    hv2_draw_header_("PICK NETWORK");
    for (uint32_t i = 0U; i < HV2_VISIBLE_ROWS; i++)
    {
        uint32_t idx = s_scan_window + i;
        if (idx >= s_scan_count)
        {
            break;
        }
        int16_t y = (int16_t)(14 + (i * 10U));
        poom_arduboy_set_cursor(3, y);
        (void)snprintf(l, sizeof(l), "%s%.18s",
                       s_scan_secure[idx] ? "*" : " ", s_scan_ssid[idx]);
        (void)poom_arduboy_print(l);
        if (idx == s_scan_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }
    poom_arduboy_set_cursor(2, 56);
    (void)snprintf(l, sizeof(l), "%u/%u A:JOIN B:BACK",
                   (unsigned)(s_scan_sel + 1U), (unsigned)s_scan_count);
    (void)poom_arduboy_print(l);
    poom_arduboy_display();
}

static void hv2_draw_save_(void)
{
    char l[40];
    hv2_draw_header_("SAVE NETWORK?");
    poom_arduboy_set_cursor(2, 22);
    (void)snprintf(l, sizeof(l), "%.20s", s_pending_ssid);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print(F("Save to profiles?"));
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("A:YES  B:NO"));
    poom_arduboy_display();
}

static void hv2_draw_(void)
{
    if (!s_active)
    {
        return;
    }
    switch (s_screen)
    {
    case HV2_MENU:        hv2_draw_menu_();        break;
    case HV2_EXPORT_MENU: hv2_draw_export_menu_(); break;
    case HV2_PICK:        hv2_draw_pick_();        break;
    case HV2_SCANNING:    hv2_draw_scanning_();    break;
    case HV2_SCAN_LIST:   hv2_draw_scan_list_();   break;
    case HV2_PASSWORD:    poom_ui_keyboard_draw(&s_kb); break;
    case HV2_SAVE:        hv2_draw_save_();        break;
    case HV2_CONNECTING:  hv2_draw_connecting_();  break;
    case HV2_SERVING:     hv2_draw_serving_();     break;
    case HV2_INFO:
    default:              hv2_draw_info_();        break;
    }
}

/* ---- lifecycle ----------------------------------------------------------- */

static void hv2_exit_(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();

    s_active = false;
    s_exit_requested = false;

    hv2_teardown_link_();

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", hv2_button_cb_, "menu_hs_vault");
        s_buttons_subscribed = false;
    }

    if (s_task != NULL)
    {
        if (s_task != current)
        {
            TaskHandle_t t = s_task;
            s_task = NULL;
            vTaskDelete(t);
        }
        else
        {
            s_task = NULL;
        }
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static void hv2_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    button_event_msg_t ev;

    (void)user_ctx;
    if ((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }
    (void)memcpy(&ev, msg->data, sizeof(ev));
    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    switch (s_screen)
    {
    case HV2_MENU:
        if (ev.button == BUTTON_B)
        {
            s_exit_requested = true;
        }
        else if (ev.button == BUTTON_UP || ev.button == BUTTON_DOWN)
        {
            s_menu_sel = (uint8_t)(s_menu_sel ^ 1U);
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_menu_sel == 0U)
            {
                s_launch_crack = true; /* serviced on the task */
            }
            else
            {
                hv2_begin_export_();
            }
        }
        break;

    case HV2_EXPORT_MENU:
        if (ev.button == BUTTON_B)
        {
            s_screen = HV2_MENU;
        }
        else if (ev.button == BUTTON_UP || ev.button == BUTTON_DOWN)
        {
            s_export_sel = (uint8_t)(s_export_sel ^ 1U);
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_export_sel == 0U)
            {
                hv2_start_saved_();
            }
            else
            {
                s_scan_pending = true; /* scan runs on the task */
                s_screen = HV2_SCANNING;
            }
        }
        break;

    case HV2_SCAN_LIST:
        if (ev.button == BUTTON_B)
        {
            hv2_teardown_link_();
            s_screen = HV2_EXPORT_MENU;
        }
        else if (ev.button == BUTTON_UP)
        {
            if (s_scan_sel > 0U)
            {
                s_scan_sel--;
                if (s_scan_sel < s_scan_window)
                {
                    s_scan_window = s_scan_sel;
                }
            }
        }
        else if (ev.button == BUTTON_DOWN)
        {
            if (s_scan_sel + 1U < s_scan_count)
            {
                s_scan_sel++;
                if (s_scan_sel >= s_scan_window + HV2_VISIBLE_ROWS)
                {
                    s_scan_window = (uint16_t)(s_scan_sel - (HV2_VISIBLE_ROWS - 1U));
                }
            }
        }
        else if (ev.button == BUTTON_A)
        {
            hv2_pick_scanned_();
        }
        break;

    case HV2_PASSWORD:
        if (ev.button == BUTTON_B)
        {
            s_screen = HV2_SCAN_LIST; /* back to pick a different network */
        }
        else if (poom_ui_keyboard_handle_button(&s_kb, ev.button) ==
                 POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            s_screen = HV2_SAVE;
        }
        break;

    case HV2_SAVE:
        if (ev.button == BUTTON_A)
        {
            hv2_finish_scanned_(true); /* save + connect */
        }
        else if (ev.button == BUTTON_B)
        {
            hv2_finish_scanned_(false); /* connect without saving */
        }
        break;

    case HV2_PICK:
        if (ev.button == BUTTON_B)
        {
            s_screen = HV2_MENU;
        }
        else if (ev.button == BUTTON_UP)
        {
            if (s_profile_sel > 0U)
            {
                s_profile_sel--;
                if (s_profile_sel < s_profile_window)
                {
                    s_profile_window = s_profile_sel;
                }
            }
        }
        else if (ev.button == BUTTON_DOWN)
        {
            if (s_profile_sel + 1U < s_profile_count)
            {
                s_profile_sel++;
                if (s_profile_sel >= s_profile_window + HV2_VISIBLE_ROWS)
                {
                    s_profile_window = (uint16_t)(s_profile_sel - (HV2_VISIBLE_ROWS - 1U));
                }
            }
        }
        else if (ev.button == BUTTON_A)
        {
            hv2_request_connect_(s_profiles[s_profile_sel].ssid,
                                 s_profiles[s_profile_sel].pass);
        }
        break;

    case HV2_CONNECTING:
        if (ev.button == BUTTON_B)
        {
            hv2_teardown_link_();
            s_screen = HV2_MENU;
        }
        break;

    case HV2_SERVING:
        if (ev.button == BUTTON_B)
        {
            hv2_teardown_link_();
            s_screen = HV2_MENU;
        }
        break;

    case HV2_INFO:
    default:
        if (ev.button == BUTTON_B)
        {
            s_screen = HV2_MENU;
        }
        break;
    }
    hv2_wake_();
}

static void hv2_task_(void *arg)
{
    (void)arg;

    hv2_ensure_sd_(); /* handshakes are on the SD; mount it before any read/serve */

    while (s_active)
    {
        if (s_exit_requested)
        {
            hv2_exit_(); /* clears s_task, unsubs, publishes resume */
            vTaskDelete(NULL);
            return;
        }

        /* Hand off to the on-device cracker: drop our own subscription + task,
         * then launch it (it owns input + publishes resume on its own exit). */
        if (s_launch_crack)
        {
            s_launch_crack = false;
            if (s_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", hv2_button_cb_, "menu_hs_vault");
                s_buttons_subscribed = false;
            }
            s_active = false;
            s_task = NULL;
            vTaskDelay(pdMS_TO_TICKS(60));
            menu_handshake_crack_show();
            vTaskDelete(NULL);
            return;
        }

        if (s_scan_pending)
        {
            s_scan_pending = false;
            hv2_draw_();       /* show "Scanning..." before the blocking scan */
            hv2_scan_run_();   /* blocking; advances to HV2_SCAN_LIST or an info */
        }

        if (s_connect_pending)
        {
            s_connect_pending = false;
            /* Force the stock Wi-Fi manager fully off so the private link starts
             * from a clean NOT_INIT state (the proven join fix). */
            if (!poom_hs_link_owned())
            {
                (void)poom_wifi_ctrl_set_promiscuous(false);
                (void)poom_wifi_ctrl_sta_disconnect();
                (void)poom_wifi_ctrl_deinit();
            }
            if (!s_link_registered)
            {
                (void)poom_hs_link_register_cb(hv2_wifi_evt_cb_, NULL);
                s_link_registered = true;
            }
            s_have_ip = false;
            esp_err_t rc = poom_hs_link_sta_connect(
                s_connect_ssid, (s_connect_pass[0] != '\0') ? s_connect_pass : NULL);
            if (rc != ESP_OK)
            {
                hv2_teardown_link_();
                hv2_info_("Connect failed");
            }
            else
            {
                s_connect_start_ms = hv2_now_ms_();
                s_screen = HV2_CONNECTING;
            }
        }

        if (s_screen == HV2_CONNECTING)
        {
            if (s_have_ip && poom_hs_link_sta_has_ip())
            {
                if (poom_hs_transfer_start(s_token))
                {
                    s_hs_count = hv2_count_22000_(); /* count once, not every redraw */
                    s_serving = true;
                    s_screen = HV2_SERVING;
                }
                else
                {
                    hv2_teardown_link_();
                    hv2_info_("HTTP start failed");
                }
            }
            else if ((hv2_now_ms_() - s_connect_start_ms) >= HV2_CONNECT_TIMEOUT_MS)
            {
                hv2_teardown_link_();
                hv2_info_("Join timeout");
            }
        }

        if ((s_screen == HV2_SERVING) && poom_hs_transfer_expired())
        {
            hv2_teardown_link_();
            hv2_info_("Export expired");
        }

        if ((s_screen == HV2_INFO) && (hv2_now_ms_() >= s_info_until_ms))
        {
            s_screen = HV2_MENU;
        }

        hv2_draw_();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HV2_REFRESH_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void menu_handshake_vault_show(void)
{
    if (s_active)
    {
        return;
    }

    s_active = true;
    s_exit_requested = false;
    s_screen = HV2_MENU;
    s_menu_sel = 0U;
    s_connect_pending = false;
    s_launch_crack = false;
    s_serving = false;
    s_link_registered = false;
    s_have_ip = false;
    s_ip[0] = '\0';
    s_token[0] = '\0';
    s_info[0] = '\0';
    s_export_sel = 0U;
    s_scan_pending = false;
    s_scan_count = 0U;

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", hv2_button_cb_, "menu_hs_vault");
        s_buttons_subscribed = true;
    }

    (void)xTaskCreate(hv2_task_, "menu_hs_vault", HV2_TASK_STACK, NULL,
                      HV2_TASK_PRIO, &s_task);
}
