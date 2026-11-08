// SPDX-License-Identifier: GPL-3.0-or-later
/* Passive Wi-Fi beacon consistency monitor.  It never sends frames. */
#include "menu_wifi_guard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define WIFI_GUARD_MAX_APS     (12U)
#define WIFI_GUARD_NAME_HASHES (3U)
#define WIFI_GUARD_STACK       (3328U)
#define WIFI_GUARD_PRIO        (4U)
#define WIFI_GUARD_TICK_MS     (100U)
#define WIFI_GUARD_HOP_MS      (180U)
#define WIFI_GUARD_DIR          "/sdcard/wifi_guard"
#define WIFI_GUARD_ACTIVITY_MAX (5U)
#define WIFI_GUARD_WINDOW_MS    (5000U)

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
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

typedef enum {
    WIFI_GUARD_VIEW_WATCH = 0,
    WIFI_GUARD_VIEW_INFO,
} wifi_guard_view_t;

typedef enum {
    WIFI_GUARD_SEC_OPEN = 0,
    WIFI_GUARD_SEC_LEGACY,
    WIFI_GUARD_SEC_WPA,
    WIFI_GUARD_SEC_WPA2,
    WIFI_GUARD_SEC_WPA3,
} wifi_guard_security_t;

typedef enum {
    WIFI_GUARD_ACTIVITY_AUTH = 0,
    WIFI_GUARD_ACTIVITY_PROBE,
    WIFI_GUARD_ACTIVITY_CSA,
    WIFI_GUARD_ACTIVITY_LOGOFF,
    WIFI_GUARD_ACTIVITY_SAE,
} wifi_guard_activity_type_t;

enum {
    WIFI_GUARD_MARK_MULTI_NAME = 1U << 0,
    WIFI_GUARD_MARK_SECURITY   = 1U << 1,
    WIFI_GUARD_MARK_TEST_OUI   = 1U << 2,
};

typedef struct {
    bool used;
    uint8_t bssid[6];
    char ssid[33];
    uint32_t names[WIFI_GUARD_NAME_HASHES];
    uint8_t name_count;
    uint8_t channel;
    int8_t rssi;
    wifi_guard_security_t security;
    uint8_t markers;
    uint32_t last_seen_ms;
} wifi_guard_entry_t;

typedef struct {
    bool active;
    uint8_t bssid[6];
    uint8_t channel;
    uint16_t count;
    uint32_t window_start_ms;
    uint32_t last_seen_ms;
} wifi_guard_activity_t;

typedef struct {
    bool is_activity;
    uint8_t index;
} wifi_guard_alert_ref_t;

typedef struct { uint8_t button; } wifi_guard_button_t;

typedef struct {
    wifi_guard_entry_t aps[WIFI_GUARD_MAX_APS];
    wifi_guard_activity_t activity[WIFI_GUARD_ACTIVITY_MAX];
    QueueHandle_t buttons;
    TaskHandle_t task;
    portMUX_TYPE lock;
    uint8_t channel_index;
    uint8_t current_channel;
    uint8_t selected_alert;
    uint32_t next_hop_ms;
    uint32_t ap_count;
    uint32_t alert_count;
    uint32_t notice_until_ms;
    char notice[18];
    wifi_guard_view_t view;
    bool active;
    bool radio_ready;
    bool subscribed;
} wifi_guard_session_t;

/* The session and its AP table exist only while WIFI GUARD is open. */
EXT_RAM_BSS_ATTR static wifi_guard_session_t *s_wifi_guard = NULL;
static const char s_wifi_guard_user[] = "menu_wifi_guard";

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t s_wifi_guard_channels[] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
#else
static const uint8_t s_wifi_guard_channels[] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13,
};
#endif

static void wifi_guard_exit_(void);

static uint32_t wifi_guard_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static uint32_t wifi_guard_name_hash_(const uint8_t *name, uint8_t len)
{
    uint32_t value = 2166136261UL;
    for(uint8_t i = 0U; i < len; ++i) value = (value ^ name[i]) * 16777619UL;
    return value;
}

static bool wifi_guard_known_prefix_(const uint8_t mac[6])
{
    return mac[0] == 0x00U && mac[1] == 0x13U && mac[2] == 0x37U;
}

static const char *wifi_guard_security_label_(wifi_guard_security_t security)
{
    switch(security) {
        case WIFI_GUARD_SEC_WPA3: return "WPA3";
        case WIFI_GUARD_SEC_WPA2: return "WPA2";
        case WIFI_GUARD_SEC_WPA: return "WPA";
        case WIFI_GUARD_SEC_LEGACY: return "WEP";
        default: return "OPEN";
    }
}

static const char *wifi_guard_activity_label_(wifi_guard_activity_type_t type)
{
    switch(type) {
        case WIFI_GUARD_ACTIVITY_AUTH: return "AUTH BURST";
        case WIFI_GUARD_ACTIVITY_PROBE: return "PROBE BURST";
        case WIFI_GUARD_ACTIVITY_CSA: return "CHANNEL SWITCH";
        case WIFI_GUARD_ACTIVITY_LOGOFF: return "EAPOL LOGOFF";
        case WIFI_GUARD_ACTIVITY_SAE: return "SAE BURST";
        default: return "UNUSUAL ACTIVITY";
    }
}

static uint16_t wifi_guard_activity_limit_(wifi_guard_activity_type_t type)
{
    switch(type) {
        case WIFI_GUARD_ACTIVITY_AUTH: return 24U;
        case WIFI_GUARD_ACTIVITY_PROBE: return 40U;
        case WIFI_GUARD_ACTIVITY_CSA: return 3U;
        case WIFI_GUARD_ACTIVITY_LOGOFF: return 3U;
        case WIFI_GUARD_ACTIVITY_SAE: return 12U;
        default: return UINT16_MAX;
    }
}

static void wifi_guard_count_alerts_locked_(void)
{
    s_wifi_guard->alert_count = 0U;
    for(uint8_t i = 0U; i < WIFI_GUARD_MAX_APS; ++i) {
        if(s_wifi_guard->aps[i].used && s_wifi_guard->aps[i].markers != 0U) ++s_wifi_guard->alert_count;
    }
    for(uint8_t i = 0U; i < WIFI_GUARD_ACTIVITY_MAX; ++i) {
        if(s_wifi_guard->activity[i].active) ++s_wifi_guard->alert_count;
    }
}

static void wifi_guard_note_activity_(wifi_guard_activity_type_t type, const uint8_t bssid[6], uint8_t channel)
{
    wifi_guard_activity_t *activity;
    const uint32_t now = wifi_guard_now_ms_();
    if(s_wifi_guard == NULL) return;
    activity = &s_wifi_guard->activity[(uint8_t)type];
    portENTER_CRITICAL(&s_wifi_guard->lock);
    if((now - activity->window_start_ms) >= WIFI_GUARD_WINDOW_MS) {
        activity->count = 0U;
        activity->active = false;
        activity->window_start_ms = now;
    }
    if(activity->count != UINT16_MAX) ++activity->count;
    if(bssid != NULL) memcpy(activity->bssid, bssid, sizeof(activity->bssid));
    activity->channel = channel;
    activity->last_seen_ms = now;
    if(activity->count >= wifi_guard_activity_limit_(type)) activity->active = true;
    wifi_guard_count_alerts_locked_();
    portEXIT_CRITICAL(&s_wifi_guard->lock);
}

static void wifi_guard_expire_activity_(void)
{
    const uint32_t now = wifi_guard_now_ms_();
    bool changed = false;
    if(s_wifi_guard == NULL) return;
    portENTER_CRITICAL(&s_wifi_guard->lock);
    for(uint8_t i = 0U; i < WIFI_GUARD_ACTIVITY_MAX; ++i) {
        wifi_guard_activity_t *activity = &s_wifi_guard->activity[i];
        if(activity->active && (now - activity->last_seen_ms) >= WIFI_GUARD_WINDOW_MS) {
            activity->active = false;
            activity->count = 0U;
            changed = true;
        }
    }
    if(changed) wifi_guard_count_alerts_locked_();
    portEXIT_CRITICAL(&s_wifi_guard->lock);
}

static wifi_guard_security_t wifi_guard_security_(const uint8_t *frame, size_t len)
{
    bool privacy;
    wifi_guard_security_t result;
    size_t offset = 36U;
    if(len < offset) return WIFI_GUARD_SEC_OPEN;
    privacy = (frame[34] & 0x10U) != 0U;
    result = privacy ? WIFI_GUARD_SEC_LEGACY : WIFI_GUARD_SEC_OPEN;
    while((offset + 2U) <= len) {
        const uint8_t id = frame[offset];
        const uint8_t ie_len = frame[offset + 1U];
        const uint8_t *ie = &frame[offset + 2U];
        if((offset + 2U + ie_len) > len) break;
        if(id == 48U) {
            /* RSN AKM list: SAE (08) identifies WPA3; other RSN is WPA2. */
            result = WIFI_GUARD_SEC_WPA2;
            if(ie_len >= 12U) {
                for(uint8_t i = 0U; (uint8_t)(i + 3U) < ie_len; ++i) {
                    if(ie[i] == 0x00U && ie[i + 1U] == 0x0FU &&
                       ie[i + 2U] == 0xACU && ie[i + 3U] == 0x08U) {
                        result = WIFI_GUARD_SEC_WPA3;
                        break;
                    }
                }
            }
        } else if(id == 221U && ie_len >= 4U && ie[0] == 0x00U &&
                  ie[1] == 0x50U && ie[2] == 0xF2U && ie[3] == 0x01U) {
            result = WIFI_GUARD_SEC_WPA;
        }
        offset += 2U + ie_len;
    }
    return result;
}

static int wifi_guard_find_slot_locked_(const uint8_t bssid[6])
{
    int free_slot = -1;
    int oldest = 0;
    uint32_t oldest_seen = UINT32_MAX;
    for(uint8_t i = 0U; i < WIFI_GUARD_MAX_APS; ++i) {
        wifi_guard_entry_t *entry = &s_wifi_guard->aps[i];
        if(entry->used && memcmp(entry->bssid, bssid, 6U) == 0) return i;
        if(!entry->used) { if(free_slot < 0) free_slot = (int)i; continue; }
        if(entry->last_seen_ms < oldest_seen) { oldest_seen = entry->last_seen_ms; oldest = (int)i; }
    }
    return (free_slot >= 0) ? free_slot : oldest;
}

static bool wifi_guard_hash_known_(const wifi_guard_entry_t *entry, uint32_t hash)
{
    for(uint8_t i = 0U; i < entry->name_count; ++i) {
        if(entry->names[i] == hash) return true;
    }
    return false;
}

static void wifi_guard_note_beacon_(const uint8_t bssid[6], const uint8_t *ssid,
                                    uint8_t ssid_len, uint8_t channel, int8_t rssi,
                                    wifi_guard_security_t security)
{
    const uint32_t now = wifi_guard_now_ms_();
    const uint32_t hash = wifi_guard_name_hash_(ssid, ssid_len);
    portENTER_CRITICAL(&s_wifi_guard->lock);
    const int slot = wifi_guard_find_slot_locked_(bssid);
    wifi_guard_entry_t *entry = &s_wifi_guard->aps[slot];
    const bool matched = entry->used && (memcmp(entry->bssid, bssid, 6U) == 0);
    if(!matched) {
        const bool was_used = entry->used;
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        memcpy(entry->bssid, bssid, 6U);
        entry->security = security;
        entry->markers = wifi_guard_known_prefix_(bssid) ? WIFI_GUARD_MARK_TEST_OUI : 0U;
        if(!was_used) ++s_wifi_guard->ap_count;
    }
    if(!wifi_guard_hash_known_(entry, hash)) {
        if(entry->name_count < WIFI_GUARD_NAME_HASHES) entry->names[entry->name_count++] = hash;
        if(entry->name_count >= 2U) entry->markers |= WIFI_GUARD_MARK_MULTI_NAME;
    }
    if(entry->ssid[0] == '\0') {
        const uint8_t copy = (ssid_len < 32U) ? ssid_len : 32U;
        memcpy(entry->ssid, ssid, copy);
        entry->ssid[copy] = '\0';
    }
    entry->channel = channel;
    entry->rssi = rssi;
    entry->security = security;
    entry->last_seen_ms = now;

    if(ssid_len != 0U) {
        for(uint8_t i = 0U; i < WIFI_GUARD_MAX_APS; ++i) {
            wifi_guard_entry_t *other = &s_wifi_guard->aps[i];
            if(!other->used || other == entry || other->ssid[0] == '\0') continue;
            if(strlen(other->ssid) == ssid_len && memcmp(other->ssid, ssid, ssid_len) == 0 &&
               other->security != security) {
                entry->markers |= WIFI_GUARD_MARK_SECURITY;
                other->markers |= WIFI_GUARD_MARK_SECURITY;
            }
        }
    }
    wifi_guard_count_alerts_locked_();
    portEXIT_CRITICAL(&s_wifi_guard->lock);
}

static void wifi_guard_rx_(void *buffer, wifi_promiscuous_pkt_type_t packet_type)
{
    const wifi_promiscuous_pkt_t *packet = (const wifi_promiscuous_pkt_t *)buffer;
    const uint8_t *frame;
    size_t length;
    size_t offset;
    uint8_t type;
    uint8_t subtype;
    const uint8_t *bssid;
    if(s_wifi_guard == NULL || !s_wifi_guard->active || packet == NULL) return;
    frame = packet->payload;
    length = packet->rx_ctrl.sig_len;
    if(length < 24U) return;
    type = (uint8_t)((frame[0] >> 2U) & 0x03U);
    subtype = (uint8_t)((frame[0] >> 4U) & 0x0FU);
    bssid = &frame[16];

    if(type == 2U) {
        /* Unprotected data can expose LLC/SNAP + EAPOL Logoff (type 2). */
        const size_t header_len = 24U + ((subtype & 0x08U) ? 2U : 0U);
        if(length >= (header_len + 10U) && frame[header_len] == 0xAAU &&
           frame[header_len + 1U] == 0xAAU && frame[header_len + 2U] == 0x03U &&
           frame[header_len + 6U] == 0x88U && frame[header_len + 7U] == 0x8EU &&
           frame[header_len + 9U] == 0x02U) {
            wifi_guard_note_activity_(WIFI_GUARD_ACTIVITY_LOGOFF, bssid, packet->rx_ctrl.channel);
        }
        return;
    }
    if(type != 0U || packet_type != WIFI_PKT_MGMT) return;
    if(subtype == 11U) {
        const uint16_t algorithm = (length >= 26U) ?
            (uint16_t)frame[24] | ((uint16_t)frame[25] << 8U) : 0xFFFFU;
        wifi_guard_note_activity_((algorithm == 3U) ? WIFI_GUARD_ACTIVITY_SAE : WIFI_GUARD_ACTIVITY_AUTH,
                                  bssid, packet->rx_ctrl.channel);
        return;
    }
    if(subtype == 4U) {
        wifi_guard_note_activity_(WIFI_GUARD_ACTIVITY_PROBE, NULL, packet->rx_ctrl.channel);
        return;
    }
    if(subtype != 8U || length < 38U) return;
    offset = 36U;
    while((offset + 2U) <= length) {
        const uint8_t id = frame[offset];
        const uint8_t ie_len = frame[offset + 1U];
        if((offset + 2U + ie_len) > length) return;
        if(id == 37U) wifi_guard_note_activity_(WIFI_GUARD_ACTIVITY_CSA, bssid, packet->rx_ctrl.channel);
        if(id == 0U) {
            if(ie_len != 0U && ie_len <= 32U) {
                wifi_guard_note_beacon_(bssid, &frame[offset + 2U], ie_len,
                                        packet->rx_ctrl.channel, (int8_t)packet->rx_ctrl.rssi,
                                        wifi_guard_security_(frame, length));
            }
            return;
        }
        offset += 2U + ie_len;
    }
}

static bool wifi_guard_alert_ref_(uint8_t ordinal, wifi_guard_alert_ref_t *out)
{
    uint8_t seen = 0U;
    if(s_wifi_guard == NULL || out == NULL) return false;
    for(uint8_t i = 0U; i < WIFI_GUARD_MAX_APS; ++i) {
        if(s_wifi_guard->aps[i].used && s_wifi_guard->aps[i].markers != 0U) {
            if(seen++ == ordinal) { out->is_activity = false; out->index = i; return true; }
        }
    }
    for(uint8_t i = 0U; i < WIFI_GUARD_ACTIVITY_MAX; ++i) {
        if(s_wifi_guard->activity[i].active && seen++ == ordinal) {
            out->is_activity = true;
            out->index = i;
            return true;
        }
    }
    return false;
}

static const char *wifi_guard_marker_label_(uint8_t markers)
{
    if((markers & WIFI_GUARD_MARK_SECURITY) != 0U) return "SECURITY CONFLICT";
    if((markers & WIFI_GUARD_MARK_MULTI_NAME) != 0U) return "MULTI NETWORK NAMES";
    return "KNOWN TEST DEVICE";
}

static void wifi_guard_print_trim_(int16_t x, int16_t y, const char *text, size_t chars)
{
    char row[22];
    (void)snprintf(row, sizeof(row), "%.*s", (int)chars, (text != NULL) ? text : "");
    poom_arduboy_set_cursor(x, y);
    (void)poom_arduboy_print(row);
}

static void wifi_guard_draw_(void)
{
    uint32_t aps, alerts;
    uint8_t channel, selected;
    wifi_guard_view_t view;
    bool blink;
    char line[22];
    if(s_wifi_guard == NULL) return;
    portENTER_CRITICAL(&s_wifi_guard->lock);
    aps = s_wifi_guard->ap_count;
    alerts = s_wifi_guard->alert_count;
    channel = s_wifi_guard->current_channel;
    selected = s_wifi_guard->selected_alert;
    view = s_wifi_guard->view;
    portEXIT_CRITICAL(&s_wifi_guard->lock);
    blink = ((esp_timer_get_time() / 350000LL) & 1LL) == 0LL;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor((view == WIFI_GUARD_VIEW_INFO) ? 28 : 35, 2);
    (void)poom_arduboy_print(view == WIFI_GUARD_VIEW_INFO ? F("WIFI ALERT") : F("WIFI GUARD"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    if(view == WIFI_GUARD_VIEW_WATCH) {
        (void)snprintf(line, sizeof(line), "CH %3u", (unsigned)channel);
        poom_arduboy_set_cursor(4, 16);
        (void)poom_arduboy_print(line);
        poom_arduboy_draw_circle(117, 19, 3, WHITE);
        if(blink) poom_arduboy_fill_rect(116, 18, 3, 3, WHITE);
        (void)snprintf(line, sizeof(line), "AP %2lu  ALERT %lu", (unsigned long)aps, (unsigned long)alerts);
        poom_arduboy_set_cursor(4, 27);
        (void)poom_arduboy_print(line);
        if(alerts == 0U) {
            poom_arduboy_set_cursor(4, 39);
            (void)poom_arduboy_print(F("WATCHING..."));
        } else {
            wifi_guard_alert_ref_t ref;
            if(wifi_guard_alert_ref_(selected, &ref)) {
                const char *label = ref.is_activity ?
                    wifi_guard_activity_label_((wifi_guard_activity_type_t)ref.index) :
                    wifi_guard_marker_label_(s_wifi_guard->aps[ref.index].markers);
                wifi_guard_print_trim_(4, 39, label, 20U);
            }
        }
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(alerts ? F("A:INFO") : F("A:WAIT"));
        poom_arduboy_set_cursor(82, 56);
        (void)poom_arduboy_print(F("B:STOP"));
    } else {
        wifi_guard_alert_ref_t ref;
        if(wifi_guard_alert_ref_(selected, &ref)) {
            if(ref.is_activity) {
                const wifi_guard_activity_t *activity = &s_wifi_guard->activity[ref.index];
                wifi_guard_print_trim_(4, 15, wifi_guard_activity_label_((wifi_guard_activity_type_t)ref.index), 20U);
                (void)snprintf(line, sizeof(line), "CH %u  %u / 5 SEC", (unsigned)activity->channel,
                               (unsigned)activity->count);
                wifi_guard_print_trim_(4, 24, line, 20U);
                if(activity->bssid[0] == 0U && activity->bssid[1] == 0U && activity->bssid[2] == 0U) {
                    wifi_guard_print_trim_(4, 33, "BROADCAST OBSERVED", 20U);
                } else {
                    (void)snprintf(line, sizeof(line), "BSSID %02X:%02X:%02X", activity->bssid[0],
                                   activity->bssid[1], activity->bssid[2]);
                    wifi_guard_print_trim_(4, 33, line, 20U);
                }
                if((int32_t)(wifi_guard_now_ms_() - s_wifi_guard->notice_until_ms) < 0) {
                    wifi_guard_print_trim_(4, 42, s_wifi_guard->notice, 20U);
                } else {
                    wifi_guard_print_trim_(4, 42, "PASSIVE OBSERVATION", 20U);
                }
            } else {
                const wifi_guard_entry_t *entry = &s_wifi_guard->aps[ref.index];
                wifi_guard_print_trim_(4, 15, wifi_guard_marker_label_(entry->markers), 20U);
                wifi_guard_print_trim_(4, 24, entry->ssid[0] ? entry->ssid : "HIDDEN", 20U);
                (void)snprintf(line, sizeof(line), "%02X:%02X:%02X  %s", entry->bssid[0], entry->bssid[1], entry->bssid[2],
                               wifi_guard_security_label_(entry->security));
                wifi_guard_print_trim_(4, 33, line, 20U);
                if((int32_t)(wifi_guard_now_ms_() - s_wifi_guard->notice_until_ms) < 0) {
                    (void)snprintf(line, sizeof(line), "%s", s_wifi_guard->notice);
                } else {
                    (void)snprintf(line, sizeof(line), "CH %u  NAMES %u", (unsigned)entry->channel, (unsigned)entry->name_count);
                }
                poom_arduboy_set_cursor(4, 42);
                (void)poom_arduboy_print(line);
            }
        }
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:SAVE"));
        poom_arduboy_set_cursor(82, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }
    poom_arduboy_display();
}

static bool wifi_guard_export_(void)
{
    char path[64];
    FILE *file;
    if(sd_card_is_not_mounted()) {
        sd_card_begin();
        if(sd_card_mount() != ESP_OK) return false;
    }
    if(sd_card_create_dir(WIFI_GUARD_DIR) != ESP_OK) return false;
    (void)snprintf(path, sizeof(path), WIFI_GUARD_DIR "/watch_%08lu.csv", (unsigned long)wifi_guard_now_ms_());
    file = fopen(path, "w");
    if(file == NULL) return false;
    (void)fputs("kind,ssid,bssid,channel,rssi,security,warning,count\n", file);
    for(uint8_t i = 0U; i < WIFI_GUARD_MAX_APS; ++i) {
        const wifi_guard_entry_t *entry = &s_wifi_guard->aps[i];
        if(!entry->used || entry->markers == 0U) continue;
        (void)fprintf(file, "ap,\"%s\",%02X:%02X:%02X:%02X:%02X:%02X,%u,%d,%s,%s,%u\n",
                      entry->ssid, entry->bssid[0], entry->bssid[1], entry->bssid[2], entry->bssid[3], entry->bssid[4], entry->bssid[5],
                      (unsigned)entry->channel, (int)entry->rssi, wifi_guard_security_label_(entry->security),
                      wifi_guard_marker_label_(entry->markers), (unsigned)entry->name_count);
    }
    for(uint8_t i = 0U; i < WIFI_GUARD_ACTIVITY_MAX; ++i) {
        const wifi_guard_activity_t *activity = &s_wifi_guard->activity[i];
        if(!activity->active) continue;
        (void)fprintf(file, "activity,,%02X:%02X:%02X:%02X:%02X:%02X,%u,,,%s,%u\n",
                      activity->bssid[0], activity->bssid[1], activity->bssid[2], activity->bssid[3],
                      activity->bssid[4], activity->bssid[5], (unsigned)activity->channel,
                      wifi_guard_activity_label_((wifi_guard_activity_type_t)i), (unsigned)activity->count);
    }
    return fclose(file) == 0;
}

static void wifi_guard_buttons_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t event;
    wifi_guard_button_t button;
    (void)user;
    if(s_wifi_guard == NULL || msg == NULL || msg->len < sizeof(event) || s_wifi_guard->buttons == NULL) return;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    button.button = event.button;
    (void)xQueueSend(s_wifi_guard->buttons, &button, 0U);
}

static bool wifi_guard_radio_start_(void)
{
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    if(poom_wifi_ctrl_init_null() != ESP_OK) return false;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    if(esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
       poom_wifi_ctrl_set_promiscuous_rx_cb(wifi_guard_rx_) != ESP_OK ||
       poom_wifi_ctrl_set_promiscuous(true) != ESP_OK) {
        (void)poom_wifi_ctrl_set_promiscuous(false);
        (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
        (void)poom_wifi_ctrl_deinit();
        return false;
    }
    s_wifi_guard->radio_ready = true;
    return true;
}

static void wifi_guard_radio_stop_(void)
{
    if(s_wifi_guard == NULL || !s_wifi_guard->radio_ready) return;
    (void)poom_wifi_ctrl_set_promiscuous(false);
    (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
    (void)poom_wifi_ctrl_deinit();
    s_wifi_guard->radio_ready = false;
}

static void wifi_guard_exit_(void)
{
    wifi_guard_session_t *state = s_wifi_guard;
    if(state == NULL) return;
    state->active = false;
    wifi_guard_radio_stop_();
    if(state->subscribed) (void)poom_sbus_unsubscribe_cb("input/button", wifi_guard_buttons_, (void *)s_wifi_guard_user);
    if(state->buttons != NULL) vQueueDelete(state->buttons);
    s_wifi_guard = NULL;
    free(state);
    { const uint8_t token = 1U; (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U); }
}

static void wifi_guard_task_(void *arg)
{
    (void)arg;
    while(s_wifi_guard != NULL && s_wifi_guard->active) {
        wifi_guard_button_t button;
        if(xQueueReceive(s_wifi_guard->buttons, &button, pdMS_TO_TICKS(WIFI_GUARD_TICK_MS)) == pdTRUE) {
            if(button.button == BTN_B) {
                if(s_wifi_guard->view == WIFI_GUARD_VIEW_INFO) s_wifi_guard->view = WIFI_GUARD_VIEW_WATCH;
                else { wifi_guard_exit_(); continue; }
            } else if(button.button == BTN_A) {
                if(s_wifi_guard->view == WIFI_GUARD_VIEW_WATCH) {
                    if(s_wifi_guard->alert_count != 0U) s_wifi_guard->view = WIFI_GUARD_VIEW_INFO;
                } else {
                    const bool saved = wifi_guard_export_();
                    (void)snprintf(s_wifi_guard->notice, sizeof(s_wifi_guard->notice),
                                   "%s", saved ? "REPORT SAVED" : "SD NOT READY");
                    s_wifi_guard->notice_until_ms = wifi_guard_now_ms_() + 1500U;
                }
            } else if(button.button == BTN_UP || button.button == BTN_DOWN) {
                if(s_wifi_guard->alert_count != 0U) {
                    if(button.button == BTN_UP) s_wifi_guard->selected_alert =
                        (uint8_t)((s_wifi_guard->selected_alert + s_wifi_guard->alert_count - 1U) % s_wifi_guard->alert_count);
                    else s_wifi_guard->selected_alert = (uint8_t)((s_wifi_guard->selected_alert + 1U) % s_wifi_guard->alert_count);
                }
            }
        }
        if(s_wifi_guard != NULL && s_wifi_guard->active &&
           (int32_t)(wifi_guard_now_ms_() - s_wifi_guard->next_hop_ms) >= 0) {
            s_wifi_guard->current_channel = s_wifi_guard_channels[s_wifi_guard->channel_index];
            (void)poom_wifi_ctrl_set_channel(s_wifi_guard->current_channel);
            s_wifi_guard->channel_index = (uint8_t)((s_wifi_guard->channel_index + 1U) %
                (sizeof(s_wifi_guard_channels) / sizeof(s_wifi_guard_channels[0])));
            s_wifi_guard->next_hop_ms = wifi_guard_now_ms_() + WIFI_GUARD_HOP_MS;
        }
        wifi_guard_expire_activity_();
        wifi_guard_draw_();
    }
    vTaskDelete(NULL);
}

void menu_wifi_guard_show(void)
{
    wifi_guard_exit_();
    s_wifi_guard = heap_caps_calloc(1U, sizeof(*s_wifi_guard), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_wifi_guard == NULL) s_wifi_guard = calloc(1U, sizeof(*s_wifi_guard));
    if(s_wifi_guard == NULL) { const uint8_t token = 1U; (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U); return; }
    s_wifi_guard->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_wifi_guard->buttons = xQueueCreate(8U, sizeof(wifi_guard_button_t));
    s_wifi_guard->active = true;
    if(s_wifi_guard->buttons == NULL || !poom_sbus_subscribe_cb("input/button", wifi_guard_buttons_, (void *)s_wifi_guard_user)) {
        wifi_guard_exit_();
        return;
    }
    s_wifi_guard->subscribed = true;
    s_wifi_guard->current_channel = s_wifi_guard_channels[0];
    s_wifi_guard->next_hop_ms = wifi_guard_now_ms_();
    wifi_guard_draw_();
    if(!wifi_guard_radio_start_() ||
       xTaskCreate(wifi_guard_task_, "menu_wifi_guard", WIFI_GUARD_STACK, NULL, WIFI_GUARD_PRIO, &s_wifi_guard->task) != pdPASS) {
        wifi_guard_exit_();
    }
}
