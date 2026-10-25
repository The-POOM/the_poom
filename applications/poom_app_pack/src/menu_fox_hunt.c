// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_fox_hunt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "bsp_pong.h"
#include "button_driver.h"
#include "buzzer.h"
#include "esp_gap_ble_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "menu_detector_view.h"
#include "menu_wifi_detector.h"
#include "menu_ble_detector.h"
#include "menu_wifi_detector_oui.h"
#include "poom_ble_scan.h"
#include "poom_ble_detector.h"
#include "poom_led_rainbow.h"
#include "poom_sbus.h"
#include "poom_wifi_attacks.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_detector.h"
#include "poom_wifi_scanner.h"
#include "ws2812.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif
#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN (0U)
#endif
#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP (1U)
#endif

static const char *FOX_TAG = "menu_fox_hunt";

/* ----- Tuning knobs. ----------------------------------------------------- */
#define FOX_TICK_MS        (60)
#define FOX_STALE_US       (2000000LL)
/* Coast windows: when samples briefly stop, keep showing the last reading for
 * this long before falling back to SEARCHING. Clients transmit sporadically so
 * they get a wider "fresh" window and a much longer coast than AP/BLE hunts. */
#define FOX_HOLD_US         (4000000LL)
#define FOX_CLIENT_STALE_US (3000000LL)
#define FOX_CLIENT_HOLD_US  (8000000LL)
#define FOX_RING           (6)
#define FOX_EMA_ALPHA      (0.30f)
#define FOX_TREND_MS       (1000)
#define FOX_TREND_DB       (2.0f)
#define FOX_LEVEL_HYST_DB  (3.0f)
#define FOX_HEAT_MIN_DBM   (-90.0f)   /* generic floor for RSSI init seeds. */
/* Heat-bar range per radio: WiFi APs/clients transmit far stronger than BLE,
 * so the same RSSI means a much greater distance -> less sensitive bands. */
#define FOX_HEAT_MIN_BLE   (-90.0f)
#define FOX_HEAT_MAX_BLE   (-45.0f)
#define FOX_HEAT_MIN_WIFI  (-77.0f)
#define FOX_HEAT_MAX_WIFI  (-43.0f)
#define FOX_LED_BRIGHT     (50U)
#define FOX_BEEP_MS        (30U)
#define FOX_WIFI_SCAN_MS   (120U)   /* per-channel active dwell while hunting. */
#define FOX_LIST_REFRESH_MS (250)
#define FOX_LIST_STALE_US  (8000000LL)
/* Unicast deauth cadence/burst while hunting a Wi-Fi client (keeps a quiet
 * client transmitting so its RSSI stays fresh). */
#define FOX_DEAUTH_GAP_MS  (250LL)
#define FOX_DEAUTH_BURST   (2)
/* A+DOWN manual nudge: one bigger burst to wake a very quiet client. */
#define FOX_DEAUTH_NUDGE   (5)
/* Client discovery (AP -> clients) reuses the Wi-Fi detector's passive monitor;
 * its snapshot is capped at the detector view capacity. */
#define FOX_CLI_CAP        (MENU_DETECTOR_VIEW_MAX_ITEMS)

#define FOX_LEVEL_SEARCHING (-1)
#define FOX_LEVEL_COUNT     (5)

/* RSSI (dBm) to step up a level, per radio. WiFi bands sit ~18 dB stronger so
 * "BOILING" only triggers right on top of a WiFi AP/client, not across a room. */
static const float s_level_up_ble[FOX_LEVEL_COUNT - 1]  = {-80.0f, -70.0f, -60.0f, -50.0f};
/* Non-linear ("orbit") bands: broad cold zone far out, tightening/heating fast
 * as you close in on a WiFi AP/client. */
static const float s_level_up_wifi[FOX_LEVEL_COUNT - 1] = {-72.0f, -58.0f, -50.0f, -45.0f};
static const char *const s_level_name[FOX_LEVEL_COUNT] = {
    "FREEZING", "COLD", "WARM", "HOT", "BOILING"};
static const uint8_t s_level_rgb[FOX_LEVEL_COUNT][3] = {
    {0, 0, 255}, {0, 255, 0}, {255, 210, 0}, {255, 90, 0}, {255, 0, 0}};
static const uint32_t s_level_beep_ms[FOX_LEVEL_COUNT] = {1200U, 700U, 400U, 220U, 90U};
static const uint32_t s_level_beep_hz[FOX_LEVEL_COUNT] = {1400U, 1700U, 2100U, 2600U, 3200U};

/* ----- Screens. ---------------------------------------------------------- */
typedef enum
{
    SCR_MAIN = 0,
    SCR_BLE_LIST,
    SCR_WIFI_LIST,
    SCR_WIFI_CLIENT_AP,   /* pick the AP whose clients we want to hunt */
    SCR_WIFI_CLIENT_LIST, /* pick a client of the chosen AP */
    SCR_OPTIONS,
    SCR_HUNT,
} fox_screen_t;

static const char *const s_main_items[] = {
    "WIFI AP", "WIFI CLIENTS", "BLE BEACON", "OPTIONS"};
#define FOX_MAIN_COUNT (4)
#define FOX_OPT_COUNT  (2)

/* Audio volume and LED brightness are 0..100% in 20% steps. Adjusted on the
 * OPTIONS screen (LEFT/RIGHT) and live during a hunt (UP/DOWN = volume,
 * LEFT/RIGHT = brightness). 100% brightness maps to WS2812 level 200. */
#define FOX_PCT_STEP     (20)
#define FOX_BRIGHT_255(pct) ((uint8_t)((pct) * 2))

#define FOX_VISIBLE_ROWS (4)
#define FOX_LIST_MAX     (16)
#define FOX_LIST_LABEL_CHARS (13U)
#define FOX_LIST_SCROLL_MS   (350U)
#define FOX_LIST_SCROLL_GAP  (3U)

typedef struct
{
    bool used;
    bool is_wifi;
    uint8_t addr[6];
    uint8_t channel;
    int rssi;
    bool rssi_known;
    int64_t last_seen_us;
    char name[20];
} fox_item_t;

/* ----- Radio ownership. -------------------------------------------------- */
/* FOX_RADIO_WIFI       = active-scan driven (AP hunt / AP list).
 * FOX_RADIO_WIFI_SNIFF = promiscuous capture of one client's RSSI (client hunt). */
typedef enum {
    FOX_RADIO_NONE = 0,
    FOX_RADIO_BLE,
    FOX_RADIO_WIFI,
    FOX_RADIO_WIFI_SNIFF,
} fox_radio_t;

/* ----- Shared state. ----------------------------------------------------- */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_active;
static volatile bool s_exit_requested;
static TaskHandle_t s_task;
static bool s_buttons_subscribed;

static volatile fox_screen_t s_screen;
static volatile int s_nav_delta;       /* +1/-1 queued from buttons (UP/DOWN). */
static volatile int s_level_delta;     /* +1/-1 queued from buttons (LEFT/RIGHT). */
static volatile bool s_toggle_label;   /* LEFT on a list screen: name<->MAC. */
static volatile bool s_select_pressed;
static bool s_show_mac;                /* list display mode: false=name, true=MAC. */
static volatile bool s_back_pressed;
static volatile bool s_reset_peak_requested;
static volatile bool s_wifi_scanning;  /* true while a blocking AP scan runs. */
static volatile bool s_scan_cancelled; /* B pressed during that scan. */
/* Do not turn a double B used to cancel a scan into an app exit. */
static int64_t s_cancel_back_ignore_until_us;

static int s_sel;
static int s_scroll;

/* audio volume + LED brightness, 0..100 in 20% steps. */
static int s_vol_pct = 40;
static int s_bright_pct = 40;
/* Only non-sensitive presentation preferences are stored in this namespace. */
static void fox_preferences_load_(void)
{
    nvs_handle_t handle;
    if (nvs_open("fox_prefs", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value;
    if (nvs_get_u8(handle, "volume", &value) == ESP_OK && value <= 100) s_vol_pct = value;
    if (nvs_get_u8(handle, "led", &value) == ESP_OK && value <= 100) s_bright_pct = value;
    nvs_close(handle);
}
static void fox_preferences_save_(void)
{
    nvs_handle_t handle;
    if (nvs_open("fox_prefs", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t a = nvs_set_u8(handle, "volume", (uint8_t)s_vol_pct);
    esp_err_t b = nvs_set_u8(handle, "led", (uint8_t)s_bright_pct);
    if (a == ESP_OK && b == ESP_OK) (void)nvs_commit(handle);
    nvs_close(handle);
}

/* transient HUD line shown briefly after a live vol/brightness change. */
static char s_hud[16];
static int64_t s_hud_until_us;

/* Lists are only needed while Fox Hunt is open. Keep them off permanent
 * internal RAM and put them in PSRAM when it is available. */
typedef struct
{
    fox_item_t list[FOX_LIST_MAX];
    poom_wifi_detector_record_t cli_storage[FOX_CLI_CAP];
    poom_wifi_detector_record_t *cli_ptrs[FOX_CLI_CAP];
    fox_item_t cli_cache[FOX_LIST_MAX];
    uint32_t list_selection_tick_ms;
} fox_session_t;

static fox_session_t *s_session = NULL;
#define s_list        (s_session->list)
#define s_cli_storage (s_session->cli_storage)
#define s_cli_ptrs    (s_session->cli_ptrs)
#define s_cli_cache   (s_session->cli_cache)
#define s_list_selection_tick_ms (s_session->list_selection_tick_ms)

static int s_list_count;

/* hunt target */
static bool s_target_wifi;             /* target uses the Wi-Fi radio (AP or client) */
static bool s_target_client;          /* target is a Wi-Fi client (promiscuous sniff) */
static uint8_t s_target_addr[6];       /* BLE addr / AP BSSID / client MAC */
static uint8_t s_target_channel;
static uint8_t s_target_ap_bssid[6];   /* AP the hunted client is associated to */
static char s_target_name[20];

/* client hunt: deauth overlay + where B returns to. */
static volatile bool s_deauth_on;
static volatile bool s_toggle_deauth;  /* A+UP: toggle constant deauth */
static volatile bool s_burst_deauth;   /* A+DOWN: one manual deauth nudge */
static volatile bool s_a_held;         /* A held = FN modifier (chord) */
static int64_t s_last_deauth_us;
static fox_screen_t s_hunt_return = SCR_MAIN;
static bool s_start_in_hunt;           /* launched straight into the hunt screen */
static bool s_return_to_detector;      /* launched from WIFI DETECT: B goes back there */
static bool s_return_to_ble_detector;  /* launched from BLE DETECT */

/* Client discovery (AP -> clients) uses the session buffers above. */
static bool s_monitor_on;
/* Cached client list so returning from a hunt is instant (no empty re-listen). */
static int s_cli_cache_count;
static uint8_t s_cli_cache_ap[6];

/* hunt RSSI pipeline */
static int s_raw_ring[FOX_RING];
static int s_raw_head;
static int s_raw_count;
static int64_t s_last_rx_us;

/* LEDs */
static ws2812_strip_t s_strip;
static bool s_led_ok;
static bool s_strip_inited;
static bool s_rainbow_was_running;
static int s_led_level;

static fox_radio_t s_radio;

static void fox_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

static bool fox_alloc_session_(void)
{
    if(s_session != NULL) return true;
    s_session = heap_caps_calloc(1U, sizeof(*s_session),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_session == NULL) s_session = calloc(1U, sizeof(*s_session));
    return (s_session != NULL);
}

static void fox_free_session_(void)
{
    if(s_session != NULL)
    {
        (void)memset(s_session, 0, sizeof(*s_session));
        free(s_session);
        s_session = NULL;
    }
}

/* ======================================================================== */
/* RSSI pipeline + hunt math                                                */
/* ======================================================================== */

static void fox_report_rssi_(int rssi)
{
    portENTER_CRITICAL(&s_lock);
    s_raw_ring[s_raw_head] = rssi;
    s_raw_head = (s_raw_head + 1) % FOX_RING;
    if (s_raw_count < FOX_RING)
    {
        s_raw_count++;
    }
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

static void fox_rssi_reset_(void)
{
    portENTER_CRITICAL(&s_lock);
    s_raw_head = 0;
    s_raw_count = 0;
    s_last_rx_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

static int fox_median_(const int *values, int count)
{
    int sorted[FOX_RING];
    int i;
    int j;

    for (i = 0; i < count; ++i)
    {
        sorted[i] = values[i];
    }
    for (i = 1; i < count; ++i)
    {
        int key = sorted[i];
        for (j = i - 1; (j >= 0) && (sorted[j] > key); --j)
        {
            sorted[j + 1] = sorted[j];
        }
        sorted[j + 1] = key;
    }
    return sorted[count / 2];
}

static int fox_level_from_rssi_(float rssi, int current)
{
    const float *lvl = s_target_wifi ? s_level_up_wifi : s_level_up_ble;
    int level = (current < 0) ? 0 : current;

    while ((level < (FOX_LEVEL_COUNT - 1)) && (rssi >= lvl[level]))
    {
        level++;
    }
    while ((level > 0) && (rssi < (lvl[level - 1] - FOX_LEVEL_HYST_DB)))
    {
        level--;
    }
    return level;
}

static float fox_clampf_(float v, float lo, float hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

static void fox_set_led_(int level)
{
    if (!s_led_ok)
    {
        return;
    }
    if (s_bright_pct <= 0)
    {
        level = FOX_LEVEL_SEARCHING; /* brightness 0 -> keep LEDs dark. */
    }
    if (level == s_led_level)
    {
        return;
    }
    s_led_level = level;
    if (level == FOX_LEVEL_SEARCHING)
    {
        ws2812_clear(&s_strip);
    }
    else
    {
        ws2812_set_brightness(&s_strip, FOX_BRIGHT_255(s_bright_pct));
        ws2812_fill(&s_strip,
                    s_level_rgb[level][0],
                    s_level_rgb[level][1],
                    s_level_rgb[level][2],
                    0);
    }
    (void)ws2812_show(&s_strip);
}

/* Live preview of the LED brightness while adjusting it in OPTIONS.
 * Lights the strip white at the selected level, or clears it at OFF.
 * Bypasses the hunt level cache, so the next hunt render still applies. */
static void fox_led_preview_(int leds_pct)
{
    if (!s_led_ok)
    {
        return;
    }
    s_led_level = -2; /* force the next hunt render to repaint. */
    if (leds_pct <= 0)
    {
        ws2812_clear(&s_strip);
    }
    else
    {
        ws2812_set_brightness(&s_strip, FOX_BRIGHT_255(leds_pct));
        ws2812_fill(&s_strip, 255, 255, 255, 0);
    }
    (void)ws2812_show(&s_strip);
}

/* ======================================================================== */
/* BLE scan callback (feeds both the BLE list and the BLE hunt)             */
/* ======================================================================== */

static void fox_ble_name_(const esp_ble_gap_cb_param_t *p, char *out, size_t out_len)
{
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data(
        (uint8_t *)p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if ((name == NULL) || (name_len == 0))
    {
        name = esp_ble_resolve_adv_data(
            (uint8_t *)p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if ((name != NULL) && (name_len > 0))
    {
        size_t n = (name_len < (out_len - 1)) ? name_len : (out_len - 1);
        (void)memcpy(out, name, n);
        out[n] = '\0';
    }
    else
    {
        out[0] = '\0';
    }
}

static void fox_list_add_ble_(const uint8_t addr[6], int rssi, const char *name)
{
    int i;

    portENTER_CRITICAL(&s_lock);
    for (i = 0; i < s_list_count; ++i)
    {
        if (memcmp(s_list[i].addr, addr, 6) == 0)
        {
            s_list[i].rssi = rssi;
            s_list[i].rssi_known = true;
            s_list[i].last_seen_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_lock);
            return;
        }
    }
    if (s_list_count < FOX_LIST_MAX)
    {
        fox_item_t *it = &s_list[s_list_count];
        (void)memset(it, 0, sizeof(*it));
        it->used = true;
        it->is_wifi = false;
        (void)memcpy(it->addr, addr, 6);
        it->rssi = rssi;
        it->rssi_known = true;
        it->last_seen_us = esp_timer_get_time();
        if ((name != NULL) && (name[0] != '\0'))
        {
            (void)snprintf(it->name, sizeof(it->name), "%.19s", name);
        }
        else
        {
            (void)snprintf(it->name, sizeof(it->name), "%02X%02X%02X%02X%02X%02X",
                           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        }
        s_list_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void fox_ble_cb_(const esp_ble_gap_cb_param_t *scan_result)
{
    if ((scan_result == NULL) ||
        (scan_result->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT))
    {
        return;
    }

    if (s_screen == SCR_HUNT && !s_target_wifi)
    {
        if (memcmp(scan_result->scan_rst.bda, s_target_addr, 6) == 0)
        {
            fox_report_rssi_(scan_result->scan_rst.rssi);
        }
        return;
    }

    if(s_screen == SCR_BLE_LIST)
    {
        char name[20];
        poom_ble_detector_record_t classified;
        size_t adv_len = (size_t)scan_result->scan_rst.adv_data_len +
                         (size_t)scan_result->scan_rst.scan_rsp_len;

        if(adv_len > sizeof(scan_result->scan_rst.ble_adv))
        {
            adv_len = sizeof(scan_result->scan_rst.ble_adv);
        }
        if(poom_ble_detector_parse(scan_result->scan_rst.ble_adv,
                                   adv_len,
                                   scan_result->scan_rst.bda,
                                   scan_result->scan_rst.rssi,
                                   &classified) &&
           (classified.device_class != POOM_BLE_DETECTOR_CLASS_UNKNOWN))
        {
            /* BLE DETECT owns the signatures; Fox Hunt only reuses its label. */
            (void)snprintf(name, sizeof(name), "%.19s", classified.name);
        }
        else
        {
            fox_ble_name_(scan_result, name, sizeof(name));
        }
        fox_list_add_ble_(scan_result->scan_rst.bda, scan_result->scan_rst.rssi, name);
    }
}

/* ======================================================================== */
/* Wi-Fi client sniff (promiscuous RSSI of one station)                     */
/* ======================================================================== */

/* Promiscuous RX: feed the hunt pipeline every frame to/from the target
 * client (802.11 addr1 = receiver, addr2 = transmitter). */
static void fox_client_promisc_cb_(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt;
    const uint8_t *payload;
    (void)type;

    if ((buffer == NULL) || (s_screen != SCR_HUNT) || !s_target_client)
    {
        return;
    }
    pkt = (const wifi_promiscuous_pkt_t *)buffer;
    if (pkt->rx_ctrl.sig_len < 16U) /* need addr1 (+4) and addr2 (+10). */
    {
        return;
    }
    payload = pkt->payload;
    if (payload == NULL)
    {
        return;
    }
    if ((memcmp(payload + 4, s_target_addr, 6) == 0) ||
        (memcmp(payload + 10, s_target_addr, 6) == 0))
    {
        fox_report_rssi_(pkt->rx_ctrl.rssi);
    }
}

/* Send a short burst of unicast deauth frames at the hunted client so it keeps
 * transmitting (reconnect traffic), giving the RDF a steady RSSI stream. */
static void fox_client_deauth_send_(int frames)
{
    wifi_ap_record_t ap;
    int i;

    (void)memset(&ap, 0, sizeof(ap));
    (void)memcpy(ap.bssid, s_target_ap_bssid, sizeof(ap.bssid));
    ap.primary = s_target_channel;
    for (i = 0; i < frames; ++i)
    {
        poom_wifi_attacks_deauth_client_once(&ap, s_target_addr);
    }
}

/* Steady background nudge while deauth is ON (keeps a quiet client TXing). */
static void fox_client_deauth_tick_(void)
{
    fox_client_deauth_send_(FOX_DEAUTH_BURST);
}

/* Start/stop the detector's passive AP-client monitor used for discovery. */
static bool fox_client_monitor_start_(void)
{
    int i;

    for (i = 0; i < FOX_CLI_CAP; ++i)
    {
        s_cli_ptrs[i] = &s_cli_storage[i];
    }
    if (poom_wifi_detector_monitor_start(POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS,
                                         NULL, 0U, FOX_CLI_CAP) != ESP_OK)
    {
        return false;
    }
    if (poom_wifi_detector_monitor_select_ap(s_target_channel,
                                             s_target_ap_bssid) != ESP_OK)
    {
        (void)poom_wifi_detector_monitor_stop();
        return false;
    }
    s_monitor_on = true;
    return true;
}

static void fox_client_monitor_stop_(void)
{
    if (s_monitor_on)
    {
        (void)poom_wifi_detector_monitor_stop();
        s_monitor_on = false;
    }
}

/* Label a client the way WIFI DETECT does: randomized MAC -> "Private XXYY",
 * else OUI vendor + short suffix, else the full MAC. */
static void fox_client_name_(const uint8_t mac[6], char *out, size_t out_len)
{
    const char *vendor;

    if (menu_wifi_detector_oui_is_random(mac))
    {
        (void)snprintf(out, out_len, "Private %02X%02X", mac[4], mac[5]);
        return;
    }
    vendor = menu_wifi_detector_oui_vendor(mac);
    if (vendor != NULL)
    {
        (void)snprintf(out, out_len, "%.12s %02X%02X", vendor, mac[4], mac[5]);
    }
    else
    {
        (void)snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

/* Merge the current client snapshot into the shared display list: update the
 * RSSI of known clients in place and append new ones. Merging (rather than
 * rebuilding) keeps a restored/cached list populated instead of flashing empty
 * while the monitor re-discovers clients after a hunt. */
static void fox_client_list_refresh_(void)
{
    size_t count = 0U;
    size_t i;

    if (!s_monitor_on)
    {
        return;
    }
    if (poom_wifi_detector_monitor_snapshot(s_cli_ptrs, FOX_CLI_CAP, &count) != ESP_OK)
    {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    for (i = 0; i < count; ++i)
    {
        const poom_wifi_detector_record_t *r = s_cli_ptrs[i];
        int j;
        fox_item_t *it = NULL;

        if (r->is_access_point)
        {
            continue; /* clients only */
        }
        for (j = 0; j < s_list_count; ++j)
        {
            if (memcmp(s_list[j].addr, r->bssid, 6) == 0)
            {
                it = &s_list[j];
                break;
            }
        }
        if (it == NULL)
        {
            if (s_list_count >= FOX_LIST_MAX)
            {
                continue;
            }
            it = &s_list[s_list_count];
            (void)memset(it, 0, sizeof(*it));
            it->used = true;
            it->is_wifi = true;
            (void)memcpy(it->addr, r->bssid, 6);
            it->channel = s_target_channel;
            fox_client_name_(r->bssid, it->name, sizeof(it->name));
            s_list_count++;
        }
        it->rssi = r->rssi;
        it->rssi_known = r->rssi_known;
        it->last_seen_us = esp_timer_get_time();
    }
    if (s_sel >= s_list_count)
    {
        s_sel = (s_list_count > 0) ? (s_list_count - 1) : 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ======================================================================== */
/* Radio management                                                         */
/* ======================================================================== */

static void fox_radio_want_(fox_radio_t want)
{
    if (s_radio == want)
    {
        return;
    }

    if (s_radio == FOX_RADIO_BLE)
    {
        (void)poom_ble_scan_stop();
        poom_ble_scan_register_cb(NULL);
    }
    else if (s_radio == FOX_RADIO_WIFI)
    {
        (void)poom_wifi_ctrl_deinit();
    }
    else if (s_radio == FOX_RADIO_WIFI_SNIFF)
    {
        /* Client-hunt deauth is sent statelessly (deauth_client_once) from the
         * hunt loop, so there is no attack task to stop - just drop the sniffer. */
        s_deauth_on = false;
        (void)poom_wifi_ctrl_set_promiscuous(false);
        (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
        (void)poom_wifi_ctrl_deinit();
    }

    if (want == FOX_RADIO_BLE)
    {
        poom_ble_scan_register_cb(fox_ble_cb_);
        poom_ble_scan_set_scan_type(BLE_SCAN_TYPE_ACTIVE);
        (void)poom_ble_scan_start();
    }
    else if (want == FOX_RADIO_WIFI_SNIFF)
    {
        if (poom_wifi_ctrl_init_apsta() == ESP_OK)
        {
            (void)poom_wifi_ctrl_set_channel(s_target_channel);
            (void)poom_wifi_ctrl_set_promiscuous_rx_cb(fox_client_promisc_cb_);
            (void)poom_wifi_ctrl_set_promiscuous(true);
        }
    }

    s_radio = want;
}

/* One blocking Wi-Fi scan; populates the AP list. */
static void fox_wifi_scan_list_(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t scan_rc = poom_wifi_scanner_scan_configured(&cfg);
    s_wifi_scanning = false;
    if (scan_rc != ESP_OK)
    {
        return;
    }

    poom_wifi_scanner_ap_records_t *recs = poom_wifi_scanner_get_ap_records();
    portENTER_CRITICAL(&s_lock);
    s_list_count = 0;
    portEXIT_CRITICAL(&s_lock);
    if (recs == NULL)
    {
        return;
    }
    for (uint16_t i = 0; (i < recs->count) && (s_list_count < FOX_LIST_MAX); ++i)
    {
        fox_item_t *it = &s_list[s_list_count];
        (void)memset(it, 0, sizeof(*it));
        it->used = true;
        it->is_wifi = true;
        (void)memcpy(it->addr, recs->records[i].bssid, 6);
        it->channel = recs->records[i].primary;
        it->rssi = recs->records[i].rssi;
        it->rssi_known = true;
        it->last_seen_us = esp_timer_get_time();

        /* Reuse WIFI DETECT's existing classification rules.  Manufacturer
         * component OUIs are deliberately not presented as Flock devices. */
        poom_wifi_detector_record_t classified;
        poom_wifi_detector_classify_ap(&recs->records[i], &classified);
        if((classified.device_class != POOM_WIFI_DETECTOR_CLASS_UNKNOWN) &&
           (classified.device_class != POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER))
        {
            (void)snprintf(it->name, sizeof(it->name), "%.19s", classified.vendor);
        }
        else if(recs->records[i].ssid[0] != '\0')
        {
            (void)snprintf(it->name, sizeof(it->name), "%.19s",
                           (const char *)recs->records[i].ssid);
        }
        else
        {
            (void)snprintf(it->name, sizeof(it->name), "<hidden>");
        }
        s_list_count++;
    }
}

/* One quick single-channel scan to refresh the target AP's RSSI. */
static void fox_wifi_hunt_poll_(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = s_target_channel,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 40, .max = FOX_WIFI_SCAN_MS}},
    };
    if (poom_wifi_scanner_scan_configured(&cfg) != ESP_OK)
    {
        return;
    }
    poom_wifi_scanner_ap_records_t *recs = poom_wifi_scanner_get_ap_records();
    if (recs == NULL)
    {
        return;
    }
    for (uint16_t i = 0; i < recs->count; ++i)
    {
        if (memcmp(recs->records[i].bssid, s_target_addr, 6) == 0)
        {
            fox_report_rssi_(recs->records[i].rssi);
            return;
        }
    }
}

/* ======================================================================== */
/* Rendering                                                                */
/* ======================================================================== */

static int16_t fox_title_x_(const char *title)
{
    const size_t len = (title != NULL) ? strlen(title) : 0U;
    const int width = (int)(len * 6U);
    const int x = ((int)ARDUBOY_WIDTH - width) / 2;
    return (int16_t)((x > 0) ? x : 0);
}

static void fox_draw_header_(const char *title)
{
    poom_arduboy_set_cursor(fox_title_x_(title), 2);
    (void)poom_arduboy_print((title != NULL) ? title : "FOX HUNT");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void fox_draw_footer_(const char *left, const char *right)
{
    if(left != NULL) { poom_arduboy_set_cursor(0, 56); (void)poom_arduboy_print(left); }
    if(right != NULL) { poom_arduboy_set_cursor(72, 56); (void)poom_arduboy_print(right); }
}

static uint32_t fox_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void fox_format_list_label_(char *out, size_t out_len, const char *label,
                                   bool selected)
{
    const size_t label_len = (label != NULL) ? strnlen(label, 19U) : 0U;
    if((out == NULL) || (out_len == 0U)) return;
    out[0] = '\0';
    if(label == NULL) return;
    if(!selected || (label_len <= FOX_LIST_LABEL_CHARS))
    {
        (void)snprintf(out, out_len, "%.*s", (int)FOX_LIST_LABEL_CHARS, label);
        return;
    }

    const size_t cycle = label_len + FOX_LIST_SCROLL_GAP;
    const size_t phase = (size_t)(((fox_now_ms_() - s_list_selection_tick_ms) /
                                   FOX_LIST_SCROLL_MS) % cycle);
    for(size_t i = 0U; (i < FOX_LIST_LABEL_CHARS) && (i + 1U < out_len); ++i)
    {
        const size_t src = (phase + i) % cycle;
        out[i] = (src < label_len) ? label[src] : ' ';
        out[i + 1U] = '\0';
    }
}

static void fox_draw_list_(const char *title, const char *empty_hint)
{
    char title_line[22];
    char name_line[16];
    char rssi_line[10];
    int max_scroll;
    int i;

    (void)snprintf(title_line, sizeof(title_line), "%s%s", title, s_show_mac ? " [MAC]" : "");
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    fox_draw_header_(title_line);
    fox_draw_footer_("A:SELECT", s_wifi_scanning ? "B:CANCEL" : "B:BACK");
    if(s_list_count == 0)
    {
        poom_arduboy_set_cursor(22, 28);
        (void)poom_arduboy_print(empty_hint);
        poom_arduboy_display();
        return;
    }
    if(s_sel < s_scroll) s_scroll = s_sel;
    if(s_sel >= (s_scroll + FOX_VISIBLE_ROWS)) s_scroll = s_sel - FOX_VISIBLE_ROWS + 1;
    max_scroll = s_list_count - FOX_VISIBLE_ROWS;
    if(max_scroll < 0) max_scroll = 0;

    for(i = 0; i < FOX_VISIBLE_ROWS; ++i)
    {
        const int idx = s_scroll + i;
        const int16_t y = (int16_t)(14 + i * 10);
        if(idx >= s_list_count) break;
        if(s_show_mac)
        {
            const uint8_t *m = s_list[idx].addr;
            (void)snprintf(name_line, sizeof(name_line), "%02X%02X%02X%02X%02X%02X",
                           m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        else fox_format_list_label_(name_line, sizeof(name_line), s_list[idx].name,
                                    idx == s_sel);
        if(s_list[idx].rssi_known) (void)snprintf(rssi_line, sizeof(rssi_line), "%ddBm", s_list[idx].rssi);
        else (void)snprintf(rssi_line, sizeof(rssi_line), "---");
        poom_arduboy_set_cursor(2, y); (void)poom_arduboy_print(name_line);
        poom_arduboy_set_cursor(83, y); (void)poom_arduboy_print(rssi_line);
        if(idx == s_sel) poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
    }
    if(s_scroll > 0) poom_arduboy_fill_triangle(124, 11, 120, 15, 127, 15, WHITE);
    if(s_scroll < max_scroll) poom_arduboy_fill_triangle(120, 50, 127, 50, 124, 54, WHITE);
    poom_arduboy_display();
}

static void fox_draw_menu_(const char *title, const char *const *items, int count)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    fox_draw_header_(title);
    fox_draw_footer_("A:SELECT", "B:EXIT");
    for(int i = 0; i < count; ++i)
    {
        const int16_t y = (int16_t)(14 + i * 10);
        poom_arduboy_set_cursor(2, y); (void)poom_arduboy_print(items[i]);
        if(i == s_sel) poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
    }
    poom_arduboy_display();
}

static void fox_draw_options_(void)
{
    char line[22];
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    fox_draw_header_("OPTIONS");
    fox_draw_footer_("L/R:SET", "B:BACK");
    for(int i = 0; i < FOX_OPT_COUNT; ++i)
    {
        const int16_t y = (int16_t)(14 + i * 10);
        const char *name = (i == 0) ? "AUDIO" : "LEDS";
        const int pct = (i == 0) ? s_vol_pct : s_bright_pct;
        (void)snprintf(line, sizeof(line), "%-7s %3d%%", name, pct);
        poom_arduboy_set_cursor(2, y); (void)poom_arduboy_print(line);
        if(i == s_sel) poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
    }
    poom_arduboy_display();
}

static void fox_draw_hunt_(bool searching, const char *lvl_name, float heat,
                           int trend, int now_dbm, int peak_dbm)
{
    char buf[24];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(0, 0);
    (void)poom_arduboy_print(s_target_name);

    if (searching)
    {
        poom_arduboy_set_text_size(2);
        poom_arduboy_set_cursor(4, 24);
        (void)poom_arduboy_print("SEARCHING");
        if ((s_hud[0] != '\0') && (esp_timer_get_time() < s_hud_until_us))
        {
            poom_arduboy_set_text_size(1);
            poom_arduboy_set_cursor(0, 56);
            (void)poom_arduboy_print(s_hud);
        }
        poom_arduboy_display();
        return;
    }

    int len = (int)strlen(lvl_name);
    int x = (128 - (len * 12)) / 2;
    if (x < 0)
    {
        x = 0;
    }
    poom_arduboy_set_text_size(2);
    poom_arduboy_set_cursor(x, 16);
    (void)poom_arduboy_print(lvl_name);

    if (trend > 0)
    {
        poom_arduboy_fill_triangle(118, 8, 126, 8, 122, 1, WHITE);
    }
    else if (trend < 0)
    {
        poom_arduboy_fill_triangle(118, 1, 126, 1, 122, 8, WHITE);
    }

    int fill_w = (int)(118.0f * heat);
    if (fill_w < 0) { fill_w = 0; }
    if (fill_w > 118) { fill_w = 118; }
    poom_arduboy_draw_rect(3, 40, 122, 13, WHITE);
    if (fill_w > 0)
    {
        poom_arduboy_fill_rect(5, 42, fill_w, 9, WHITE);
    }

    poom_arduboy_set_text_size(1);
    if ((s_hud[0] != '\0') && (esp_timer_get_time() < s_hud_until_us))
    {
        (void)snprintf(buf, sizeof(buf), "%s", s_hud); /* VOL/LED readout */
    }
    else if (s_target_client)
    {
        /* Client hunt: show the deauth state unambiguously (A+UP toggles it). */
        (void)snprintf(buf, sizeof(buf), "%s now %d",
                       s_deauth_on ? "DEAUTH:ON" : "DEAUTH:OFF", now_dbm);
    }
    else
    {
        (void)snprintf(buf, sizeof(buf), "now %d  best %d", now_dbm, peak_dbm);
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(buf);
    poom_arduboy_display();
}

/* ======================================================================== */
/* Screen transitions (run in task context)                                */
/* ======================================================================== */

static void fox_enter_screen_(fox_screen_t scr)
{
    s_screen = scr;
    s_sel = 0;
    s_scroll = 0;
    s_list_selection_tick_ms = fox_now_ms_();

    if (scr == SCR_BLE_LIST)
    {
        /* Populate the list only from live BLE advertisements. */
        portENTER_CRITICAL(&s_lock);
        s_list_count = 0;
        portEXIT_CRITICAL(&s_lock);
        fox_radio_want_(FOX_RADIO_BLE);
    }
    else if (scr == SCR_WIFI_LIST || scr == SCR_WIFI_CLIENT_AP)
    {
        /* Both pick an AP from one active scan; CLIENT_AP then drills into the
         * AP's clients, WIFI_LIST hunts the AP directly. */
        const char *title = (scr == SCR_WIFI_CLIENT_AP) ? "CLIENT AP" : "WIFI AP";
        if (scr == SCR_WIFI_CLIENT_AP)
        {
            s_cli_cache_count = 0; /* picking a new AP invalidates the cache. */
        }
        fox_client_monitor_stop_();
        fox_radio_want_(FOX_RADIO_WIFI);
        s_list_count = 0;
        s_scan_cancelled = false;
        s_wifi_scanning = true;
        fox_draw_list_(title, "scanning...");
        fox_wifi_scan_list_();
        if(s_scan_cancelled)
        {
            s_scan_cancelled = false;
            portENTER_CRITICAL(&s_lock);
            s_back_pressed = false;
            s_select_pressed = false;
            portEXIT_CRITICAL(&s_lock);
            s_screen = SCR_MAIN;
            s_sel = 0;
            s_scroll = 0;
            fox_radio_want_(FOX_RADIO_NONE);
            s_cancel_back_ignore_until_us = esp_timer_get_time() + 1500000LL;
            fox_draw_menu_("FOX HUNT", s_main_items, FOX_MAIN_COUNT);
        }
    }
    else if (scr == SCR_WIFI_CLIENT_LIST)
    {
        /* Hand the radio to the detector's passive client monitor. */
        fox_radio_want_(FOX_RADIO_NONE);
        /* Restore the cached list for this AP so the screen is populated
         * immediately (the live monitor then refreshes it in place). */
        portENTER_CRITICAL(&s_lock);
        if ((s_cli_cache_count > 0) &&
            (memcmp(s_cli_cache_ap, s_target_ap_bssid, 6) == 0))
        {
            (void)memcpy(s_list, s_cli_cache,
                         sizeof(fox_item_t) * (size_t)s_cli_cache_count);
            s_list_count = s_cli_cache_count;
        }
        else
        {
            s_list_count = 0;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!fox_client_monitor_start_())
        {
            /* Monitor failed to start; fall back to the AP picker. */
            fox_enter_screen_(SCR_WIFI_CLIENT_AP);
        }
    }
    else if (scr == SCR_HUNT)
    {
        fox_client_monitor_stop_();
        fox_rssi_reset_();
        s_led_level = -2;
        if (s_target_client)
        {
            s_deauth_on = false; /* off by default; A+UP turns it on when needed. */
            s_last_deauth_us = 0;
            fox_radio_want_(FOX_RADIO_WIFI_SNIFF);
        }
        else
        {
            fox_radio_want_(s_target_wifi ? FOX_RADIO_WIFI : FOX_RADIO_BLE);
        }
    }
    else
    {
        fox_client_monitor_stop_();
        fox_radio_want_(FOX_RADIO_NONE);
    }
}

/* ======================================================================== */
/* Task                                                                     */
/* ======================================================================== */

static void fox_task_(void *arg)
{
    bool have_smoothed = false;
    bool have_peak = false;
    float smoothed = FOX_HEAT_MIN_DBM;
    float peak = FOX_HEAT_MIN_DBM;
    int level = FOX_LEVEL_SEARCHING;
    int trend = 0;
    float trend_ref = FOX_HEAT_MIN_DBM;
    int64_t trend_ref_us = 0;
    int64_t last_beep_us = 0;
    int64_t last_wifi_poll_us = 0;
    int64_t last_list_draw_us = 0;
    (void)arg;

    buzzer_init(PIN_NUM_BUZZER);

    /* Take the LED strip over once and keep it (no teardown churn). */
    if (!s_strip_inited)
    {
        s_rainbow_was_running = poom_led_rainbow_deinit();
        if (ws2812_init(&s_strip, PIN_NUM_WS2812, PIN_NUM_LEDS, false,
                        (10 * 1000 * 1000)) == ESP_OK)
        {
            ws2812_set_brightness(&s_strip, FOX_LED_BRIGHT);
            s_strip_inited = true;
        }
        else
        {
            poom_led_rainbow_init();
            if (s_rainbow_was_running)
            {
                (void)poom_led_rainbow_start();
            }
            s_rainbow_was_running = false;
        }
    }
    s_led_ok = s_strip_inited;
    if (s_led_ok)
    {
        ws2812_clear(&s_strip);
        (void)ws2812_show(&s_strip);
    }

    fox_enter_screen_(s_start_in_hunt ? SCR_HUNT : SCR_MAIN);

    while (s_active)
    {
        if (s_exit_requested)
        {
            break;
        }

        /* ---- consume queued input ---- */
        int delta;
        int lvl_delta;
        bool sel;
        bool back;
        bool rstpk;
        bool toggle_label;
        portENTER_CRITICAL(&s_lock);
        delta = s_nav_delta;
        s_nav_delta = 0;
        lvl_delta = s_level_delta;
        s_level_delta = 0;
        toggle_label = s_toggle_label;
        s_toggle_label = false;
        sel = s_select_pressed;
        s_select_pressed = false;
        back = s_back_pressed;
        s_back_pressed = false;
        rstpk = s_reset_peak_requested;
        s_reset_peak_requested = false;
        portEXIT_CRITICAL(&s_lock);

        fox_screen_t scr = s_screen;

        if (scr == SCR_MAIN)
        {
            if (delta != 0)
            {
                s_sel = (s_sel + delta + FOX_MAIN_COUNT) % FOX_MAIN_COUNT;
            }
            if(back && (esp_timer_get_time() >= s_cancel_back_ignore_until_us))
            {
                break; /* exit whole app */
            }
            if (sel)
            {
                if (s_sel == 0) { fox_enter_screen_(SCR_WIFI_LIST); }
                else if (s_sel == 1) { fox_enter_screen_(SCR_WIFI_CLIENT_AP); }
                else if (s_sel == 2) { fox_enter_screen_(SCR_BLE_LIST); }
                else { fox_enter_screen_(SCR_OPTIONS); }
                continue;
            }
            fox_draw_menu_("FOX HUNT", s_main_items, FOX_MAIN_COUNT);
        }
        else if (scr == SCR_OPTIONS)
        {
            if (delta != 0)
            {
                s_sel = (s_sel + delta + FOX_OPT_COUNT) % FOX_OPT_COUNT;
                /* Moving onto/off the LEDS row updates the live preview. */
                fox_led_preview_(s_sel == 1 ? s_bright_pct : 0);
            }
            if (lvl_delta != 0)
            {
                int *p = (s_sel == 0) ? &s_vol_pct : &s_bright_pct;
                int v = *p + (lvl_delta * FOX_PCT_STEP);
                if (v < 0) { v = 0; }
                if (v > 100) { v = 100; }
                *p = v;
                fox_preferences_save_();
                if (s_sel == 0)
                {
                    /* Preview the new volume with a short beep. */
                    buzzer_set_volume(v);
                    if (v > 0) { buzzer_tone(2200U, 40U); }
                }
                else
                {
                    fox_led_preview_(v);
                }
            }
            if (back)
            {
                if (s_led_ok) { ws2812_clear(&s_strip); (void)ws2812_show(&s_strip); s_led_level = -2; }
                fox_enter_screen_(SCR_MAIN);
                continue;
            }
            fox_draw_options_();
        }
        else if (scr == SCR_BLE_LIST || scr == SCR_WIFI_LIST ||
                 scr == SCR_WIFI_CLIENT_AP || scr == SCR_WIFI_CLIENT_LIST)
        {
            if (toggle_label)
            {
                s_show_mac = !s_show_mac; /* LEFT: toggle name <-> MAC */
            }
            if(delta != 0 && s_list_count > 0)
            {
                s_sel = (s_sel + delta + s_list_count) % s_list_count;
                s_list_selection_tick_ms = fox_now_ms_();
            }
            if (back)
            {
                /* CLIENT_LIST steps back to the AP picker; others to the menu. */
                fox_enter_screen_(scr == SCR_WIFI_CLIENT_LIST ? SCR_WIFI_CLIENT_AP
                                                              : SCR_MAIN);
                continue;
            }
            if (sel && s_list_count > 0 && s_sel < s_list_count)
            {
                portENTER_CRITICAL(&s_lock);
                fox_item_t it = s_list[s_sel];
                portEXIT_CRITICAL(&s_lock);

                if (scr == SCR_WIFI_CLIENT_AP)
                {
                    /* Chose an AP; drill into its clients. */
                    (void)memcpy(s_target_ap_bssid, it.addr, 6);
                    s_target_channel = it.channel;
                    fox_enter_screen_(SCR_WIFI_CLIENT_LIST);
                    continue;
                }

                if (scr == SCR_WIFI_CLIENT_LIST)
                {
                    s_target_client = true;
                    s_target_wifi = true;
                    (void)memcpy(s_target_addr, it.addr, 6);
                    /* s_target_channel / s_target_ap_bssid already set. */
                    s_hunt_return = SCR_WIFI_CLIENT_LIST;
                    /* Cache the list so returning from the hunt is instant. */
                    portENTER_CRITICAL(&s_lock);
                    (void)memcpy(s_cli_cache, s_list,
                                 sizeof(fox_item_t) * (size_t)s_list_count);
                    s_cli_cache_count = s_list_count;
                    (void)memcpy(s_cli_cache_ap, s_target_ap_bssid, 6);
                    portEXIT_CRITICAL(&s_lock);
                }
                else
                {
                    s_target_client = false;
                    s_target_wifi = it.is_wifi;
                    (void)memcpy(s_target_addr, it.addr, 6);
                    s_target_channel = it.channel;
                    s_hunt_return = scr;
                }
                (void)snprintf(s_target_name, sizeof(s_target_name), "%.19s", it.name);
                have_smoothed = false;
                have_peak = false;
                level = FOX_LEVEL_SEARCHING;
                trend = 0;
                fox_enter_screen_(SCR_HUNT);
                continue;
            }

            int64_t now_us = esp_timer_get_time();
            if (scr == SCR_BLE_LIST)
            {
                /* Age out stale BLE entries. */
                portENTER_CRITICAL(&s_lock);
                for (int i = 0; i < s_list_count; )
                {
                    if ((now_us - s_list[i].last_seen_us) > FOX_LIST_STALE_US)
                    {
                        for (int j = i; j < s_list_count - 1; ++j)
                        {
                            s_list[j] = s_list[j + 1];
                        }
                        s_list_count--;
                        if (s_sel >= s_list_count && s_sel > 0) { s_sel--; }
                    }
                    else
                    {
                        ++i;
                    }
                }
                portEXIT_CRITICAL(&s_lock);
            }
            else if (scr == SCR_WIFI_CLIENT_LIST)
            {
                fox_client_list_refresh_();
            }
            if ((now_us - last_list_draw_us) > (FOX_LIST_REFRESH_MS * 1000LL))
            {
                const char *title = "WIFI AP";
                const char *empty = "no APs";
                if (scr == SCR_BLE_LIST)      { title = "BLE BEACON";   empty = "scanning..."; }
                else if (scr == SCR_WIFI_CLIENT_AP)   { title = "CLIENT AP"; }
                else if (scr == SCR_WIFI_CLIENT_LIST) { title = "CLIENTS";   empty = "listening..."; }
                last_list_draw_us = now_us;
                fox_draw_list_(title, empty);
            }
        }
        else if (scr == SCR_HUNT)
        {
            if (back)
            {
                if (s_led_ok)
                {
                    ws2812_clear(&s_strip);
                    (void)ws2812_show(&s_strip);
                    s_led_level = -2;
                }
                if (s_return_to_detector || s_return_to_ble_detector)
                {
                    /* Launched from a detector: exit and hand control back to
                     * the detector at that AP's client list (teardown does it). */
                    s_exit_requested = true;
                    continue;
                }
                fox_enter_screen_(s_hunt_return);
                continue;
            }
            (void)rstpk; /* reset-peak removed; A is the FN modifier now. */

            /* A+UP toggles the constant deauth overlay (client hunt only). */
            if (s_toggle_deauth)
            {
                s_toggle_deauth = false;
                if (s_target_client)
                {
                    s_deauth_on = !s_deauth_on;
                    (void)snprintf(s_hud, sizeof(s_hud),
                                   s_deauth_on ? "DEAUTH ON" : "DEAUTH OFF");
                    s_hud_until_us = esp_timer_get_time() + 1200000LL;
                }
            }
            /* A+DOWN fires one manual deauth nudge to wake a quiet client. */
            if (s_burst_deauth)
            {
                s_burst_deauth = false;
                if (s_target_client)
                {
                    fox_client_deauth_send_(FOX_DEAUTH_NUDGE);
                    s_last_deauth_us = esp_timer_get_time();
                    (void)snprintf(s_hud, sizeof(s_hud), "DEAUTH x%d", FOX_DEAUTH_NUDGE);
                    s_hud_until_us = esp_timer_get_time() + 1200000LL;
                }
            }

            /* Live tuning: UP/DOWN = volume, LEFT/RIGHT = brightness (20% steps). */
            if (delta != 0)
            {
                s_vol_pct += (-delta) * FOX_PCT_STEP; /* UP(delta<0) raises volume */
                if (s_vol_pct < 0) { s_vol_pct = 0; }
                if (s_vol_pct > 100) { s_vol_pct = 100; }
                fox_preferences_save_();
                (void)snprintf(s_hud, sizeof(s_hud), "VOL %d%%", s_vol_pct);
                s_hud_until_us = esp_timer_get_time() + 1200000LL;
            }
            if (lvl_delta != 0)
            {
                s_bright_pct += lvl_delta * FOX_PCT_STEP; /* RIGHT raises brightness */
                if (s_bright_pct < 0) { s_bright_pct = 0; }
                if (s_bright_pct > 100) { s_bright_pct = 100; }
                fox_preferences_save_();
                s_led_level = -2; /* repaint LEDs at the new brightness */
                (void)snprintf(s_hud, sizeof(s_hud), "LED %d%%", s_bright_pct);
                s_hud_until_us = esp_timer_get_time() + 1200000LL;
            }

            int64_t now_us = esp_timer_get_time();

            if (s_target_client)
            {
                /* RSSI arrives via the promiscuous callback; keep the client
                 * transmitting by nudging it with unicast deauth bursts. */
                if (s_deauth_on &&
                    ((now_us - s_last_deauth_us) > (FOX_DEAUTH_GAP_MS * 1000LL)))
                {
                    s_last_deauth_us = now_us;
                    fox_client_deauth_tick_();
                    now_us = esp_timer_get_time();
                }
            }
            else if (s_target_wifi &&
                ((now_us - last_wifi_poll_us) > (FOX_WIFI_SCAN_MS * 1000LL + 40000LL)))
            {
                last_wifi_poll_us = now_us;
                fox_wifi_hunt_poll_();
                now_us = esp_timer_get_time();
            }

            int samples[FOX_RING];
            int n;
            int64_t last_us;
            portENTER_CRITICAL(&s_lock);
            n = s_raw_count;
            for (int i = 0; i < n; ++i) { samples[i] = s_raw_ring[i]; }
            last_us = s_last_rx_us;
            portEXIT_CRITICAL(&s_lock);

            int64_t stale_us = s_target_client ? FOX_CLIENT_STALE_US : FOX_STALE_US;
            int64_t hold_us  = s_target_client ? FOX_CLIENT_HOLD_US  : FOX_HOLD_US;
            int64_t age = now_us - last_us;
            bool fresh = (n > 0) && (age < stale_us);
            bool coast = have_smoothed && (n > 0) && (age < hold_us);
            if (fresh)
            {
                int med = fox_median_(samples, n);
                if (!have_smoothed)
                {
                    smoothed = (float)med;
                    have_smoothed = true;
                    trend_ref = smoothed;
                    trend_ref_us = now_us;
                }
                else
                {
                    smoothed += FOX_EMA_ALPHA * ((float)med - smoothed);
                }
                if (!have_peak || (smoothed > peak))
                {
                    peak = smoothed;
                    have_peak = true;
                }
                level = fox_level_from_rssi_(smoothed, level);
                if ((now_us - trend_ref_us) >= (FOX_TREND_MS * 1000LL))
                {
                    float d = smoothed - trend_ref;
                    trend = (d > FOX_TREND_DB) ? 1 : ((d < -FOX_TREND_DB) ? -1 : 0);
                    trend_ref = smoothed;
                    trend_ref_us = now_us;
                }
                float hmin = s_target_wifi ? FOX_HEAT_MIN_WIFI : FOX_HEAT_MIN_BLE;
                float hmax = s_target_wifi ? FOX_HEAT_MAX_WIFI : FOX_HEAT_MAX_BLE;
                float heat = fox_clampf_((smoothed - hmin) / (hmax - hmin), 0.0f, 1.0f);
                fox_set_led_(level);
                fox_draw_hunt_(false, s_level_name[level], heat, trend,
                               (int)smoothed, (int)peak);
                if ((s_vol_pct > 0) &&
                    ((now_us - last_beep_us) >= ((int64_t)s_level_beep_ms[level] * 1000LL)))
                {
                    last_beep_us = now_us;
                    buzzer_set_volume(s_vol_pct);
                    buzzer_tone(s_level_beep_hz[level], FOX_BEEP_MS);
                }
            }
            else if (coast)
            {
                /* Samples briefly stopped (a quiet client, or a missed scan).
                 * Coast on the last reading instead of snapping to SEARCHING. */
                float hmin = s_target_wifi ? FOX_HEAT_MIN_WIFI : FOX_HEAT_MIN_BLE;
                float hmax = s_target_wifi ? FOX_HEAT_MAX_WIFI : FOX_HEAT_MAX_BLE;
                float heat = fox_clampf_((smoothed - hmin) / (hmax - hmin), 0.0f, 1.0f);
                fox_set_led_(level);
                fox_draw_hunt_(false, s_level_name[level], heat, 0,
                               (int)smoothed, (int)peak);
                if ((s_vol_pct > 0) &&
                    ((now_us - last_beep_us) >= ((int64_t)s_level_beep_ms[level] * 1000LL)))
                {
                    last_beep_us = now_us;
                    buzzer_set_volume(s_vol_pct);
                    buzzer_tone(s_level_beep_hz[level], FOX_BEEP_MS);
                }
            }
            else
            {
                level = FOX_LEVEL_SEARCHING;
                trend = 0;
                have_smoothed = false;
                fox_set_led_(FOX_LEVEL_SEARCHING);
                fox_draw_hunt_(true, NULL, 0.0f, 0, 0, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FOX_TICK_MS));
    }

    /* ---- teardown ---- */
    fox_client_monitor_stop_();
    fox_radio_want_(FOX_RADIO_NONE);
    poom_ble_scan_register_cb(NULL);
    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", fox_button_cb_, "menu_fox_hunt");
        s_buttons_subscribed = false;
    }
    buzzer_tone(0U, 0U);
    buzzer_set_volume(100U); /* restore full volume for other apps. */
    if (s_led_ok)
    {
        ws2812_clear(&s_strip);
        (void)ws2812_show(&s_strip);
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)ws2812_show(&s_strip);
        s_led_ok = false;
    }

    ESP_LOGI(FOX_TAG, "fox hunt stopped");
    s_active = false;
    s_task = NULL;
    fox_free_session_();

    if (s_return_to_ble_detector)
    {
        s_return_to_ble_detector = false;
        menu_ble_detector_show();
    }
    else if (s_return_to_detector)
    {
        /* Return to WIFI DETECT at the hunted client's AP list; the detector
         * publishes the resume token itself when it exits. */
        s_return_to_detector = false;
        menu_wifi_detector_show_clients(s_target_ap_bssid, s_target_channel);
    }
    else
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
    vTaskDelete(NULL);
}

/* ======================================================================== */
/* Input                                                                    */
/* ======================================================================== */

static void fox_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    button_event_msg_t ev;
    bool stop_wifi_scan = false;

    (void)user_ctx;
    if(!s_active || (msg == NULL) || (msg->len < sizeof(ev))) return;
    (void)memcpy(&ev, msg->data, sizeof(ev));
    if(ev.button == BUTTON_A)
    {
        if(ev.event == BUTTON_PRESS_DOWN) s_a_held = true;
        else if(ev.event == BUTTON_PRESS_UP) s_a_held = false;
    }
    if(ev.event != BUTTON_SINGLE_CLICK) return;

    portENTER_CRITICAL(&s_lock);
    switch(ev.button)
    {
        case BUTTON_UP:
            if((s_screen == SCR_HUNT) && s_a_held) s_toggle_deauth = true;
            else s_nav_delta -= 1;
            break;
        case BUTTON_DOWN:
            if((s_screen == SCR_HUNT) && s_a_held) s_burst_deauth = true;
            else s_nav_delta += 1;
            break;
        case BUTTON_LEFT:
            if(!((s_screen == SCR_HUNT) && s_a_held)) { s_level_delta -= 1; s_toggle_label = true; }
            break;
        case BUTTON_RIGHT:
            if(!((s_screen == SCR_HUNT) && s_a_held)) s_level_delta += 1;
            break;
        case BUTTON_A:
            if(s_screen != SCR_HUNT) s_select_pressed = true;
            break;
        case BUTTON_B:
            if(s_wifi_scanning)
            {
                /* The Wi-Fi driver call must execute after leaving the spinlock. */
                s_scan_cancelled = true;
                stop_wifi_scan = true;
            }
            else s_back_pressed = true;
            break;
        default:
            break;
    }
    portEXIT_CRITICAL(&s_lock);

    if(stop_wifi_scan) (void)esp_wifi_scan_stop();
}

/* Shared reset of all run state before (re)starting the hunt task. */
static void fox_reset_state_(void)
{
    s_active = true;
    s_exit_requested = false;
    s_nav_delta = 0;
    s_level_delta = 0;
    s_toggle_label = false;
    s_show_mac = false;
    s_select_pressed = false;
    s_back_pressed = false;
    s_reset_peak_requested = false;
    s_cancel_back_ignore_until_us = 0;
    s_toggle_deauth = false;
    s_burst_deauth = false;
    s_a_held = false;
    s_screen = SCR_MAIN;
    s_sel = 0;
    s_scroll = 0;
    s_list_count = 0;
    s_radio = FOX_RADIO_NONE;
    s_led_level = -2;
    s_target_client = false;
    s_deauth_on = false;
    s_monitor_on = false;
    s_start_in_hunt = false;
    s_return_to_detector = false;
    s_return_to_ble_detector = false;
    s_hunt_return = SCR_MAIN;
    s_cli_cache_count = 0;
    fox_rssi_reset_();
}

/* Subscribe to buttons and spawn the hunt task; publishes resume on failure. */
static void fox_launch_task_(void)
{
    s_buttons_subscribed =
        poom_sbus_subscribe_cb("input/button", fox_button_cb_, "menu_fox_hunt");

    if (xTaskCreate(fox_task_, "menu_fox_hunt", 5120, NULL, 5, &s_task) != pdPASS)
    {
        ESP_LOGE(FOX_TAG, "task create failed");
        if (s_buttons_subscribed)
        {
            (void)poom_sbus_unsubscribe_cb("input/button", fox_button_cb_, "menu_fox_hunt");
            s_buttons_subscribed = false;
        }
        s_active = false;
        s_task = NULL;
        fox_free_session_();
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}

void app_fox_hunt(void)
{
    if (s_active)
    {
        return;
    }
    fox_preferences_load_();
    if(!fox_alloc_session_())
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    fox_reset_state_();
    fox_launch_task_();
}

void app_fox_hunt_wifi_client(const uint8_t ap_bssid[6],
                              uint8_t channel,
                              const uint8_t client_mac[6],
                              const char *name)
{
    if (s_active || (ap_bssid == NULL) || (client_mac == NULL) || (channel == 0U))
    {
        return;
    }
    fox_preferences_load_();
    if(!fox_alloc_session_())
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    fox_reset_state_();

    s_target_client = true;
    s_target_wifi = true;
    s_target_channel = channel;
    (void)memcpy(s_target_ap_bssid, ap_bssid, 6);
    (void)memcpy(s_target_addr, client_mac, 6);
    if ((name != NULL) && (name[0] != '\0'))
    {
        (void)snprintf(s_target_name, sizeof(s_target_name), "%.19s", name);
    }
    else
    {
        (void)snprintf(s_target_name, sizeof(s_target_name),
                       "%02X%02X%02X%02X%02X%02X",
                       client_mac[0], client_mac[1], client_mac[2],
                       client_mac[3], client_mac[4], client_mac[5]);
    }
    /* Launched from WIFI DETECT straight onto the hunt; B hands control back
     * to the detector at this AP's client list. */
    s_start_in_hunt = true;
    s_return_to_detector = true;
    s_hunt_return = SCR_MAIN;

    fox_launch_task_();
}

void app_fox_hunt_ble_device(const uint8_t address[6], const char *name)
{
    if (s_active || (address == NULL))
    {
        return;
    }
    fox_preferences_load_();
    if(!fox_alloc_session_())
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    fox_reset_state_();
    s_target_wifi = false;
    s_target_client = false;
    (void)memcpy(s_target_addr, address, 6);
    if ((name != NULL) && (name[0] != '\0'))
    {
        (void)snprintf(s_target_name, sizeof(s_target_name), "%.19s", name);
    }
    else
    {
        (void)snprintf(s_target_name, sizeof(s_target_name),
                       "%02X%02X%02X%02X%02X%02X",
                       address[0], address[1], address[2],
                       address[3], address[4], address[5]);
    }
    s_start_in_hunt = true;
    s_return_to_ble_detector = true;
    s_hunt_return = SCR_MAIN;
    fox_launch_task_();
}
