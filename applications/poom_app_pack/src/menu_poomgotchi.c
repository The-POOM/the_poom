// SPDX-License-Identifier: MIT
#include "menu_poomgotchi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_pcap_manager.h"
#include "poom_sbus.h"
#include "poom_wifi_attacks.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_scanner.h"

#define POOM_MENU_RESUME_TOPIC        "poom/menu/resume"
#define POOMGOTCHI_REFRESH_MS         (250U)
#define POOMGOTCHI_STACK              (2560U)
#define POOMGOTCHI_TASK_PRIO          (4U)
#define POOMGOTCHI_TARGET_MAX         (8U)
#define POOMGOTCHI_CAPTURE_FILE_LEN   (21U)
#define POOMGOTCHI_CAPTURE_MS         (15000U)
#define POOMGOTCHI_RESULT_MS          (1500U)
#define POOMGOTCHI_ASSIST_WAIT_MS     (6000U)
#define POOMGOTCHI_DEAUTH_MIN_MS      (10000U)
#define POOMGOTCHI_HEADER_H           (11)
#define POOMGOTCHI_BOX_Y              (12)
#define POOMGOTCHI_BOX_H              (40)
#define POOMGOTCHI_TEXT_X             (4)
#define POOMGOTCHI_ROW0_Y             (16)
#define POOMGOTCHI_ROW_STEP           (10)
#define POOMGOTCHI_HISTORY_EAPOL      (1U << 0)
#define POOMGOTCHI_HISTORY_CAPTURED   (1U << 1)
#define POOMGOTCHI_HISTORY_NONE       (0xFFU)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum {
    POOMGOTCHI_SCREEN_SCAN = 0,
    POOMGOTCHI_SCREEN_CAPTURE,
    POOMGOTCHI_SCREEN_RESULT,
    POOMGOTCHI_SCREEN_IDLE,
    POOMGOTCHI_SCREEN_ERROR,
    POOMGOTCHI_SCREEN_SUMMARY,
} poomgotchi_screen_t;

typedef enum {
    POOMGOTCHI_POLICY_OFF = 0,
    POOMGOTCHI_POLICY_MANUAL,
    POOMGOTCHI_POLICY_ASSIST,
    POOMGOTCHI_POLICY_COUNT,
} poomgotchi_policy_t;

typedef struct {
    uint8_t scan_index;
    uint8_t score;
    uint8_t history_index;
} poomgotchi_target_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t attempts;
    uint8_t flags;
    uint8_t clients_seen;
    bool used;
} poomgotchi_history_t;

typedef struct {
    poomgotchi_target_t targets[POOMGOTCHI_TARGET_MAX];
    poomgotchi_history_t history[POOMGOTCHI_TARGET_MAX];
    uint8_t history_replace_index;
} poomgotchi_session_t;

typedef struct {
    uint16_t aps_seen;
    uint16_t targets_tried;
    uint16_t handshakes;
    uint16_t pmkids;
    uint16_t eapol_targets;
    uint16_t clients_seen;
    uint16_t deauth_seen;
    uint16_t deauths;
} poomgotchi_stats_t;

static bool s_active;
static bool s_buttons_subscribed;
static bool s_scan_in_progress;
static bool s_capture_running;
static bool s_assist_sent;
static bool s_dirty;
static volatile bool s_exit_requested;
static volatile bool s_stop_requested;
static volatile bool s_scan_requested;
static volatile bool s_restart_requested;
static volatile bool s_deauth_requested;
static volatile int8_t s_policy_delta;
static TaskHandle_t s_task;
static const char s_user[] = "menu_poomgotchi";
static poomgotchi_screen_t s_screen;
static poomgotchi_policy_t s_policy;
static poomgotchi_session_t *s_session;
static uint8_t s_target_count;
static uint8_t s_target_index;
static poomgotchi_stats_t s_stats;
static esp_err_t s_last_error;
static uint32_t s_capture_started_ms;
static uint32_t s_result_until_ms;
static uint32_t s_last_deauth_ms;
static bool s_result_captured;
static char s_capture_file[POOMGOTCHI_CAPTURE_FILE_LEN];

static void poomgotchi_button_cb_(const poom_sbus_msg_t *msg, void *user);
static void poomgotchi_task_(void *arg);

static uint32_t poomgotchi_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void poomgotchi_request_render_(void)
{
    s_dirty = true;
    if(s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

static bool poomgotchi_elapsed_(uint32_t since_ms, uint32_t duration_ms)
{
    return (uint32_t)(poomgotchi_now_ms_() - since_ms) >= duration_ms;
}

static bool poomgotchi_deadline_reached_(uint32_t deadline_ms)
{
    return (int32_t)(poomgotchi_now_ms_() - deadline_ms) >= 0;
}

static poomgotchi_target_t *poomgotchi_target_meta_(void)
{
    if((s_session == NULL) || (s_target_index >= s_target_count)) {
        return NULL;
    }
    return &s_session->targets[s_target_index];
}

static wifi_ap_record_t *poomgotchi_target_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();
    const poomgotchi_target_t *target = poomgotchi_target_meta_();

    if((records == NULL) || (target == NULL) || (target->scan_index >= records->count)) {
        return NULL;
    }
    return &records->records[target->scan_index];
}

static poomgotchi_history_t *poomgotchi_target_history_(void)
{
    poomgotchi_target_t *target = poomgotchi_target_meta_();

    if((s_session == NULL) || (target == NULL) ||
       (target->history_index >= POOMGOTCHI_TARGET_MAX)) {
        return NULL;
    }
    return &s_session->history[target->history_index];
}

static const char *poomgotchi_policy_label_(void)
{
    switch(s_policy) {
        case POOMGOTCHI_POLICY_OFF: return "OFF";
        case POOMGOTCHI_POLICY_ASSIST: return "ASSIST";
        case POOMGOTCHI_POLICY_MANUAL:
        default: return "MANUAL";
    }
}

static void poomgotchi_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(31, 2);
    (void)poom_arduboy_print(F("POOMGOTCHI"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, POOMGOTCHI_HEADER_H, INVERT);
}

static void poomgotchi_draw_footer_(const char *left, const char *right)
{
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(left);
    if(right != NULL) {
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(right);
    }
}

static void poomgotchi_draw_target_name_(const wifi_ap_record_t *target, int16_t y)
{
    char name[21];

    poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, y);
    if((target == NULL) || (target->ssid[0] == '\0')) {
        (void)poom_arduboy_print(F("<HIDDEN>"));
        return;
    }
    (void)snprintf(name, sizeof(name), "%.20s", (const char *)target->ssid);
    (void)poom_arduboy_print(name);
}

static const char *poomgotchi_error_label_(void)
{
    if(s_last_error == ESP_ERR_INVALID_STATE) return "SNIFFER BUSY";
    if(s_last_error == ESP_ERR_NO_MEM) return "NO MEMORY";
    if(s_last_error == ESP_ERR_NOT_FOUND) return "SD NOT READY";
    return esp_err_to_name(s_last_error);
}

static uint8_t poomgotchi_display_count_(uint16_t count)
{
    return (count > 99U) ? 99U : (uint8_t)count;
}

static void poomgotchi_draw_(void)
{
    char line[24];
    const wifi_ap_record_t *target = poomgotchi_target_();

    poom_arduboy_clear();
    poomgotchi_draw_header_();
    poom_arduboy_draw_rect(0, POOMGOTCHI_BOX_Y, ARDUBOY_WIDTH, POOMGOTCHI_BOX_H, WHITE);

    if(s_screen == POOMGOTCHI_SCREEN_SCAN) {
        const int16_t sweep_x = (int16_t)(8U + ((poomgotchi_now_ms_() / 70U) % 100U));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(F("(o_o) SCANNING"));
        (void)snprintf(line, sizeof(line), "AP %u TRY %u", (unsigned)s_stats.aps_seen,
                       (unsigned)s_stats.targets_tried);
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        poom_arduboy_draw_rect(8, 42, 112, 4, WHITE);
        poom_arduboy_fill_rect(sweep_x, 43, 12, 2, WHITE);
        poomgotchi_draw_footer_("B:STOP", NULL);
    } else if(s_screen == POOMGOTCHI_SCREEN_CAPTURE) {
        poom_pcap_wifi_handshake_status_t hs = {0};
        (void)poom_pcap_manager_wifi_handshake_get_status(&hs);
        (void)snprintf(line, sizeof(line), "(o_o) %s C%u", poomgotchi_policy_label_(),
                       target != NULL ? (unsigned)target->primary : 0U);
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(line);
        poomgotchi_draw_target_name_(target, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)snprintf(line, sizeof(line), "E%X P%u U%u D%u", (unsigned)hs.eapol_message_mask,
                       (unsigned)hs.pmkid_count, (unsigned)hs.client_count,
                       (unsigned)poomgotchi_display_count_(hs.deauth_count));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + 2 * POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        poomgotchi_draw_footer_(s_policy == POOMGOTCHI_POLICY_OFF ? "A:OFF" : "A:TX", "B:STOP");
    } else if(s_screen == POOMGOTCHI_SCREEN_RESULT) {
        poom_pcap_wifi_handshake_status_t hs = {0};
        (void)poom_pcap_manager_wifi_handshake_get_status(&hs);
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(s_result_captured ? F("(^_^) CAPTURE") : F("(-_-) NO CAP"));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(s_capture_file[0] != '\0' ? s_capture_file : "UART PCAP");
        (void)snprintf(line, sizeof(line), "HS %u PMK %u D%u", (unsigned)s_stats.handshakes,
                       (unsigned)s_stats.pmkids, (unsigned)poomgotchi_display_count_(hs.deauth_count));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + 2 * POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        poomgotchi_draw_footer_("AUTO", "B:STOP");
    } else if(s_screen == POOMGOTCHI_SCREEN_ERROR) {
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(F("(x_x) ERROR"));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(poomgotchi_error_label_());
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + 2 * POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(F("PCAP OR SD BUSY"));
        poomgotchi_draw_footer_("A:RETRY", "B:STOP");
    } else if(s_screen == POOMGOTCHI_SCREEN_SUMMARY) {
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(F("(^_^) SESSION"));
        (void)snprintf(line, sizeof(line), "AP%u T%u U%u",
                       (unsigned)poomgotchi_display_count_(s_stats.aps_seen),
                       (unsigned)poomgotchi_display_count_(s_stats.targets_tried),
                       (unsigned)poomgotchi_display_count_(s_stats.clients_seen));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        (void)snprintf(line, sizeof(line), "H%u P%u D%u/%u",
                       (unsigned)poomgotchi_display_count_(s_stats.handshakes),
                       (unsigned)poomgotchi_display_count_(s_stats.pmkids),
                       (unsigned)poomgotchi_display_count_(s_stats.deauth_seen),
                       (unsigned)poomgotchi_display_count_(s_stats.deauths));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + 2 * POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        poomgotchi_draw_footer_("A:AGAIN", "B:EXIT");
    } else {
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, POOMGOTCHI_ROW0_Y);
        (void)poom_arduboy_print(F("(-_-) NO TARGET"));
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(F("SCAN COMPLETE"));
        (void)snprintf(line, sizeof(line), "AP %u", (unsigned)s_stats.aps_seen);
        poom_arduboy_set_cursor(POOMGOTCHI_TEXT_X, (int16_t)(POOMGOTCHI_ROW0_Y + 2 * POOMGOTCHI_ROW_STEP));
        (void)poom_arduboy_print(line);
        poomgotchi_draw_footer_("A:SCAN", "B:STOP");
    }

    poom_arduboy_display();
}

static void poomgotchi_free_session_(void)
{
    free(s_session);
    s_session = NULL;
    s_target_count = 0U;
    s_target_index = 0U;
}

static esp_err_t poomgotchi_alloc_session_(void)
{
    if(s_session != NULL) return ESP_OK;
    s_session = heap_caps_calloc(1U, sizeof(*s_session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_session == NULL) s_session = calloc(1U, sizeof(*s_session));
    return (s_session != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool poomgotchi_ap_is_usable_(const wifi_ap_record_t *ap)
{
    return (ap != NULL) && poom_wifi_ctrl_channel_is_valid(ap->primary) &&
           (memcmp(ap->bssid, "\0\0\0\0\0\0", 6U) != 0);
}

static int poomgotchi_history_find_(const uint8_t bssid[6])
{
    if((s_session == NULL) || (bssid == NULL)) return -1;
    for(uint8_t i = 0U; i < POOMGOTCHI_TARGET_MAX; ++i) {
        if(s_session->history[i].used &&
           (memcmp(s_session->history[i].bssid, bssid, 6U) == 0)) return i;
    }
    return -1;
}

static uint8_t poomgotchi_history_get_(const uint8_t bssid[6])
{
    int found = poomgotchi_history_find_(bssid);
    if(found >= 0) return (uint8_t)found;

    uint8_t slot = POOMGOTCHI_HISTORY_NONE;
    for(uint8_t i = 0U; i < POOMGOTCHI_TARGET_MAX; ++i) {
        if(!s_session->history[i].used) {
            slot = i;
            break;
        }
    }
    if(slot == POOMGOTCHI_HISTORY_NONE) {
        slot = s_session->history_replace_index++ % POOMGOTCHI_TARGET_MAX;
    }
    (void)memset(&s_session->history[slot], 0, sizeof(s_session->history[slot]));
    (void)memcpy(s_session->history[slot].bssid, bssid, 6U);
    s_session->history[slot].used = true;
    return slot;
}

static uint8_t poomgotchi_score_(const wifi_ap_record_t *ap, const poomgotchi_history_t *history)
{
    int score = 10;

    if(ap->rssi >= -55) score += 4;
    else if(ap->rssi >= -70) score += 2;
    else if(ap->rssi <= -80) score -= 4;
    if(ap->authmode >= WIFI_AUTH_WPA_PSK) score += 3;
    if(ap->ssid[0] == '\0') score -= 2;
    if(history != NULL) {
        if((history->flags & POOMGOTCHI_HISTORY_EAPOL) != 0U) score += 4;
        if(history->clients_seen != 0U) score += 4;
        if((history->flags & POOMGOTCHI_HISTORY_CAPTURED) != 0U) score -= 8;
        score -= (history->attempts > 3U) ? 6 : (int)(history->attempts * 2U);
    }
    if(score < 0) score = 0;
    if(score > 99) score = 99;
    return (uint8_t)score;
}

static void poomgotchi_copy_targets_(void)
{
    poom_wifi_scanner_ap_records_t *records = poom_wifi_scanner_get_ap_records();

    s_target_count = 0U;
    s_target_index = 0U;
    if((records == NULL) || (s_session == NULL)) return;

    s_stats.aps_seen = (uint16_t)(s_stats.aps_seen + records->count);
    for(uint16_t i = 0U; i < records->count; ++i) {
        const wifi_ap_record_t *ap = &records->records[i];
        const int history_index = poomgotchi_history_find_(ap->bssid);
        poomgotchi_target_t candidate;
        uint8_t insert_at;

        if(!poomgotchi_ap_is_usable_(ap)) continue;
        candidate.scan_index = (uint8_t)i;
        candidate.score = poomgotchi_score_(ap,
            (history_index >= 0) ? &s_session->history[history_index] : NULL);
        candidate.history_index = POOMGOTCHI_HISTORY_NONE;
        insert_at = s_target_count;
        while((insert_at > 0U) && (s_session->targets[insert_at - 1U].score < candidate.score)) {
            if(insert_at < POOMGOTCHI_TARGET_MAX) s_session->targets[insert_at] = s_session->targets[insert_at - 1U];
            --insert_at;
        }
        if(insert_at < POOMGOTCHI_TARGET_MAX) {
            s_session->targets[insert_at] = candidate;
            if(s_target_count < POOMGOTCHI_TARGET_MAX) ++s_target_count;
        }
    }
    for(uint8_t i = 0U; i < s_target_count; ++i) {
        wifi_ap_record_t *ap = &records->records[s_session->targets[i].scan_index];
        uint8_t history_index = poomgotchi_history_get_(ap->bssid);
        s_session->targets[i].history_index = history_index;
        s_session->targets[i].score = poomgotchi_score_(ap, &s_session->history[history_index]);
    }
}

static void poomgotchi_stop_capture_(void)
{
    if(s_capture_running) {
        (void)poom_pcap_manager_sniffer_stop();
        s_capture_running = false;
    }
}

static bool poomgotchi_deauth_allowed_(void)
{
    return (s_last_deauth_ms == 0U) || poomgotchi_elapsed_(s_last_deauth_ms, POOMGOTCHI_DEAUTH_MIN_MS);
}

static bool poomgotchi_send_deauth_(void)
{
    wifi_ap_record_t *target = poomgotchi_target_();

    if((s_policy == POOMGOTCHI_POLICY_OFF) || !s_capture_running ||
       (target == NULL) || !poomgotchi_deauth_allowed_()) return false;
    poom_wifi_attacks_broadcast_once(target);
    s_last_deauth_ms = poomgotchi_now_ms_();
    ++s_stats.deauths;
    poomgotchi_request_render_();
    return true;
}

static void poomgotchi_remember_capture_file_(void)
{
    const char *path = poom_pcap_manager_get_file_path();
    const char *name;

    s_capture_file[0] = '\0';
    if(path == NULL) return;
    name = strrchr(path, '/');
    name = (name != NULL) ? (name + 1) : path;
    (void)snprintf(s_capture_file, sizeof(s_capture_file), "%.20s", name);
}

static void poomgotchi_start_capture_(void)
{
    const wifi_ap_record_t *target = poomgotchi_target_();
    const uint32_t filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;

    if(target == NULL) {
        s_screen = POOMGOTCHI_SCREEN_IDLE;
        poomgotchi_request_render_();
        return;
    }
    if(s_capture_running) {
        s_last_error = poom_pcap_manager_sniffer_retarget_wifi_handshake(
            target->primary, filter_mask, target->bssid);
    } else {
        s_capture_file[0] = '\0';
        s_last_error = poom_pcap_manager_sniffer_start_wifi_capture_target(
            target->primary, filter_mask, target->bssid);
    }
    if(s_last_error != ESP_OK) {
        s_capture_running = false;
        s_screen = POOMGOTCHI_SCREEN_ERROR;
        poomgotchi_request_render_();
        return;
    }

    poomgotchi_remember_capture_file_();
    s_assist_sent = false;
    s_capture_running = true;
    s_capture_started_ms = poomgotchi_now_ms_();
    ++s_stats.targets_tried;
    s_screen = POOMGOTCHI_SCREEN_CAPTURE;
    poomgotchi_request_render_();
}

static void poomgotchi_record_capture_observations_(const poom_pcap_wifi_handshake_status_t *hs,
                                                     bool captured)
{
    poomgotchi_history_t *history = poomgotchi_target_history_();

    if(hs == NULL) return;
    if(history != NULL) {
        if(history->attempts < UINT8_MAX) ++history->attempts;
        if(hs->eapol_message_mask != 0U) {
            history->flags |= POOMGOTCHI_HISTORY_EAPOL;
            ++s_stats.eapol_targets;
        }
        if(hs->client_count > history->clients_seen) history->clients_seen = hs->client_count;
        if(captured) history->flags |= POOMGOTCHI_HISTORY_CAPTURED;
    }
    s_stats.clients_seen = (uint16_t)(s_stats.clients_seen + hs->client_count);
    s_stats.deauth_seen = (uint16_t)(s_stats.deauth_seen + hs->deauth_count);
    if(captured) {
        s_stats.handshakes = (uint16_t)(s_stats.handshakes + hs->valid_pair_count);
        s_stats.pmkids = (uint16_t)(s_stats.pmkids + hs->pmkid_count);
    }
}

static void poomgotchi_finish_capture_(bool captured)
{
    poom_pcap_wifi_handshake_status_t hs = {0};

    (void)poom_pcap_manager_wifi_handshake_get_status(&hs);
    poomgotchi_record_capture_observations_(&hs, captured);
    poomgotchi_remember_capture_file_();
    s_result_captured = captured;
    s_result_until_ms = poomgotchi_now_ms_() + POOMGOTCHI_RESULT_MS;
    s_screen = POOMGOTCHI_SCREEN_RESULT;
    poomgotchi_request_render_();
}

static void poomgotchi_show_summary_(void)
{
    if(s_capture_running && (s_screen == POOMGOTCHI_SCREEN_CAPTURE)) {
        poom_pcap_wifi_handshake_status_t hs = {0};
        (void)poom_pcap_manager_wifi_handshake_get_status(&hs);
        poomgotchi_record_capture_observations_(&hs, false);
    }
    poomgotchi_stop_capture_();
    s_stop_requested = false;
    s_scan_requested = false;
    s_deauth_requested = false;
    s_screen = POOMGOTCHI_SCREEN_SUMMARY;
    poomgotchi_request_render_();
}

static void poomgotchi_run_scan_(void)
{
    if(s_capture_running) {
        s_last_error = poom_pcap_manager_sniffer_pause_wifi_handshake();
        if(s_last_error != ESP_OK) {
            poomgotchi_stop_capture_();
            s_screen = POOMGOTCHI_SCREEN_ERROR;
            poomgotchi_request_render_();
            return;
        }
    }
    s_scan_in_progress = true;
    s_screen = POOMGOTCHI_SCREEN_SCAN;
    poomgotchi_request_render_();
    poomgotchi_draw_();

    s_last_error = poom_wifi_scanner_scan();
    s_scan_in_progress = false;
    if(s_stop_requested) {
        poomgotchi_show_summary_();
        return;
    }
    if(s_last_error != ESP_OK) {
        s_screen = POOMGOTCHI_SCREEN_ERROR;
        poomgotchi_request_render_();
        return;
    }

    poomgotchi_copy_targets_();
    if(s_target_count == 0U) {
        s_screen = POOMGOTCHI_SCREEN_IDLE;
        poomgotchi_request_render_();
        return;
    }
    poomgotchi_start_capture_();
}

static void poomgotchi_next_target_(void)
{
    ++s_target_index;
    if(s_target_index >= s_target_count) {
        s_scan_requested = true;
        return;
    }
    poomgotchi_start_capture_();
}

static void poomgotchi_reset_session_(void)
{
    (void)memset(&s_stats, 0, sizeof(s_stats));
    if(s_session != NULL) (void)memset(s_session, 0, sizeof(*s_session));
    s_target_count = 0U;
    s_target_index = 0U;
    s_last_deauth_ms = 0U;
    s_result_captured = false;
    s_capture_file[0] = '\0';
}

static void poomgotchi_close_(void)
{
    const uint8_t token = 1U;

    poomgotchi_stop_capture_();
    poomgotchi_free_session_();
    s_active = false;
    s_scan_in_progress = false;
    s_exit_requested = false;
    s_stop_requested = false;
    s_scan_requested = false;
    s_restart_requested = false;
    s_deauth_requested = false;
    s_policy_delta = 0;
    if(s_buttons_subscribed) {
        (void)poom_sbus_unsubscribe_cb("input/button", poomgotchi_button_cb_, (void *)s_user);
        s_buttons_subscribed = false;
    }
    s_task = NULL;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static void poomgotchi_button_cb_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t event;
    (void)user;

    if((msg == NULL) || (msg->len < sizeof(event))) return;
    (void)memcpy(&event, msg->data, sizeof(event));
    if((event.event != BUTTON_SINGLE_CLICK) || !s_active) return;

    if(event.button == BUTTON_B) {
        if(s_screen == POOMGOTCHI_SCREEN_SUMMARY) s_exit_requested = true;
        else {
            s_stop_requested = true;
            if(s_scan_in_progress) (void)esp_wifi_scan_stop();
        }
    } else if(event.button == BUTTON_A) {
        if(s_screen == POOMGOTCHI_SCREEN_CAPTURE) s_deauth_requested = true;
        else if(s_screen == POOMGOTCHI_SCREEN_SUMMARY) {
            s_restart_requested = true;
        } else if((s_screen == POOMGOTCHI_SCREEN_IDLE) || (s_screen == POOMGOTCHI_SCREEN_ERROR)) {
            s_scan_requested = true;
        }
    } else if(s_screen == POOMGOTCHI_SCREEN_CAPTURE) {
        if(event.button == BUTTON_UP) s_policy_delta = 1;
        else if(event.button == BUTTON_DOWN) s_policy_delta = -1;
    }
    poomgotchi_request_render_();
}

static void poomgotchi_apply_policy_input_(void)
{
    if(s_policy_delta == 0) return;
    int policy = (int)s_policy + (int)s_policy_delta;
    s_policy_delta = 0;
    if(policy < 0) policy = POOMGOTCHI_POLICY_COUNT - 1;
    else if(policy >= POOMGOTCHI_POLICY_COUNT) policy = 0;
    s_policy = (poomgotchi_policy_t)policy;
    poomgotchi_request_render_();
}

static void poomgotchi_task_(void *arg)
{
    (void)arg;
    poomgotchi_request_render_();
    s_scan_requested = (s_session != NULL);

    while(s_active) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POOMGOTCHI_REFRESH_MS));
        if(s_exit_requested) {
            poomgotchi_close_();
            break;
        }
        if(s_stop_requested && !s_scan_in_progress) {
            poomgotchi_show_summary_();
        }
        if(s_restart_requested) {
            s_restart_requested = false;
            poomgotchi_reset_session_();
            s_screen = POOMGOTCHI_SCREEN_SCAN;
            s_scan_requested = (s_session != NULL);
        }
        if(s_scan_requested && !s_scan_in_progress) {
            s_scan_requested = false;
            poomgotchi_run_scan_();
            continue;
        }
        poomgotchi_apply_policy_input_();
        if(s_deauth_requested) {
            s_deauth_requested = false;
            (void)poomgotchi_send_deauth_();
        }
        if(s_screen == POOMGOTCHI_SCREEN_CAPTURE) {
            poom_pcap_wifi_handshake_status_t hs = {0};
            (void)poom_pcap_manager_wifi_handshake_get_status(&hs);
            if((hs.valid_pair_count > 0U) || (hs.pmkid_count > 0U)) {
                poomgotchi_finish_capture_(true);
            } else if((s_policy == POOMGOTCHI_POLICY_ASSIST) && !s_assist_sent &&
                      (hs.client_count > 0U) && (hs.eapol_message_mask == 0U) &&
                      poomgotchi_elapsed_(s_capture_started_ms, POOMGOTCHI_ASSIST_WAIT_MS)) {
                s_assist_sent = poomgotchi_send_deauth_();
            } else if(poomgotchi_elapsed_(s_capture_started_ms, POOMGOTCHI_CAPTURE_MS)) {
                poomgotchi_finish_capture_(false);
            } else if(!poom_pcap_manager_sniffer_is_active()) {
                s_last_error = ESP_ERR_INVALID_STATE;
                s_capture_running = false;
                s_screen = POOMGOTCHI_SCREEN_ERROR;
                poomgotchi_request_render_();
            }
        } else if((s_screen == POOMGOTCHI_SCREEN_RESULT) &&
                  poomgotchi_deadline_reached_(s_result_until_ms)) {
            poomgotchi_next_target_();
        }
        if(s_dirty || (s_screen == POOMGOTCHI_SCREEN_SCAN) ||
           (s_screen == POOMGOTCHI_SCREEN_CAPTURE)) {
            poomgotchi_draw_();
            s_dirty = false;
        }
    }
    vTaskDelete(NULL);
}

void menu_poomgotchi_show(void)
{
    const uint8_t token = 1U;

    if(s_active || (s_task != NULL)) return;
    s_active = true;
    s_buttons_subscribed = false;
    s_scan_in_progress = false;
    s_capture_running = false;
    s_assist_sent = false;
    s_exit_requested = false;
    s_stop_requested = false;
    s_scan_requested = false;
    s_restart_requested = false;
    s_deauth_requested = false;
    s_policy_delta = 0;
    s_dirty = true;
    s_screen = POOMGOTCHI_SCREEN_SCAN;
    s_policy = POOMGOTCHI_POLICY_MANUAL;
    s_last_error = ESP_OK;
    poomgotchi_reset_session_();

    if(poomgotchi_alloc_session_() != ESP_OK) {
        s_last_error = ESP_ERR_NO_MEM;
        s_screen = POOMGOTCHI_SCREEN_ERROR;
    }
    if(!poom_sbus_subscribe_cb("input/button", poomgotchi_button_cb_, (void *)s_user)) {
        poomgotchi_free_session_();
        s_active = false;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    s_buttons_subscribed = true;
    if(xTaskCreate(poomgotchi_task_, "menu_poomgotchi", POOMGOTCHI_STACK, NULL,
                   POOMGOTCHI_TASK_PRIO, &s_task) != pdPASS) {
        poomgotchi_close_();
    }
}
