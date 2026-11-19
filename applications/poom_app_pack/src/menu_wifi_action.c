// SPDX-License-Identifier: GPL-3.0-or-later
/* Native POOM action-first UI. Radio discovery is scoped to the chosen action. */
#include "menu_wifi_action.h"

#include <stdbool.h>
#include <stdint.h>
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
#include "poom_sbus.h"
#include "poom_wifi_attacks.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_detector.h"
#include "poom_wifi_marauder.h"
#include "poom_wifi_scanner.h"

#define ACTION_CLIENTS POOM_MARAUDER_MAX_CLIENTS
#define ACTION_TICK_MS 100U
#define ACTION_RESUME_TOPIC "poom/menu/resume"

enum { ACTION_A = 0, ACTION_B = 1, ACTION_LEFT = 2, ACTION_RIGHT = 3,
       ACTION_UP = 4, ACTION_DOWN = 5 };

typedef enum {
    VIEW_SCAN, VIEW_APS, VIEW_SCOPE, VIEW_CLIENTS, VIEW_READY, VIEW_RUNNING, VIEW_RESULT, VIEW_ERROR
} action_view_t;

typedef enum { CLIENT_SCOPE_ALL, CLIENT_SCOPE_CHOOSE } client_scope_t;

typedef struct {
    bool scan_done;
    uint8_t button;
    esp_err_t error;
} action_event_t;

typedef struct {
    poom_wifi_action_t action;
    action_view_t view;
    QueueHandle_t events;
    wifi_ap_record_t aps[POOM_WIFI_SCANNER_MAX_AP];
    uint16_t ap_count;
    uint16_t selected_ap;
    poom_wifi_detector_record_t clients[ACTION_CLIENTS];
    poom_wifi_detector_record_t *client_ptrs[ACTION_CLIENTS];
    bool client_observed[ACTION_CLIENTS];
    size_t client_count;
    size_t selected_client;
    client_scope_t client_scope;
    poom_marauder_config_t config;
    poom_marauder_status_t status;
    esp_err_t error;
    uint32_t discover_started_ms;
    uint32_t last_snapshot_ms;
    bool scanning;
    bool cancel_scan;
    bool monitoring;
    bool subscribed;
} action_session_t;

static action_session_t *s_action;
/* A callback already copied by SBUS may outlive unsubscribe. It only accesses
 * this guarded queue, never session memory. Cleanup clears it before deletion. */
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_input_queue;
static bool s_stop_action;
static const char s_input_user[] = "menu_wifi_action";

static const char *const s_titles[] = {
    "BAD MSG", "ASSOC SLEEP", "QUIET TIME", "CHANNEL SWITCH", "SAE COMMIT FLOOD",
    "PROBE REQUEST FLOOD", "AP CLONE SPAM",
};
static const poom_marauder_mode_t s_modes[] = {
    POOM_MARAUDER_BAD_MSG, POOM_MARAUDER_ASSOC_SLEEP,
    POOM_MARAUDER_QUIET, POOM_MARAUDER_CSA, POOM_MARAUDER_SAE_COMMIT,
    POOM_MARAUDER_PROBE_REQUEST, POOM_MARAUDER_AP_CLONE,
};
static const uint8_t s_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
#if CONFIG_IDF_TARGET_ESP32C5
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
#endif
};

static uint32_t now_ms_(void) { return (uint32_t)(esp_timer_get_time() / 1000LL); }
static bool cancelled_(action_session_t *s)
{
    portENTER_CRITICAL(&s_input_lock);
    const bool cancelled = s->cancel_scan || s_stop_action;
    portEXIT_CRITICAL(&s_input_lock);
    return cancelled;
}

bool menu_wifi_action_is_active(void)
{
    portENTER_CRITICAL(&s_input_lock);
    const bool active = s_action != NULL;
    portEXIT_CRITICAL(&s_input_lock);
    return active;
}

void menu_wifi_action_stop(void)
{
    portENTER_CRITICAL(&s_input_lock);
    s_stop_action = true;
    portEXIT_CRITICAL(&s_input_lock);
    while(menu_wifi_action_is_active()) vTaskDelay(pdMS_TO_TICKS(10U));
}
static bool needs_clients_(const action_session_t *s)
{
    return s->config.mode == POOM_MARAUDER_BAD_MSG || s->config.mode == POOM_MARAUDER_ASSOC_SLEEP;
}
static int selected_client_index_(const action_session_t *s, const uint8_t mac[6])
{
    for(size_t i = 0U; i < s->config.client_count; ++i) {
        if(memcmp(s->config.clients[i], mac, 6U) == 0) return (int)i;
    }
    return -1;
}

static void toggle_client_(action_session_t *s)
{
    if(s->selected_client >= s->client_count) return;
    const uint8_t *mac = s->clients[s->selected_client].bssid;
    const int index = selected_client_index_(s, mac);
    if(index >= 0) {
        const size_t remaining = s->config.client_count - (size_t)index - 1U;
        if(remaining) memmove(s->config.clients[index], s->config.clients[index + 1U], remaining * 6U);
        --s->config.client_count;
        memset(s->config.clients[s->config.client_count], 0, 6U);
    } else if(s->config.client_count < ACTION_CLIENTS) {
        memcpy(s->config.clients[s->config.client_count++], mac, 6U);
    }
}
static bool supports_sae_(const wifi_ap_record_t *ap)
{
    /* SAE is WPA3-Personal, not WPA3-Enterprise. */
    return ap->authmode == WIFI_AUTH_WPA3_PSK || ap->authmode == WIFI_AUTH_WPA2_WPA3_PSK;
}
static void print_(int y, const char *text)
{
    char line[22];
    (void)snprintf(line, sizeof(line), "%.21s", text);
    poom_arduboy_set_cursor(1, y);
    (void)poom_arduboy_print(line);
}
static void ap_name_(char *out, size_t len, const wifi_ap_record_t *ap)
{
    (void)snprintf(out, len, "%.*s", 32, ap->ssid[0] ? (const char *)ap->ssid : "<hidden>");
    /* SSIDs are bytes supplied by nearby radios, not display commands. */
    for(char *p = out; *p; ++p) if((unsigned char)*p < 32U || (unsigned char)*p > 126U) *p = '?';
}

static void draw_(action_session_t *s)
{
    char line[64];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    print_(1, s_titles[s->action]);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 10, INVERT);
    switch(s->view) {
    case VIEW_SCAN:
        print_(16, s->cancel_scan ? "Finishing scan..." : "Scanning networks...");
        print_(29, s->action == POOM_WIFI_ACTION_SAE_COMMIT ? "Looking for WPA3" : "2.4 / 5 GHz on C5");
        print_(42, "No prior scan needed");
        print_(56, "B:BACK");
        break;
    case VIEW_APS:
        if(!s->ap_count) {
            print_(16, s->action == POOM_WIFI_ACTION_SAE_COMMIT ? "No WPA3 APs found" : "No networks found");
            print_(30, "A: scan again");
        } else {
            const uint16_t first = (s->selected_ap / 2U) * 2U;
            for(uint16_t i = first; i < s->ap_count && i < first + 2U; ++i) {
                const wifi_ap_record_t *ap = &s->aps[i];
                const int y = 13 + (i - first) * 20;
                char name[34];
                ap_name_(name, sizeof(name), ap);
                (void)snprintf(line, sizeof(line), "%c%.20s", i == s->selected_ap ? '>' : ' ', name);
                print_(y, line);
                (void)snprintf(line, sizeof(line), " CH%3u %ddB %02X%02X%02X", ap->primary,
                               ap->rssi, ap->bssid[3], ap->bssid[4], ap->bssid[5]);
                print_(y + 9, line);
            }
        }
        print_(56, "A:PICK >:SCAN B:BACK");
        break;
    case VIEW_SCOPE:
        ap_name_(line, sizeof(line), &s->config.ap);
        print_(13, line);
        print_(28, s->client_scope == CLIENT_SCOPE_ALL ? "> ALL CLIENTS" : "  ALL CLIENTS");
        print_(40, s->client_scope == CLIENT_SCOPE_CHOOSE ? "> CHOOSE CLIENTS" : "  CHOOSE CLIENTS");
        print_(56, "UP/DN A:PICK B:BACK");
        break;
    case VIEW_CLIENTS:
        ap_name_(line, sizeof(line), &s->config.ap);
        print_(13, line);
        if(s->client_scope == CLIENT_SCOPE_CHOOSE) {
            (void)snprintf(line, sizeof(line), "%u clients, %u picked", (unsigned)s->client_count,
                           (unsigned)s->config.client_count);
        } else {
            (void)snprintf(line, sizeof(line), "%u clients / %lus", (unsigned)s->client_count,
                           (unsigned long)((now_ms_() - s->discover_started_ms) / 1000U));
        }
        print_(24, line);
        if(!s->client_count) {
            print_(35, "Listening for clients");
            print_(45, "Use lab client Wi-Fi");
        } else if(s->client_scope == CLIENT_SCOPE_CHOOSE) {
            const uint8_t *mac = s->clients[s->selected_client].bssid;
            (void)snprintf(line, sizeof(line), "[%c]%02X:%02X:%02X:%02X:%02X:%02X",
                           selected_client_index_(s, mac) >= 0 ? 'x' : ' ',
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            print_(35, line);
            (void)snprintf(line, sizeof(line), "%u/%u  %s", (unsigned)s->selected_client + 1U,
                           (unsigned)s->client_count,
                           s->client_observed[s->selected_client] ? "UP/DOWN: browse" : "seen earlier");
            print_(45, line);
        } else {
            print_(35, "Selecting all clients");
            print_(45, "Only this network");
        }
        if(s->client_scope == CLIENT_SCOPE_CHOOSE) {
            print_(56, s->config.client_count ? "A:MARK >:NEXT B:BACK" : "A:MARK        B:BACK");
        } else print_(56, s->client_count ? "A:USE CLIENTS B:BACK" : "B:BACK");
        break;
    case VIEW_READY:
        ap_name_(line, sizeof(line), &s->config.ap);
        print_(13, line);
        (void)snprintf(line, sizeof(line), "CH%u AP %02X:%02X:%02X", s->config.ap.primary,
                       s->config.ap.bssid[3], s->config.ap.bssid[4], s->config.ap.bssid[5]);
        print_(23, line);
        (void)snprintf(line, sizeof(line), "%lus%s  UP/DOWN: time", (unsigned long)(s->config.duration_ms / 1000U),
                       s->config.duration_ms < 60000U ? " " : "");
        print_(33, line);
        if(s->action == POOM_WIFI_ACTION_CHANNEL_SWITCH) {
            (void)snprintf(line, sizeof(line), "To CH%u  LEFT/RIGHT", s->config.csa_channel);
        } else if(needs_clients_(s)) {
            (void)snprintf(line, sizeof(line), "%u client%s selected", (unsigned)s->config.client_count,
                           s->config.client_count == 1U ? "" : "s");
        } else {
            (void)snprintf(line, sizeof(line), "Ready on this network");
        }
        print_(43, line);
        print_(56, "A:START        B:BACK");
        break;
    case VIEW_RUNNING:
    case VIEW_RESULT:
        print_(13, s->view == VIEW_RUNNING ? "RUNNING" : "STOPPED");
        (void)snprintf(line, sizeof(line), "Sent %lu  Err %lu", (unsigned long)s->status.sent,
                       (unsigned long)s->status.errors);
        print_(24, line);
        (void)snprintf(line, sizeof(line), "%lus / %lus  CH%u", (unsigned long)(s->status.elapsed_ms / 1000U),
                       (unsigned long)(s->config.duration_ms / 1000U), s->config.ap.primary);
        print_(35, line);
        if(s->status.last_error != ESP_OK) print_(45, esp_err_to_name(s->status.last_error));
        else print_(45, "Driver TX counters");
        print_(56, s->view == VIEW_RUNNING ? "B:STOP" : "A:REPEAT       B:BACK");
        break;
    case VIEW_ERROR:
        print_(16, "Unable to continue");
        print_(29, esp_err_to_name(s->error));
        (void)snprintf(line, sizeof(line), "Error 0x%lX", (unsigned long)s->error);
        print_(42, line);
        print_(56, "A:SCAN AGAIN   B:BACK");
        break;
    }
    poom_arduboy_display();
}

static void on_button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    button_event_msg_t input;
    if(!msg || msg->len < sizeof(input)) return;
    memcpy(&input, msg->data, sizeof(input));
    if(input.event != BUTTON_SINGLE_CLICK) return;
    const action_event_t event = {.button = input.button};
    portENTER_CRITICAL(&s_input_lock);
    if(s_input_queue) (void)xQueueSend(s_input_queue, &event, 0U);
    portEXIT_CRITICAL(&s_input_lock);
}

static void stop_monitor_(action_session_t *s)
{
    if(s->monitoring) {
        (void)poom_wifi_detector_monitor_stop();
        s->monitoring = false;
    }
}

static void scan_task_(void *arg)
{
    action_session_t *s = arg;
    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 40, .max = 120},
    };
    const esp_err_t result = cancelled_(s) ? ESP_ERR_INVALID_STATE : poom_wifi_scanner_scan_configured(&scan);
    s->ap_count = 0U;
    if(result == ESP_OK) {
        const poom_wifi_scanner_ap_records_t *found = poom_wifi_scanner_get_ap_records();
        for(uint16_t i = 0U; i < found->count && s->ap_count < POOM_WIFI_SCANNER_MAX_AP; ++i) {
            if(s->action == POOM_WIFI_ACTION_SAE_COMMIT && !supports_sae_(&found->records[i])) continue;
            if((s->action == POOM_WIFI_ACTION_PROBE_REQUEST || s->action == POOM_WIFI_ACTION_AP_CLONE) &&
               found->records[i].ssid[0] == 0) continue;
            /* Keep equal SSIDs on different BSSIDs/bands as distinct targets. */
            s->aps[s->ap_count++] = found->records[i];
        }
    }
    const action_event_t event = {.scan_done = true, .error = result};
    const QueueHandle_t events = s->events;
    /* The UI may free s after receiving this completion. No more s accesses. */
    (void)xQueueSend(events, &event, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void begin_scan_(action_session_t *s)
{
    stop_monitor_(s);
    poom_wifi_marauder_stop();
    s->selected_ap = 0U;
    portENTER_CRITICAL(&s_input_lock);
    s->cancel_scan = false;
    portEXIT_CRITICAL(&s_input_lock);
    s->scanning = true;
    s->view = VIEW_SCAN;
    draw_(s);
    if(xTaskCreate(scan_task_, "action_scan", 4096U, s, 4U, NULL) != pdPASS) {
        s->scanning = false;
        s->error = ESP_ERR_NO_MEM;
        s->view = VIEW_ERROR;
    }
}

static void discover_clients_(action_session_t *s)
{
    s->client_count = 0U;
    s->selected_client = 0U;
    s->config.client_count = 0U;
    memset(s->config.clients, 0, sizeof(s->config.clients));
    memset(s->client_observed, 0, sizeof(s->client_observed));
    esp_err_t err = poom_wifi_detector_monitor_start(POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS,
                                                   NULL, 0U, ACTION_CLIENTS);
    s->monitoring = err == ESP_OK;
    if(err == ESP_OK) err = poom_wifi_detector_monitor_select_ap(s->config.ap.primary, s->config.ap.bssid);
    /* The monitor retries tuning internally; surface an initially rejected
     * channel instead of leaving this screen listening indefinitely. */
    if(err == ESP_OK) err = poom_wifi_ctrl_set_channel(s->config.ap.primary);
    if(err != ESP_OK) {
        stop_monitor_(s);
        s->error = err;
        s->view = VIEW_ERROR;
        return;
    }
    s->discover_started_ms = now_ms_();
    s->last_snapshot_ms = 0U;
    s->view = VIEW_CLIENTS;
}

static void use_clients_(action_session_t *s)
{
    if(s->client_scope == CLIENT_SCOPE_ALL) {
        s->config.client_count = s->client_count;
        for(size_t i = 0U; i < s->client_count; ++i) {
            memcpy(s->config.clients[i], s->clients[i].bssid, 6U);
        }
    }
    if(!s->config.client_count) return;
    stop_monitor_(s);
    s->view = VIEW_READY;
}

static void refresh_clients_(action_session_t *s)
{
    if(now_ms_() - s->last_snapshot_ms < 500U) return;
    s->last_snapshot_ms = now_ms_();
    uint8_t selected_mac[6] = {0};
    if(s->client_count) memcpy(selected_mac, s->clients[s->selected_client].bssid, 6U);
    size_t count = 0U;
    const esp_err_t err = poom_wifi_detector_monitor_snapshot(s->client_ptrs, ACTION_CLIENTS, &count);
    if(err != ESP_OK) {
        s->error = err;
        stop_monitor_(s);
        s->view = VIEW_ERROR;
        return;
    }
    for(size_t i = 0U; i < count; ++i) s->client_observed[i] = true;
    /* Selection is a set of MACs, independent of snapshot order or pruning.
     * Keep a selected-but-unseen station browsable so it can be deselected. */
    if(s->client_scope == CLIENT_SCOPE_CHOOSE) {
        for(size_t picked = 0U; picked < s->config.client_count; ++picked) {
            size_t index = 0U;
            while(index < count && memcmp(s->clients[index].bssid, s->config.clients[picked], 6U) != 0) ++index;
            if(index < count) continue;
            if(count < ACTION_CLIENTS) ++count;
            else {
                for(index = 0U; index < count; ++index) {
                    if(selected_client_index_(s, s->clients[index].bssid) < 0) break;
                }
                if(index == count) continue;
            }
            memset(&s->clients[index], 0, sizeof(s->clients[index]));
            memcpy(s->clients[index].bssid, s->config.clients[picked], 6U);
            s->clients[index].channel = s->config.ap.primary;
            s->client_observed[index] = false;
        }
    }
    s->client_count = count;
    s->selected_client = 0U;
    for(size_t i = 0U; i < count; ++i) {
        if(memcmp(selected_mac, s->clients[i].bssid, 6U) == 0) s->selected_client = i;
    }
    /* ALL CLIENTS collects briefly, then copies the set to the ready screen. */
    if(count && s->client_scope == CLIENT_SCOPE_ALL && now_ms_() - s->discover_started_ms >= 3000U) use_clients_(s);
}

static void change_channel_(action_session_t *s, bool next)
{
    const size_t n = sizeof(s_channels) / sizeof(s_channels[0]);
    size_t index = 0U;
    for(size_t i = 0U; i < n; ++i) if(s_channels[i] == s->config.csa_channel) index = i;
    for(size_t i = 0U; i < n; ++i) {
        index = next ? (index + 1U) % n : (index + n - 1U) % n;
        const uint8_t ch = s_channels[index];
        if(ch != s->config.ap.primary && (ch > 14U) == (s->config.ap.primary > 14U) &&
           poom_wifi_ctrl_channel_is_valid(ch)) {
            s->config.csa_channel = ch;
            return;
        }
    }
}

/* Returns false only once all transient workers have relinquished the session. */
static bool button_(action_session_t *s, uint8_t button)
{
    if(s->scanning) {
        if(button == ACTION_B && !s->cancel_scan) {
            portENTER_CRITICAL(&s_input_lock);
            s->cancel_scan = true;
            portEXIT_CRITICAL(&s_input_lock);
            (void)esp_wifi_scan_stop();
        }
        return true;
    }
    if(button == ACTION_B) {
        switch(s->view) {
        case VIEW_RUNNING:
            poom_wifi_marauder_stop();
            poom_wifi_marauder_get_status(&s->status);
            s->view = VIEW_RESULT;
            break;
        case VIEW_CLIENTS:
            stop_monitor_(s);
            s->view = VIEW_SCOPE;
            break;
        case VIEW_SCOPE: s->view = VIEW_APS; break;
        case VIEW_READY:
            s->view = needs_clients_(s) ? VIEW_SCOPE : VIEW_APS;
            break;
        case VIEW_RESULT: s->view = VIEW_APS; break;
        default: return false;
        }
    } else if(button == ACTION_A) {
        switch(s->view) {
        case VIEW_APS:
            if(!s->ap_count) { begin_scan_(s); break; }
            s->config.ap = s->aps[s->selected_ap];
            s->config.csa_channel = s->config.ap.primary;
            change_channel_(s, true);
            if(needs_clients_(s)) {
                s->client_scope = CLIENT_SCOPE_ALL;
                s->config.client_count = 0U;
                s->view = VIEW_SCOPE;
            }
            else s->view = VIEW_READY;
            break;
        case VIEW_SCOPE: discover_clients_(s); break;
        case VIEW_CLIENTS:
            if(s->client_scope == CLIENT_SCOPE_CHOOSE) toggle_client_(s);
            else if(s->client_count) use_clients_(s);
            break;
        case VIEW_READY:
            (void)poom_wifi_ctrl_deinit();
            s->error = poom_wifi_marauder_start(&s->config);
            s->view = s->error == ESP_OK ? VIEW_RUNNING : VIEW_ERROR;
            poom_wifi_marauder_get_status(&s->status);
            break;
        case VIEW_RESULT: s->view = VIEW_READY; break;
        case VIEW_ERROR: begin_scan_(s); break;
        default: break;
        }
    } else if(button == ACTION_UP || button == ACTION_DOWN) {
        const bool next = button == ACTION_DOWN;
        if(s->view == VIEW_APS && s->ap_count) {
            s->selected_ap = (s->selected_ap + (next ? 1U : s->ap_count - 1U)) % s->ap_count;
        } else if(s->view == VIEW_SCOPE) {
            s->client_scope = s->client_scope == CLIENT_SCOPE_ALL ? CLIENT_SCOPE_CHOOSE : CLIENT_SCOPE_ALL;
        } else if(s->view == VIEW_CLIENTS && s->client_count && s->client_scope == CLIENT_SCOPE_CHOOSE) {
            s->selected_client = (s->selected_client + (next ? 1U : s->client_count - 1U)) % s->client_count;
        } else if(s->view == VIEW_READY) {
            const uint32_t duration = s->config.duration_ms;
            s->config.duration_ms = next ? (duration == 15000U ? 30000U : duration == 30000U ? 60000U : 15000U) :
                                          (duration == 60000U ? 30000U : duration == 30000U ? 15000U : 60000U);
        }
    } else if(button == ACTION_LEFT || button == ACTION_RIGHT) {
        if(s->view == VIEW_APS && button == ACTION_RIGHT) begin_scan_(s);
        else if(s->view == VIEW_CLIENTS && s->client_scope == CLIENT_SCOPE_CHOOSE && button == ACTION_RIGHT) {
            use_clients_(s);
        }
        else if(s->view == VIEW_READY && s->action == POOM_WIFI_ACTION_CHANNEL_SWITCH) {
            change_channel_(s, button == ACTION_RIGHT);
        }
    }
    return true;
}

static void finish_(action_session_t *s)
{
    portENTER_CRITICAL(&s_input_lock);
    s_input_queue = NULL;
    portEXIT_CRITICAL(&s_input_lock);
    if(s->subscribed) (void)poom_sbus_unsubscribe_cb("input/button", on_button_, (void *)s_input_user);
    stop_monitor_(s);
    poom_wifi_marauder_stop();
    (void)poom_wifi_ctrl_deinit();
    if(s->events) vQueueDelete(s->events);
    portENTER_CRITICAL(&s_input_lock);
    s_action = NULL;
    portEXIT_CRITICAL(&s_input_lock);
    free(s);
    const uint8_t token = 1U;
    (void)poom_sbus_publish(ACTION_RESUME_TOPIC, &token, sizeof(token), 0U);
}

static void ui_task_(void *arg)
{
    action_session_t *s = arg;
    poom_wifi_attacks_stop_all();
    begin_scan_(s);
    bool active = true;
    while(active) {
        portENTER_CRITICAL(&s_input_lock);
        const bool stop = s_stop_action;
        portEXIT_CRITICAL(&s_input_lock);
        if(stop && !s->scanning) break;
        if(s->scanning && cancelled_(s)) {
            /* Also catches B/CLI stop pressed before the scan worker began. */
            (void)esp_wifi_scan_stop();
        }
        action_event_t event;
        if(xQueueReceive(s->events, &event, pdMS_TO_TICKS(ACTION_TICK_MS)) == pdTRUE) {
            if(event.scan_done) {
                s->scanning = false;
                if(cancelled_(s)) { active = false; continue; }
                s->error = event.error;
                s->view = event.error == ESP_OK ? VIEW_APS : VIEW_ERROR;
            } else active = button_(s, event.button);
        }
        if(!active) break;
        if(s->view == VIEW_CLIENTS) refresh_clients_(s);
        if(s->view == VIEW_RUNNING) {
            poom_wifi_marauder_get_status(&s->status);
            if(!s->status.running) s->view = VIEW_RESULT;
        }
        draw_(s);
    }
    finish_(s);
    vTaskDelete(NULL);
}

void menu_wifi_action_show(poom_wifi_action_t action)
{
    if(menu_wifi_action_is_active() || action < POOM_WIFI_ACTION_BAD_MSG || action >= POOM_WIFI_ACTION_COUNT) return;
    action_session_t *s = heap_caps_calloc(1U, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s) s = calloc(1U, sizeof(*s));
    if(!s) {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(ACTION_RESUME_TOPIC, &token, sizeof(token), 0U);
        return;
    }
    portENTER_CRITICAL(&s_input_lock);
    s_action = s;
    s_stop_action = false;
    portEXIT_CRITICAL(&s_input_lock);
    s->action = action;
    s->config.mode = s_modes[action];
    s->config.duration_ms = 30000U;
    s->config.interval_ms = 100U;
    for(size_t i = 0U; i < ACTION_CLIENTS; ++i) s->client_ptrs[i] = &s->clients[i];
    s->events = xQueueCreate(12U, sizeof(action_event_t));
    if(!s->events) { finish_(s); return; }
    portENTER_CRITICAL(&s_input_lock);
    s_input_queue = s->events;
    portEXIT_CRITICAL(&s_input_lock);
    s->subscribed = poom_sbus_subscribe_cb("input/button", on_button_, (void *)s_input_user);
    if(!s->subscribed || xTaskCreate(ui_task_, "wifi_action_ui", 4096U, s, 4U, NULL) != pdPASS) finish_(s);
}
