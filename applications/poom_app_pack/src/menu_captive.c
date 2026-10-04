// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_captive.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sd_browser.h"
#include "poom_sbus.h"
#include "poom_secrets_store.h"
#include "sd_card.h"
#include "poom_ui_keyboard.h"
#include "poom_wifi_captive.h"
#include "poom_wifi_scanner.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif
#define HEADER_H (11)
#define VISIBLE_ROWS (4U)
#define SCROLL_MS (350U)
#define SCROLL_GAP (3U)
#define REFRESH_MS (1000U)
#define TEXT_COLS (21U)
#define LIST_COLS (19U)
#define CLIENT_COLS (17U)
#define CAPTIVE_AP_NAME_DEFAULT "coffee"
#define CAPTIVE_CFG_KEY_AP_NAME "captive_ap_name"
#define CAPTIVE_CFG_KEY_PORTAL "captive_portal"
#define CAPTIVE_PORTAL_STORE_LEN (128U)

typedef enum {
    VIEW_SELECT, VIEW_SETTINGS, VIEW_MSG, VIEW_KEYBOARD, VIEW_SCANNING,
    VIEW_SCAN_LIST, VIEW_STATUS, VIEW_CLIENTS, VIEW_CLIENT_INFO
} captive_view_t;

typedef struct {
    bool scan_done;
    uint8_t button;
    esp_err_t scan_status;
} captive_ui_event_t;

/* Allocate view state only for the lifetime of this menu. The bus callback
 * accesses only the queue, under s_event_lock, so shutdown can detach and
 * delete it even if the bus already copied the callback before unsubscribe. */
typedef struct {
    bool running;
    bool exit;
    bool cancel_scan;
    bool monitor_error;
    bool open_portal_browser;
    captive_view_t view;
    unsigned option;
    unsigned settings_option;
    unsigned status_option;
    unsigned scan_selected;
    unsigned scan_scroll;
    unsigned client_selected;
    unsigned client_scroll;
    size_t client_count;
    uint32_t selection_ms;
    uint32_t refresh_ms;
    char message[22];
    char ssid[33];
    char keyboard_ssid[33];
    char ap_name[33];
    char portal[CAPTIVE_PORTAL_STORE_LEN];
    poom_ui_keyboard_t keyboard;
    poom_wifi_captive_client_t clients[POOM_WIFI_CAPTIVE_MAX_CLIENTS];
    poom_wifi_captive_client_t detail;
} captive_ui_t;

static captive_ui_t *s_ui;
static QueueHandle_t s_events;
static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_accept_buttons;
static bool s_worker_active;
static bool s_reopen_settings;

static void menu_captive_button_cb_(const poom_sbus_msg_t *msg, void *ctx);

static uint32_t now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void change_view_(captive_view_t view)
{
    s_ui->view = view;
    s_ui->selection_ms = now_ms_();
}

static void message_(const char *text)
{
    (void)snprintf(s_ui->message, sizeof(s_ui->message), "%.21s", text);
    change_view_(VIEW_MSG);
}

static const char *option_label_(unsigned option)
{
    switch(option) {
        case 0U: return "Start";
        case 1U: return "Settings";
        default: return "Scan SSID";
    }
}

static const char *settings_label_(unsigned option)
{
    return (option == 0U) ? "AP Name" : "Portal";
}

/* Same 350 ms marquee and three-space gap as Wi-Fi Scan. Bound every row
 * before printing so the font never wraps into a neighbouring row/footer. */
static void format_text_(char *out, const char *text, size_t cols, bool selected)
{
    size_t length = strlen(text);
    size_t phase = 0U;
    if(selected && (length > cols))
        phase = ((now_ms_() - s_ui->selection_ms) / SCROLL_MS) % (length + SCROLL_GAP);
    size_t n = (length < cols) ? length : cols;
    for(size_t i = 0; i < n; ++i)
    {
        size_t source = (phase + i) % (length + SCROLL_GAP);
        unsigned char c = (source < length) ? (unsigned char)text[source] : ' ';
        out[i] = ((c >= 32U) && (c <= 126U)) ? (char)c : '?';
    }
    out[n] = '\0';
}

static void print_(int x, int y, const char *text)
{
    poom_arduboy_set_cursor(x, y);
    (void)poom_arduboy_print(text);
}

static void header_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    print_((ARDUBOY_WIDTH - (int)strlen(title) * 6) / 2, 2, title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

static void footer_(const char *action, const char *back)
{
    print_(0, 56, action);
    print_(72, 56, back);
}

static void normalize_(unsigned count, unsigned *selected, unsigned *scroll)
{
    if(count == 0U) { *selected = 0U; *scroll = 0U; return; }
    if(*selected >= count) *selected = count - 1U;
    if(*selected < *scroll) *scroll = *selected;
    if(*selected >= *scroll + VISIBLE_ROWS) *scroll = *selected - VISIBLE_ROWS + 1U;
    unsigned max_scroll = (count > VISIBLE_ROWS) ? count - VISIBLE_ROWS : 0U;
    if(*scroll > max_scroll) *scroll = max_scroll;
}

static void arrows_(unsigned count, unsigned scroll)
{
    if(scroll > 0U) poom_arduboy_fill_triangle(124, 11, 120, 15, 127, 15, WHITE);
    if(scroll + VISIBLE_ROWS < count)
        poom_arduboy_fill_triangle(120, 50, 127, 50, 124, 54, WHITE);
}

static const char *portal_label_(const char *path)
{
    const char *slash;
    if((path == NULL) || (path[0] == '\0')) return CAPTIVE_PORTAL_DEFAULT_NAME;
    slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

static bool client_label_repeats_(unsigned index)
{
    if((s_ui == NULL) || (index >= s_ui->client_count)) return false;
    const char *label = poom_wifi_captive_client_label(&s_ui->clients[index]);
    for(unsigned i = 0U; i < s_ui->client_count; ++i)
    {
        if((i != index) &&
           (strcmp(label, poom_wifi_captive_client_label(&s_ui->clients[i])) == 0))
        {
            return true;
        }
    }
    return false;
}

static const char *client_ip_tail_(const poom_wifi_captive_client_t *client)
{
    if((client == NULL) || (client->ip[0] == '\0')) return NULL;
    const char *last = strrchr(client->ip, '.');
    if((last == NULL) || (last == client->ip) || (last[1] == '\0')) return client->ip;
    const char *p = last - 1;
    while((p > client->ip) && (*p != '.')) --p;
    return (*p == '.') ? (p + 1) : client->ip;
}

static void format_client_list_label_(char *out, size_t out_len, size_t cols, unsigned index, bool selected)
{
    char label[TEXT_COLS + 1U];
    const poom_wifi_captive_client_t *client = &s_ui->clients[index];
    const char *ip_tail = client_ip_tail_(client);
    const char *base = (client->device_type[0] != '\0')
        ? client->device_type
        : poom_wifi_captive_client_label(client);

    if((client->device_type[0] != '\0') && (ip_tail != NULL))
    {
        (void)snprintf(label, sizeof(label), "%.12s %s", base, ip_tail);
    }
    else if(client_label_repeats_(index))
    {
        if(ip_tail != NULL)
            (void)snprintf(label, sizeof(label), "%.12s %s", base, ip_tail);
        else
            (void)snprintf(label, sizeof(label), "%.12s %02X%02X", base,
                           client->mac[4], client->mac[5]);
    }
    else
    {
        (void)snprintf(label, sizeof(label), "%s", base);
    }

    if(cols >= out_len) cols = out_len - 1U;
    format_text_(out, label, cols, selected);
}

static void config_defaults_(void)
{
    (void)snprintf(s_ui->ap_name, sizeof(s_ui->ap_name), "%s", CAPTIVE_AP_NAME_DEFAULT);
    (void)snprintf(s_ui->portal, sizeof(s_ui->portal), "%s", CAPTIVE_PORTAL_DEFAULT_NAME);
}

static void config_load_(void)
{
    size_t len;
    config_defaults_();
    if(poom_secrets_init() != ESP_OK) return;

    len = sizeof(s_ui->ap_name);
    if((poom_secrets_get_str(CAPTIVE_CFG_KEY_AP_NAME, s_ui->ap_name, &len) != ESP_OK) ||
       (s_ui->ap_name[0] == '\0'))
    {
        (void)snprintf(s_ui->ap_name, sizeof(s_ui->ap_name), "%s", CAPTIVE_AP_NAME_DEFAULT);
    }
    s_ui->ap_name[sizeof(s_ui->ap_name) - 1U] = '\0';

    len = sizeof(s_ui->portal);
    if((poom_secrets_get_str(CAPTIVE_CFG_KEY_PORTAL, s_ui->portal, &len) != ESP_OK) ||
       (s_ui->portal[0] == '\0'))
    {
        (void)snprintf(s_ui->portal, sizeof(s_ui->portal), "%s", CAPTIVE_PORTAL_DEFAULT_NAME);
    }
    s_ui->portal[sizeof(s_ui->portal) - 1U] = '\0';
}

static void config_save_ap_(void)
{
    if((s_ui == NULL) || (s_ui->ap_name[0] == '\0')) return;
    if(poom_secrets_init() == ESP_OK)
        (void)poom_secrets_set_str(CAPTIVE_CFG_KEY_AP_NAME, s_ui->ap_name);
}

static void config_save_portal_value_(const char *portal)
{
    if((portal == NULL) || (portal[0] == '\0')) return;
    if(poom_secrets_init() == ESP_OK)
        (void)poom_secrets_set_str(CAPTIVE_CFG_KEY_PORTAL, portal);
}

static bool portal_file_filter_(const char *name, bool is_directory, void *user_ctx)
{
    (void)user_ctx;
    if(is_directory) return true;
    if((name == NULL) || (name[0] == '\0')) return false;
    const char *dot = strrchr(name, '.');
    return (dot != NULL) && ((strcasecmp(dot, ".html") == 0) || (strcasecmp(dot, ".htm") == 0));
}

static void portal_browser_exit_cb_(void *user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    s_reopen_settings = true;
    menu_captive_display();
}

static void portal_browser_file_cb_(const char *abs_path, void *user_ctx)
{
    const char prefix[] = "/sdcard/portals/";
    const char *saved = abs_path;
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);

    if((abs_path != NULL) && (strncmp(abs_path, prefix, sizeof(prefix) - 1U) == 0))
        saved = abs_path + sizeof(prefix) - 1U;
    config_save_portal_value_(saved);
    s_reopen_settings = true;
    menu_captive_display();
}

static void launch_portal_browser_(void)
{
    poom_sd_browser_config_t cfg = {
        .start_dir = "/sdcard/portals",
        .header = "PORTAL",
        .filter = portal_file_filter_,
        .filter_ctx = NULL,
        .on_file_selected = portal_browser_file_cb_,
        .on_file_selected_ctx = NULL,
    };

    sd_card_begin();
    if(sd_card_is_not_mounted()) (void)sd_card_mount();
    if(!sd_card_is_not_mounted()) (void)sd_card_create_dir(CAPTIVE_PORTALS_FOLDER_PATH);

    (void)poom_sd_browser_set_exit_callback(portal_browser_exit_cb_, NULL);
    if(poom_sd_browser_start_ex(&cfg) != ESP_OK)
    {
        s_reopen_settings = true;
    }
}

static unsigned scan_count_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
    if(records == NULL) return 0U;
    return (records->count <= POOM_WIFI_SCANNER_MAX_AP) ? records->count : POOM_WIFI_SCANNER_MAX_AP;
}

static void refresh_clients_(void)
{
    /* Preserve identity before refreshing the existing buffer in place. The
     * backend leaves that buffer untouched on error, avoiding a second table. */
    uint8_t selected_mac[6] = {0};
    if(s_ui->client_selected < s_ui->client_count)
        memcpy(selected_mac, s_ui->clients[s_ui->client_selected].mac, sizeof(selected_mac));
    size_t count = 0U;
    s_ui->refresh_ms = now_ms_();
    esp_err_t err = poom_wifi_captive_get_clients(s_ui->clients, POOM_WIFI_CAPTIVE_MAX_CLIENTS, &count);
    s_ui->monitor_error = (err != ESP_OK);
    if(s_ui->monitor_error) return;

    s_ui->client_count = count;
    for(size_t i = 0; i < count; ++i)
        if(memcmp(selected_mac, s_ui->clients[i].mac, sizeof(selected_mac)) == 0) s_ui->client_selected = i;
    normalize_((unsigned)count, &s_ui->client_selected, &s_ui->client_scroll);
    if((s_ui->view == VIEW_CLIENTS) && (count > 0U) &&
       (memcmp(selected_mac, s_ui->clients[s_ui->client_selected].mac, 6) != 0))
        s_ui->selection_ms = now_ms_();

    if(s_ui->view == VIEW_CLIENT_INFO)
    {
        s_ui->detail.online = false;
        s_ui->detail.rssi_known = false;
        for(size_t i = 0; i < count; ++i)
        {
            if(memcmp(s_ui->detail.mac, s_ui->clients[i].mac, 6) == 0)
            {
                s_ui->detail = s_ui->clients[i];
                break;
            }
        }
    }
}

static void render_(void)
{
    char line[TEXT_COLS + 1U];
    if(s_ui->view == VIEW_KEYBOARD)
    {
        poom_ui_keyboard_draw(&s_ui->keyboard);
        return;
    }
    if(s_ui->view == VIEW_SELECT)
    {
        header_("CAPTIVE");
        poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);
        for(unsigned i = 0U; i < 3U; ++i)
        {
            int y = 16 + (int)i * 10;
            print_(4, y, option_label_(i));
            if(i == s_ui->option) poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, 9, INVERT);
        }
        footer_("A:SELECT", "B:EXIT");
    }
    else if(s_ui->view == VIEW_SETTINGS)
    {
        header_("SETTINGS");
        poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);
        for(unsigned i = 0U; i < 2U; ++i)
        {
            int y = 16 + (int)i * 10;
            print_(4, y, settings_label_(i));
            if(i == s_ui->settings_option)
                poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, 9, INVERT);
        }
        format_text_(line,
                     (s_ui->settings_option == 0U) ? s_ui->ap_name : portal_label_(s_ui->portal),
                     TEXT_COLS, true);
        print_(2, 42, line);
        footer_("A:CHANGE", "B:BACK");
    }
    else if(s_ui->view == VIEW_MSG)
    {
        header_("CAPTIVE");
        print_(0, 24, s_ui->message);
        footer_("A:OK", "B:BACK");
    }
    else if(s_ui->view == VIEW_SCANNING)
    {
        header_("SCAN");
        print_(22, 26, s_ui->cancel_scan ? "Cancelling..." : "Scanning...");
        footer_("", "B:BACK");
    }
    else if(s_ui->view == VIEW_SCAN_LIST)
    {
        unsigned count = scan_count_();
        (void)snprintf(line, sizeof(line), "SCAN %u", count);
        header_(line);
        normalize_(count, &s_ui->scan_selected, &s_ui->scan_scroll);
        for(unsigned row = 0U; (row < VISIBLE_ROWS) && (s_ui->scan_scroll + row < count); ++row)
        {
            unsigned index = s_ui->scan_scroll + row;
            const wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record(index);
            char ssid[33] = {0};
            if(ap != NULL) memcpy(ssid, ap->ssid, 32U);
            format_text_(line, (ssid[0] != '\0') ? ssid : "<hidden>", LIST_COLS, index == s_ui->scan_selected);
            int y = 14 + (int)row * 10;
            print_(2, y, line);
            if(index == s_ui->scan_selected) poom_arduboy_fill_rect(0, y - 1, ARDUBOY_WIDTH, 9, INVERT);
        }
        arrows_(count, s_ui->scan_scroll);
        footer_("A:SEL", "B:BACK");
    }
    else if(s_ui->view == VIEW_STATUS)
    {
        header_("CAPTIVE");
        format_text_(line, s_ui->ssid[0] ? s_ui->ssid : s_ui->ap_name, TEXT_COLS, true);
        print_(0, 14, line);
        (void)snprintf(line, sizeof(line), "Portal:%.13s", portal_label_(s_ui->portal));
        print_(2, 24, line);
        (void)snprintf(line, sizeof(line), "Clients: %u", (unsigned)s_ui->client_count);
        print_(2, 34, s_ui->monitor_error ? "Clients: ---" : line);
        print_(2, 44, "Stop");
        poom_arduboy_fill_rect(0, 33 + (int)s_ui->status_option * 10, ARDUBOY_WIDTH, 9, INVERT);
        footer_("A:SELECT", "B:EXIT");
    }
    else if(s_ui->view == VIEW_CLIENTS)
    {
        (void)snprintf(line, sizeof(line), "CLIENTS %u", (unsigned)s_ui->client_count);
        header_(s_ui->monitor_error ? "CLIENTS ---" : line);
        if(s_ui->monitor_error) print_(4, 26, "Monitor unavailable");
        else if(s_ui->client_count == 0U) print_(28, 26, "No clients");
        else
        {
            for(unsigned row = 0; (row < VISIBLE_ROWS) && (s_ui->client_scroll + row < s_ui->client_count); ++row)
            {
                unsigned index = s_ui->client_scroll + row;
                int y = 14 + (int)row * 10;
                /* A small filled disc, independent of font/Unicode support. */
                poom_arduboy_fill_rect(3, y + 2, 5, 3, WHITE);
                poom_arduboy_fill_rect(4, y + 1, 3, 5, WHITE);
                format_client_list_label_(line, sizeof(line), CLIENT_COLS, index, index == s_ui->client_selected);
                print_(12, y, line);
                if(index == s_ui->client_selected) poom_arduboy_fill_rect(0, y - 1, ARDUBOY_WIDTH, 9, INVERT);
            }
            arrows_((unsigned)s_ui->client_count, s_ui->client_scroll);
        }
        footer_((s_ui->client_count && !s_ui->monitor_error) ? "A:INFO" : "", "B:BACK");
    }
    else if(s_ui->view == VIEW_CLIENT_INFO)
    {
        header_("CLIENT INFO");
        format_text_(line, poom_wifi_captive_client_label(&s_ui->detail), TEXT_COLS, true);
        print_(0, 14, line);
        (void)snprintf(line, sizeof(line), "IP:%s", s_ui->detail.ip[0] ? s_ui->detail.ip : "---");
        print_(0, 22, line);
        (void)snprintf(line, sizeof(line), "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                       s_ui->detail.mac[0], s_ui->detail.mac[1], s_ui->detail.mac[2],
                       s_ui->detail.mac[3], s_ui->detail.mac[4], s_ui->detail.mac[5]);
        print_(0, 30, line);
        if(s_ui->detail.rssi_known && !s_ui->monitor_error)
            (void)snprintf(line, sizeof(line), "RSSI:%d dBm", (int)s_ui->detail.rssi);
        else (void)snprintf(line, sizeof(line), "RSSI:---");
        print_(0, 38, line);
        (void)snprintf(line, sizeof(line), "%.10s%s%s", s_ui->detail.device_type,
                       s_ui->detail.device_type[0] ? " " : "",
                       s_ui->monitor_error ? "UNKNOWN" : (s_ui->detail.online ? "ONLINE" : "OFFLINE"));
        print_(0, 46, line);
        footer_("", "B:BACK");
    }
    poom_arduboy_display();
}

static void start_(void)
{
    const char *ap_name = (s_ui->ap_name[0] != '\0') ? s_ui->ap_name : CAPTIVE_AP_NAME_DEFAULT;
    const char *portal = (s_ui->portal[0] != '\0') ? s_ui->portal : CAPTIVE_PORTAL_DEFAULT_NAME;

    poom_wifi_captive_set_ap_clone(ap_name, true);
    poom_wifi_captive_set_portal_file(portal);
    (void)snprintf(s_ui->ssid, sizeof(s_ui->ssid), "%.32s", ap_name);
    sd_card_begin();
    if(poom_wifi_captive_start() != ESP_OK)
    {
        message_("Portal start failed");
        return;
    }
    s_ui->running = true;
    s_ui->status_option = 0U;
    s_ui->client_count = 0U;
    memset(&s_ui->detail, 0, sizeof(s_ui->detail));
    change_view_(VIEW_STATUS);
    refresh_clients_();
}

static void scan_worker_(void *arg)
{
    QueueHandle_t events = (QueueHandle_t)arg;
    (void)poom_wifi_scanner_clear_ap_records();
    captive_ui_event_t result = {.scan_done = true, .scan_status = poom_wifi_scanner_scan()};
    (void)xQueueSend(events, &result, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void start_scan_(void)
{
    s_ui->cancel_scan = false;
    change_view_(VIEW_SCANNING);
    if(xTaskCreate(scan_worker_, "captive_scan", 4096, s_events, 4, NULL) != pdPASS)
        message_("Scan task failed");
}

static void move_(unsigned *selected, unsigned count, bool up)
{
    unsigned old = *selected;
    if(up && (*selected > 0U)) --*selected;
    else if(!up && (*selected + 1U < count)) ++*selected;
    if(old != *selected) s_ui->selection_ms = now_ms_();
}

static void button_(uint8_t button)
{
    if(s_ui->view == VIEW_SCANNING)
    {
        if(button == BUTTON_B)
        {
            s_ui->cancel_scan = true;
            (void)esp_wifi_scan_stop();
        }
        return;
    }
    if(button == BUTTON_B)
    {
        if(s_ui->view == VIEW_CLIENT_INFO) change_view_(VIEW_CLIENTS);
        else if(s_ui->view == VIEW_CLIENTS) change_view_(VIEW_STATUS);
        else if(s_ui->view == VIEW_STATUS) s_ui->exit = true;
        else if(s_ui->view == VIEW_SETTINGS) change_view_(VIEW_SELECT);
        else if(s_ui->view == VIEW_KEYBOARD) change_view_(VIEW_SETTINGS);
        else if(s_ui->view == VIEW_SELECT) s_ui->exit = true;
        else change_view_(VIEW_SELECT);
        return;
    }
    if(s_ui->view == VIEW_MSG)
    {
        change_view_(VIEW_SELECT);
        return;
    }
    if(s_ui->view == VIEW_KEYBOARD)
    {
        if(poom_ui_keyboard_handle_button(&s_ui->keyboard, button) == POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            if(s_ui->keyboard.text_len == 0U) message_("Name empty");
            else
            {
                (void)snprintf(s_ui->ap_name, sizeof(s_ui->ap_name), "%.32s", s_ui->keyboard_ssid);
                config_save_ap_();
                change_view_(VIEW_SETTINGS);
            }
        }
        return;
    }
    if(button == BUTTON_A)
    {
        if(s_ui->view == VIEW_SELECT)
        {
            if(s_ui->option == 0U) start_();
            else if(s_ui->option == 1U) change_view_(VIEW_SETTINGS);
            else start_scan_();
        }
        else if(s_ui->view == VIEW_SETTINGS)
        {
            if(s_ui->settings_option == 0U)
            {
                (void)snprintf(s_ui->keyboard_ssid, sizeof(s_ui->keyboard_ssid), "%.32s", s_ui->ap_name);
                poom_ui_keyboard_init(&s_ui->keyboard, s_ui->keyboard_ssid, sizeof(s_ui->keyboard_ssid), "AP NAME");
                change_view_(VIEW_KEYBOARD);
            }
            else
            {
                s_ui->open_portal_browser = true;
                s_ui->exit = true;
            }
        }
        else if(s_ui->view == VIEW_SCAN_LIST)
        {
            const wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record(s_ui->scan_selected);
            if((ap == NULL) || (ap->ssid[0] == 0U)) message_("Hidden SSID");
            else
            {
                char ssid[33] = {0};
                memcpy(ssid, ap->ssid, 32U);
                (void)snprintf(s_ui->ap_name, sizeof(s_ui->ap_name), "%.32s", ssid);
                config_save_ap_();
                s_ui->settings_option = 0U;
                change_view_(VIEW_SETTINGS);
            }
        }
        else if(s_ui->view == VIEW_STATUS)
        {
            if(s_ui->status_option == 0U)
            {
                refresh_clients_();
                change_view_(VIEW_CLIENTS);
            }
            else
            {
                poom_wifi_captive_stop();
                s_ui->running = false;
                change_view_(VIEW_SELECT);
            }
        }
        else if((s_ui->view == VIEW_CLIENTS) && !s_ui->monitor_error && (s_ui->client_selected < s_ui->client_count))
        {
            s_ui->detail = s_ui->clients[s_ui->client_selected];
            change_view_(VIEW_CLIENT_INFO);
        }
        return;
    }
    bool up = (button == BUTTON_UP) || (button == BUTTON_LEFT);
    bool down = (button == BUTTON_DOWN) || (button == BUTTON_RIGHT);
    if(!up && !down) return;
    if(s_ui->view == VIEW_SELECT) move_(&s_ui->option, 3U, up);
    else if(s_ui->view == VIEW_SETTINGS) move_(&s_ui->settings_option, 2U, up);
    else if(s_ui->view == VIEW_SCAN_LIST) move_(&s_ui->scan_selected, scan_count_(), up);
    else if(s_ui->view == VIEW_STATUS) move_(&s_ui->status_option, 2U, up);
    else if(s_ui->view == VIEW_CLIENTS)
    {
        move_(&s_ui->client_selected, (unsigned)s_ui->client_count, up);
        normalize_((unsigned)s_ui->client_count, &s_ui->client_selected, &s_ui->client_scroll);
    }
}

/* Call only before the worker starts or after it has finished using the
 * context. Scanning must complete (including cancellation) before menu exit. */
static void release_ui_(void)
{
    portENTER_CRITICAL(&s_event_lock);
    s_accept_buttons = false;
    QueueHandle_t events = s_events;
    s_events = NULL;
    portEXIT_CRITICAL(&s_event_lock);
    (void)poom_sbus_unsubscribe_cb("input/button", menu_captive_button_cb_, NULL);
    if(events != NULL) vQueueDelete(events);
    free(s_ui);
    s_ui = NULL;
    portENTER_CRITICAL(&s_event_lock);
    s_worker_active = false;
    portEXIT_CRITICAL(&s_event_lock);
}

static void menu_worker_(void *arg)
{
    (void)arg;
    const bool reopen_settings = s_reopen_settings;
    s_reopen_settings = false;
    s_ui->running = false;
    s_ui->exit = false;
    s_ui->open_portal_browser = false;
    s_ui->option = s_ui->settings_option = s_ui->status_option = 0U;
    s_ui->scan_selected = s_ui->scan_scroll = 0U;
    s_ui->client_selected = s_ui->client_scroll = 0U;
    s_ui->client_count = 0U;
    s_ui->monitor_error = false;
    s_ui->ssid[0] = '\0';
    config_load_();
    change_view_(reopen_settings ? VIEW_SETTINGS : VIEW_SELECT);
    render_();
    while(!s_ui->exit)
    {
        captive_ui_event_t event;
        bool redraw = false;
        if(xQueueReceive(s_events, &event, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if(event.scan_done)
            {
                if(s_ui->cancel_scan) change_view_(VIEW_SELECT);
                else if(event.scan_status != ESP_OK) message_("Scan failed");
                else if(scan_count_() == 0U) message_("No networks");
                else
                {
                    s_ui->scan_selected = s_ui->scan_scroll = 0U;
                    change_view_(VIEW_SCAN_LIST);
                }
            }
            else button_(event.button);
            redraw = true;
        }
        if(s_ui->exit) break;
        if(s_ui->running && ((uint32_t)(now_ms_() - s_ui->refresh_ms) >= REFRESH_MS))
        {
            refresh_clients_();
            redraw = true;
        }
        /* Periodic redraw advances the existing Wi-Fi marquee. */
        if(redraw || (s_ui->view == VIEW_SETTINGS) || (s_ui->view == VIEW_SCAN_LIST) ||
           (s_ui->view == VIEW_CLIENTS) || (s_ui->view == VIEW_CLIENT_INFO) ||
           (s_ui->view == VIEW_STATUS)) render_();
    }

    const bool open_portal_browser = s_ui->open_portal_browser;
    if(s_ui->running) poom_wifi_captive_stop();
    release_ui_();
    if(open_portal_browser)
    {
        launch_portal_browser_();
    }
    else
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
    vTaskDelete(NULL);
}

static void menu_captive_button_cb_(const poom_sbus_msg_t *msg, void *ctx)
{
    (void)ctx;
    if((msg == NULL) || (msg->len < sizeof(button_event_msg_t))) return;
    button_event_msg_t button;
    memcpy(&button, msg->data, sizeof(button));
    if(button.event != BUTTON_SINGLE_CLICK) return;
    captive_ui_event_t event = {.button = button.button};
    portENTER_CRITICAL(&s_event_lock);
    if(s_accept_buttons) (void)xQueueSend(s_events, &event, 0);
    portEXIT_CRITICAL(&s_event_lock);
}

void menu_captive_display(void)
{
    portENTER_CRITICAL(&s_event_lock);
    bool active = s_worker_active;
    s_worker_active = true;
    portEXIT_CRITICAL(&s_event_lock);
    if(active) return;
    /* Prefer external RAM; retain support for boards without PSRAM. */
    s_ui = heap_caps_calloc(1, sizeof(*s_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_ui == NULL) s_ui = calloc(1, sizeof(*s_ui));
    if(s_ui != NULL) s_events = xQueueCreate(16, sizeof(captive_ui_event_t));
    if(s_events != NULL)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_captive_button_cb_, NULL))
        {
            portENTER_CRITICAL(&s_event_lock);
            s_accept_buttons = true;
            portEXIT_CRITICAL(&s_event_lock);
            if(xTaskCreate(menu_worker_, "captive_menu", 6144, NULL, 4, NULL) == pdPASS) return;
        }
    }
    release_ui_();
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}
