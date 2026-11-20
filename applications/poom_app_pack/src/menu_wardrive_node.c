// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * BEAST > WARDRIVING NODE
 *
 * Turns the POOM into an ESP-NOW scanning node for an external wardriving
 * core.  The core owns GPS and WiGLE logging; the POOM scans the channels the
 * core assigns and forwards every access point it hears as one text record:
 *
 *     BSSID,SSID,[AUTH],CHANNEL,RSSI,W
 *
 * Three cores speak the same "ENOW" wire protocol.  The core kind is read
 * from its CORE_REPLY, so every mode works with every core; the mode picks
 * which core to prefer when several answer, and the screen.
 *
 *  - JCMK NODE: JustCallMeKoko ESP32 dual-band wardriver core.  The core
 *    answers CORE_REQUEST with CORE_REPLY and assigns a slice of its shared
 *    40-entry channel table with a 10-byte binary ADMIN message.  Records and
 *    heartbeats are broadcast, exactly like a stock JCMK node.  The node
 *    scans as soon as it is paired.
 *  - BISCUIT NODE: Biscuit Shop core in plain-reply mode ("encrypt=0" in its
 *    CORE_REPLY).  The node introduces itself (type 7), acknowledges the role
 *    assignment (type 5 -> 15) and the channel plan (type 10 -> 11), and sends
 *    unicast to the core.  Like a Biscuit node it stays idle on the home
 *    channel until the core assigns it the Wi-Fi role, and stops scanning
 *    when the core takes the role away.
 *  - PIGLET NODE: Hamspiced Piglet in Core mode ("PIGLET1" marker in its
 *    CORE_REPLY).  A Piglet core assigns the channel plan Biscuit-style
 *    (type 5 + type 10), records go unicast, and the node scans as soon as
 *    it is paired, like a PigletNode.  The node identifies itself with the
 *    same marker but no board type or version, so a core never offers it a
 *    Piglet firmware update.
 *
 * Wire behaviour follows Just Call Me Koko's ESP32DualBandWardriver and
 * Hamspiced/Piglet. The Biscuit plain-reply handshake and channel-plan
 * reference is Hedge (@CodeHedge / Biscuit Shop)'s contribution to Piglet:
 * https://github.com/Hamspiced/piglet/pull/16
 * The pig on the PIGLET NODE screen is original POOM pixel art.
 *
 * The radio is shared between scanning and ESP-NOW, so the core can only be
 * heard on the home channel (6).  After every channel scan the node is back
 * on channel 6 to forward what it found; after every full cycle it sends a
 * heartbeat and listens for a short admin window.
 */
#include "menu_wardrive_node.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_wifi_ctrl.h"
#include "wardrive_node_logos.h"
#include "wardrive_node_pig.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

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

/* ---- Tasks ------------------------------------------------------------ */
#define WDN_UI_STACK       (3072U)
#define WDN_UI_PRIO        (4U)
#define WDN_UI_TICK_MS     (100U)
#define WDN_NODE_STACK     (4096U)
#define WDN_NODE_PRIO      (5U)
#define WDN_STOP_WAIT_MS   (3000U)

/* ---- ENOW protocol ---------------------------------------------------- */
#define WDN_HOME_CHANNEL   (6U)
#define WDN_TEXT_MAX       (200U)

#define WDN_MSG_CORE_REQUEST (1U)
#define WDN_MSG_CORE_REPLY   (2U)
#define WDN_MSG_HEARTBEAT    (3U)
#define WDN_MSG_TEXT         (4U)
#define WDN_MSG_ADMIN        (5U)  /* JCMK channel range / Biscuit role assign */
#define WDN_MSG_RECORD_ACK   (6U)  /* JCMK core -> node, per record */
#define WDN_MSG_HELLO        (7U)  /* Biscuit: node name + firmware */
#define WDN_MSG_CONFIG       (10U) /* Biscuit: "channels=..;dwell=.." */
#define WDN_MSG_CONFIG_ACK   (11U)
#define WDN_MSG_ROLE_ACK     (15U)

/* Piglet identification, carried in the text of CORE_REQUEST, CORE_REPLY
 * and HEARTBEAT: "PIGLET1", a board-type byte and an 8-byte version. */
#define WDN_PIGLET_MARKER      "PIGLET1"
#define WDN_PIGLET_MARKER_LEN  (7U)
#define WDN_PIGLET_ID_LEN      (16U)
#define WDN_PIGLET_BOARD_NONE  (0xFFU) /* not a Piglet board: never updated */

/* ---- Timing ----------------------------------------------------------- */
#define WDN_REQ_INIT_MS      (300U)   /* CORE_REQUEST backoff start */
#define WDN_REQ_MAX_MS       (5000U)  /* and cap (matches JCMK nodes) */
#define WDN_PREFER_WAIT_MS   (1500U)  /* wait for the preferred core kind */
#define WDN_HANDSHAKE_MS     (1500U)  /* Biscuit role/config window */
#define WDN_ADMIN_WIN_MS     (300U)   /* listen window after each cycle */
#define WDN_IDLE_WIN_MS      (2000U)  /* heartbeat period while waiting to start */
#define WDN_PROBE_AFTER_MS   (30000U) /* silent core: probe with CORE_REQUEST */
#define WDN_LOST_AFTER_MS    (120000U)/* silent core: search again */
#define WDN_DWELL_DEFAULT_MS (80U)    /* JCMK CHANNEL_TIMER */
#define WDN_DWELL_MIN_MS     (40U)
#define WDN_DWELL_MAX_MS     (400U)
#define WDN_PASSIVE_MS       (110U)   /* > one beacon interval for DFS */

/* ---- Tables ----------------------------------------------------------- */
#define WDN_RECENT_LEN     (256U)  /* recently forwarded BSSIDs (JCMK uses 200) */
#define WDN_SEEN_CAP_PSRAM (8192U) /* unique BSSIDs counted on screen */
#define WDN_SEEN_CAP_IRAM  (1024U)
#define WDN_PLAN_MAX       (48U)

typedef enum {
    WDN_KIND_BISCUIT = 0,
    WDN_KIND_JCMK,
    WDN_KIND_PIGLET,
    WDN_KIND_COUNT,
} wdn_kind_t;

/* What the paired core turned out to be (from its CORE_REPLY). */
typedef enum {
    WDN_CORE_JCMK = 0,
    WDN_CORE_BISCUIT,
    WDN_CORE_PIGLET,
} wdn_core_t;

typedef enum {
    WDN_VIEW_PICK = 0,
    WDN_VIEW_RUN,
} wdn_view_t;

typedef enum {
    WDN_LINK_SEARCHING = 0,
    WDN_LINK_LINKED,
    WDN_LINK_WAITING,   /* Biscuit: paired, waiting for the core to start us */
    WDN_LINK_QUIET,
    WDN_LINK_RADIO_ERROR,
} wdn_link_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t type;
    uint32_t counter;
    uint16_t len;
    char text[WDN_TEXT_MAX + 1U];
} wdn_text_msg_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t type;
    uint8_t assignment_version;
    uint8_t node_index;
    uint8_t node_count;
    uint8_t start_channel_idx;
    uint8_t end_channel_idx;
} wdn_admin_msg_t;

_Static_assert(sizeof(wdn_text_msg_t) == 212U, "ENOW text frame must stay 212 bytes");
_Static_assert(sizeof(wdn_admin_msg_t) == 10U, "ENOW admin frame must stay 10 bytes");
_Static_assert(sizeof(wdn_text_msg_t) <= ESP_NOW_MAX_DATA_LEN, "ENOW frame exceeds ESP-NOW v1 payload");

typedef enum {
    WDN_EVT_REPLY = 0,
    WDN_EVT_JCMK_ADMIN,
    WDN_EVT_BISCUIT_ROLE,
    WDN_EVT_BISCUIT_CONFIG,
    WDN_EVT_OTHER,           /* anything else from the core: logged */
} wdn_evt_kind_t;

/* Events copied out of the Wi-Fi task by the receive callback. */
typedef struct {
    uint8_t kind;
    uint8_t mac[6];
    uint8_t core;            /* REPLY: wdn_core_t */
    uint8_t role;            /* BISCUIT_ROLE */
    uint8_t type;            /* OTHER: message type */
    uint16_t len;            /* OTHER: frame length */
    wdn_admin_msg_t admin;   /* JCMK_ADMIN */
    uint8_t text_len;        /* BISCUIT_CONFIG */
    char text[WDN_TEXT_MAX + 1U];
} wdn_evt_t;

typedef struct { uint8_t button; } wdn_button_t;

typedef struct {
    /* lifecycle */
    QueueHandle_t buttons;
    QueueHandle_t events;
    TaskHandle_t ui_task;
    TaskHandle_t node_task;
    portMUX_TYPE lock;
    bool subscribed;
    bool radio_ready;
    volatile bool running;     /* node task + rx callback may work */
    volatile bool node_alive;  /* node task has not exited yet */

    /* ui */
    wdn_view_t view;
    uint8_t pick;
    wdn_kind_t kind;

    /* link (written by node task, read by rx callback and ui) */
    volatile wdn_link_t link;
    volatile bool have_core;
    volatile wdn_core_t core;
    volatile bool scan_enabled;  /* Biscuit: once the core assigns a Wi-Fi role. Others: once paired */
    uint8_t core_mac[6];
    volatile uint32_t core_heard_ms;

    /* channel plan */
    uint8_t plan[WDN_PLAN_MAX];
    uint8_t plan_len;
    uint8_t plan_pos;
    uint16_t dwell_ms;
    uint8_t admin_version;
    bool assigned;             /* the core has sent us a channel plan */
    uint64_t bad_channels_lo;  /* channels 0..63 the driver refused */
    uint64_t bad_channels_hi;  /* channels 64..127 */
    uint64_t bad_channels_top; /* channels 128..191 */

    /* counters shown on screen */
    volatile uint32_t unique_count;
    volatile uint32_t sent_count;
    volatile uint8_t channel;
    uint32_t text_counter;
    uint32_t hb_counter;

    /* dedupe */
    uint8_t recent[WDN_RECENT_LEN][6];
    uint16_t recent_pos;
    uint64_t *seen;
    uint32_t seen_cap;
    uint32_t seen_used;

    /* PIGLET NODE pig (ui task only) */
    struct {
        int16_t x;
        bool left;
        uint8_t mode;          /* WARDRIVE_NODE_PIG_WALK / _DANCE / _TWERK */
        uint8_t tick;
        uint32_t until_ms;     /* end of the current dance or twerk */
        uint32_t next_dance_ms;
        uint32_t next_twerk_ms;
        uint32_t last_count;
        wdn_link_t last_link;
    } pig;
} wdn_session_t;

static const char *TAG = "wardrive_node";
static const char s_wdn_user[] = "menu_wardrive_node";
static const uint8_t s_wdn_magic[4] = {'E', 'N', 'O', 'W'};
static const uint8_t s_wdn_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Shared with the JCMK core: assignments are index ranges into this table,
 * so the order must never change. */
static const uint8_t s_wdn_jcmk_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177,
};
#define WDN_JCMK_CHANNEL_COUNT ((uint8_t)(sizeof(s_wdn_jcmk_channels) / sizeof(s_wdn_jcmk_channels[0])))

static const char *const s_wdn_titles[WDN_KIND_COUNT] = {"BISCUIT NODE", "JCMK NODE", "PIGLET NODE"};
static const char *const s_wdn_hints[WDN_KIND_COUNT] = {"PAIR WITH A BISCUIT CORE", "PAIR WITH A JCMK WARDRIVER",
                                                        "PAIR WITH A PIGLET CORE"};
static const char *const s_wdn_core_names[] = {"jcmk", "biscuit", "piglet"};

/* Exists only while WARDRIVING NODE is open. */
static wdn_session_t *s_wdn = NULL;

/* Survive leaving the screen: the last mode picked, and the last JCMK
 * channel assignment.  A JCMK core only sends ADMIN when a node's slice
 * changes, so a node that comes back within the core's 60 s node timeout
 * would otherwise scan every channel instead of its own slice. */
static uint8_t s_wdn_last_pick = 0U;
static struct {
    bool valid;
    uint8_t core_mac[6];
    wdn_admin_msg_t admin;
} s_wdn_last_jcmk;

static void wdn_exit_(void);

static inline uint32_t wdn_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/* ======================================================================
 * Dedupe: unique BSSIDs (screen counter) and recently forwarded BSSIDs
 * ====================================================================== */

static inline uint64_t wdn_bssid_key_(const uint8_t bssid[6])
{
    uint64_t key = 1ULL << 48; /* never zero, zero marks an empty slot */
    for(uint8_t i = 0U; i < 6U; ++i) key |= (uint64_t)bssid[i] << (40U - (8U * i));
    return key;
}

/* Returns true the first time a BSSID is seen this session. */
static bool wdn_seen_insert_(wdn_session_t *s, const uint8_t bssid[6])
{
    const uint64_t key = wdn_bssid_key_(bssid);
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    uint32_t mask;
    if(s->seen == NULL || s->seen_cap == 0U) return true;
    /* Keep the table under 75% full; when it is, start a new generation.
     * The screen total keeps counting, only very old repeats can re-count. */
    if((s->seen_used * 4U) >= (s->seen_cap * 3U)) {
        memset(s->seen, 0, (size_t)s->seen_cap * sizeof(uint64_t));
        s->seen_used = 0U;
    }
    mask = s->seen_cap - 1U;
    for(uint32_t i = (uint32_t)(h >> 32) & mask;; i = (i + 1U) & mask) {
        if(s->seen[i] == key) return false;
        if(s->seen[i] == 0ULL) {
            s->seen[i] = key;
            ++s->seen_used;
            return true;
        }
    }
}

/* JCMK nodes only forward a BSSID that is not in their recent history, so
 * a busy street does not flood the core with repeats. */
static bool wdn_recent_check_and_add_(wdn_session_t *s, const uint8_t bssid[6])
{
    for(uint16_t i = 0U; i < WDN_RECENT_LEN; ++i) {
        if(memcmp(s->recent[i], bssid, 6U) == 0) return true;
    }
    memcpy(s->recent[s->recent_pos], bssid, 6U);
    s->recent_pos = (uint16_t)((s->recent_pos + 1U) % WDN_RECENT_LEN);
    return false;
}

/* ======================================================================
 * Channel plan
 * ====================================================================== */

static bool wdn_channel_supported_(uint8_t ch)
{
    if(ch >= 1U && ch <= 14U) return true;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    /* UNII-1/2/2e on multiples of 4, UNII-3/4 on 149 + 4n. */
    if(ch >= 36U && ch <= 64U && (ch % 4U) == 0U) return true;
    if(ch >= 100U && ch <= 144U && (ch % 4U) == 0U) return true;
    if(ch >= 149U && ch <= 177U && (ch % 4U) == 1U) return true;
#endif
    return false;
}

static bool wdn_channel_bad_(const wdn_session_t *s, uint8_t ch)
{
    if(ch < 64U) return (s->bad_channels_lo >> ch) & 1ULL;
    if(ch < 128U) return (s->bad_channels_hi >> (ch - 64U)) & 1ULL;
    if(ch < 192U) return (s->bad_channels_top >> (ch - 128U)) & 1ULL;
    return true;
}

static void wdn_channel_mark_bad_(wdn_session_t *s, uint8_t ch)
{
    if(ch < 64U) s->bad_channels_lo |= 1ULL << ch;
    else if(ch < 128U) s->bad_channels_hi |= 1ULL << (ch - 64U);
    else if(ch < 192U) s->bad_channels_top |= 1ULL << (ch - 128U);
}

static void wdn_plan_jcmk_range_(wdn_session_t *s, uint8_t start, uint8_t end)
{
    if(start >= WDN_JCMK_CHANNEL_COUNT || end >= WDN_JCMK_CHANNEL_COUNT || start > end) {
        start = 0U;
        end = (uint8_t)(WDN_JCMK_CHANNEL_COUNT - 1U);
    }
    s->plan_len = 0U;
    for(uint8_t i = start; i <= end && s->plan_len < WDN_PLAN_MAX; ++i) {
        if(wdn_channel_supported_(s_wdn_jcmk_channels[i])) s->plan[s->plan_len++] = s_wdn_jcmk_channels[i];
    }
    s->plan_pos = 0U;
}

/* Parses a Biscuit "channels=1,6,11,36;dwell=80" plan. */
static bool wdn_plan_biscuit_(wdn_session_t *s, const char *text)
{
    uint8_t plan[WDN_PLAN_MAX];
    uint8_t count = 0U;
    uint16_t dwell = s->dwell_ms;
    const char *p;
    if(strncmp(text, "channels=", 9U) != 0) return false;
    p = text + 9;
    while(*p != '\0' && *p != ';') {
        char *next = NULL;
        const long ch = strtol(p, &next, 10);
        if(next == p) return false;
        if(ch > 0 && ch < 256 && wdn_channel_supported_((uint8_t)ch) && count < WDN_PLAN_MAX) {
            bool dup = false;
            for(uint8_t i = 0U; i < count; ++i) dup = dup || (plan[i] == (uint8_t)ch);
            if(!dup) plan[count++] = (uint8_t)ch;
        }
        p = next;
        if(*p == ',') ++p;
        else if(*p != ';' && *p != '\0') return false;
    }
    if(*p == ';') {
        const char *d = strstr(p, "dwell=");
        if(d != NULL) {
            const long v = strtol(d + 6, NULL, 10);
            if(v > 0) dwell = (uint16_t)((v < WDN_DWELL_MIN_MS) ? WDN_DWELL_MIN_MS :
                                         (v > WDN_DWELL_MAX_MS) ? WDN_DWELL_MAX_MS : v);
        }
    }
    if(count == 0U) return false;
    memcpy(s->plan, plan, count);
    s->plan_len = count;
    s->plan_pos = 0U;
    s->dwell_ms = dwell;
    return true;
}

/* ======================================================================
 * ESP-NOW
 * ====================================================================== */

static bool wdn_text_has_(const wdn_text_msg_t *msg, const char *needle)
{
    const size_t n = strlen(needle);
    const size_t len = (msg->len < WDN_TEXT_MAX) ? msg->len : WDN_TEXT_MAX;
    for(size_t i = 0U; i + n <= len; ++i) {
        if(memcmp(&msg->text[i], needle, n) == 0) return true;
    }
    return false;
}

/* Runs in the Wi-Fi task: classify, copy what the node task needs, return. */
static void wdn_rx_(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    wdn_session_t *s = s_wdn;
    wdn_evt_t evt;
    bool from_core;
    uint8_t type;
    if(s == NULL || !s->running || info == NULL || data == NULL || len < 5) return;
    if(memcmp(data, s_wdn_magic, 4U) != 0) return;
    type = data[4];

    portENTER_CRITICAL(&s->lock);
    from_core = s->have_core && memcmp(info->src_addr, s->core_mac, 6U) == 0;
    portEXIT_CRITICAL(&s->lock);
    /* Any frame from the paired core (heartbeat, record ack, admin...) proves
     * the link is alive; most of them need nothing else. */
    if(from_core) s->core_heard_ms = wdn_now_ms_();

    memset(&evt, 0, offsetof(wdn_evt_t, text));
    memcpy(evt.mac, info->src_addr, 6U);
    if(type == WDN_MSG_CORE_REPLY) {
        const wdn_text_msg_t *msg = (const wdn_text_msg_t *)data;
        const bool full = len >= (int)sizeof(wdn_text_msg_t);
        evt.kind = WDN_EVT_REPLY;
        if(full && msg->len >= WDN_PIGLET_MARKER_LEN &&
           memcmp(msg->text, WDN_PIGLET_MARKER, WDN_PIGLET_MARKER_LEN) == 0) evt.core = WDN_CORE_PIGLET;
        else if(full && wdn_text_has_(msg, "encrypt=0")) evt.core = WDN_CORE_BISCUIT;
        else evt.core = WDN_CORE_JCMK;
    } else if(type == WDN_MSG_ADMIN && len >= (int)sizeof(wdn_text_msg_t)) {
        const wdn_text_msg_t *msg = (const wdn_text_msg_t *)data;
        evt.kind = WDN_EVT_BISCUIT_ROLE;
        evt.role = msg->len ? (uint8_t)msg->text[0] : (uint8_t)(msg->counter & 0xFFU);
    } else if(type == WDN_MSG_ADMIN && len >= (int)sizeof(wdn_admin_msg_t)) {
        evt.kind = WDN_EVT_JCMK_ADMIN;
        memcpy(&evt.admin, data, sizeof(evt.admin));
    } else if(type == WDN_MSG_CONFIG && len >= (int)sizeof(wdn_text_msg_t)) {
        const wdn_text_msg_t *msg = (const wdn_text_msg_t *)data;
        const uint16_t n = (msg->len < WDN_TEXT_MAX) ? msg->len : WDN_TEXT_MAX;
        evt.kind = WDN_EVT_BISCUIT_CONFIG;
        evt.text_len = (uint8_t)n;
        memcpy(evt.text, msg->text, n);
        evt.text[n] = '\0';
    } else if(from_core && type != WDN_MSG_HEARTBEAT && type != WDN_MSG_RECORD_ACK) {
        /* Not part of the documented handshake: hand it to the node task so
         * it shows up in the serial log. */
        evt.kind = WDN_EVT_OTHER;
        evt.type = type;
        evt.len = (uint16_t)len;
        if(len >= (int)offsetof(wdn_text_msg_t, text)) {
            const wdn_text_msg_t *msg = (const wdn_text_msg_t *)data;
            uint16_t n = (msg->len < 48U) ? msg->len : 48U;
            if((int)(offsetof(wdn_text_msg_t, text) + n) > len) n = (uint16_t)(len - (int)offsetof(wdn_text_msg_t, text));
            evt.text_len = (uint8_t)n;
            for(uint16_t i = 0U; i < n; ++i) {
                const char c = msg->text[i];
                evt.text[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            evt.text[n] = '\0';
        }
    } else {
        return;
    }
    if(s->events != NULL) (void)xQueueSend(s->events, &evt, 0U);
}

static esp_err_t wdn_send_raw_(const uint8_t dst[6], const void *frame, size_t len)
{
    esp_err_t err = ESP_FAIL;
    /* The ESP-NOW TX queue is short; when a burst fills it, give the radio
     * a tick to drain instead of dropping the record. */
    for(uint8_t attempt = 0U; attempt < 8U; ++attempt) {
        err = esp_now_send(dst, (const uint8_t *)frame, len);
        if(err != ESP_ERR_ESPNOW_NO_MEM) break;
        vTaskDelay(1);
    }
    return err;
}

static esp_err_t wdn_send_(wdn_session_t *s, const uint8_t dst[6], uint8_t type,
                           uint32_t counter, const char *text, size_t text_len)
{
    wdn_text_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.magic, s_wdn_magic, 4U);
    msg.type = type;
    msg.counter = counter;
    if(text != NULL) {
        if(text_len > WDN_TEXT_MAX) text_len = WDN_TEXT_MAX;
        memcpy(msg.text, text, text_len);
        msg.len = (uint16_t)text_len;
    }
    /* Always the full 212-byte frame: Biscuit drops shorter frames and the
     * JCMK core ignores anything shorter than its text struct. */
    return wdn_send_raw_(dst, &msg, sizeof(msg));
}

/* Records and heartbeats: unicast to Biscuit and Piglet cores (they peer
 * their nodes and ack), broadcast to a JCMK core (what stock JCMK nodes do
 * when encryption is off). */
static const uint8_t *wdn_data_dst_(const wdn_session_t *s)
{
    return (s->core == WDN_CORE_JCMK) ? s_wdn_bcast : s->core_mac;
}

static void wdn_home_(wdn_session_t *s)
{
    uint8_t primary = 0U;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&primary, &second) != ESP_OK || primary != WDN_HOME_CHANNEL) {
        (void)esp_wifi_set_channel(WDN_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
    s->channel = WDN_HOME_CHANNEL;
}

static void wdn_add_peer_(const uint8_t mac[6])
{
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6U);
    peer.channel = 0U; /* follow the current channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if(esp_now_is_peer_exist(mac)) (void)esp_now_del_peer(mac);
    (void)esp_now_add_peer(&peer);
}

static bool wdn_radio_start_(wdn_session_t *s)
{
    if(poom_wifi_ctrl_init_sta() != ESP_OK) return false;
    (void)poom_wifi_ctrl_sta_disconnect();
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if(esp_wifi_set_channel(WDN_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
       esp_now_init() != ESP_OK) {
        (void)poom_wifi_ctrl_deinit();
        return false;
    }
    if(esp_now_register_recv_cb(wdn_rx_) != ESP_OK) {
        (void)esp_now_deinit();
        (void)poom_wifi_ctrl_deinit();
        return false;
    }
    wdn_add_peer_(s_wdn_bcast);
    s->channel = WDN_HOME_CHANNEL;
    s->radio_ready = true;
    return true;
}

static void wdn_radio_stop_(wdn_session_t *s)
{
    if(!s->radio_ready) return;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_deinit();
    (void)esp_wifi_scan_stop();
    (void)poom_wifi_ctrl_deinit();
    s->radio_ready = false;
}

/* ======================================================================
 * Records
 * ====================================================================== */

/* Auth tokens exactly as each core's own nodes write them. */
static const char *wdn_auth_label_(bool biscuit, wifi_auth_mode_t mode)
{
    switch(mode) {
        case WIFI_AUTH_OPEN: return "[OPEN]";
        case WIFI_AUTH_WEP: return "[WEP]";
        case WIFI_AUTH_WPA_PSK: return "[WPA_PSK]";
        case WIFI_AUTH_WPA2_PSK: return "[WPA2_PSK]";
        case WIFI_AUTH_WPA_WPA2_PSK: return "[WPA_WPA2_PSK]";
        case WIFI_AUTH_WPA2_ENTERPRISE: return biscuit ? "[WPA2_ENTERPRISE]" : "[WPA2]";
        case WIFI_AUTH_WPA3_PSK: return "[WPA3_PSK]";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "[WPA2_WPA3_PSK]";
        case WIFI_AUTH_WAPI_PSK: return biscuit ? "[UNDEFINED]" : "[WAPI_PSK]";
        default: return "[UNDEFINED]";
    }
}

static void wdn_forward_(wdn_session_t *s, const wifi_ap_record_t *ap)
{
    char ssid[33];
    char line[96];
    size_t n = 0U;
    int len;
    /* Records are comma separated and end up in a WiGLE CSV: keep commas
     * and control characters out of the SSID, as JCMK nodes do. */
    for(; n < 32U && ap->ssid[n] != 0U; ++n) {
        const char c = (char)ap->ssid[n];
        ssid[n] = (c == ',' || (uint8_t)c < 0x20U || c == 0x7F) ? '_' : c;
    }
    ssid[n] = '\0';
    len = snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%u,%d,W",
                   ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
                   ssid, wdn_auth_label_(s->core == WDN_CORE_BISCUIT, ap->authmode), (unsigned)ap->primary, (int)ap->rssi);
    if(len <= 0) return;
    if(wdn_send_(s, wdn_data_dst_(s), WDN_MSG_TEXT, ++s->text_counter, line,
                 ((size_t)len < sizeof(line)) ? (size_t)len : sizeof(line) - 1U) == ESP_OK) {
        ++s->sent_count;
    }
}

/* Scans one channel and forwards what it found.  Returns false when the
 * driver refuses the channel (not allowed by the regulatory domain). */
static bool wdn_scan_channel_(wdn_session_t *s, uint8_t channel)
{
    wifi_scan_config_t cfg;
    uint16_t found = 0U;
    esp_err_t err;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = channel;
    cfg.show_hidden = true;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    /* Empty channels give up after half the dwell; busy ones get all of it. */
    cfg.scan_time.active.min = (uint32_t)(s->dwell_ms / 2U);
    cfg.scan_time.active.max = s->dwell_ms;
    cfg.scan_time.passive = (s->dwell_ms > WDN_PASSIVE_MS) ? s->dwell_ms : WDN_PASSIVE_MS;
    s->channel = channel;
    err = esp_wifi_scan_start(&cfg, true);
    if(err != ESP_OK) {
        wdn_home_(s);
        return err != ESP_ERR_INVALID_ARG;
    }
    wdn_home_(s);
    if(esp_wifi_scan_get_ap_num(&found) != ESP_OK) found = 0U;
    for(uint16_t i = 0U; i < found && s->running; ++i) {
        wifi_ap_record_t ap;
        if(esp_wifi_scan_get_ap_record(&ap) != ESP_OK) break;
        if(wdn_seen_insert_(s, ap.bssid)) ++s->unique_count;
        if(!wdn_recent_check_and_add_(s, ap.bssid)) wdn_forward_(s, &ap);
    }
    (void)esp_wifi_clear_ap_list();
    return true;
}

/* ======================================================================
 * Node state machine (own task, owns the radio)
 * ====================================================================== */

/* PIGLET NODE introduces itself as a Piglet-family node.  Board type
 * "none" and an empty version keep a Piglet core from ever trying to push
 * Piglet firmware to the POOM.  Other modes send an empty text, as stock
 * JCMK nodes do. */
static size_t wdn_id_text_(const wdn_session_t *s, char out[WDN_PIGLET_ID_LEN])
{
    if(s->kind != WDN_KIND_PIGLET) return 0U;
    memset(out, 0, WDN_PIGLET_ID_LEN);
    memcpy(out, WDN_PIGLET_MARKER, WDN_PIGLET_MARKER_LEN);
    out[WDN_PIGLET_MARKER_LEN] = (char)WDN_PIGLET_BOARD_NONE;
    return WDN_PIGLET_ID_LEN;
}

static void wdn_send_core_request_(wdn_session_t *s)
{
    char id[WDN_PIGLET_ID_LEN];
    const size_t n = wdn_id_text_(s, id);
    wdn_home_(s);
    (void)wdn_send_(s, s_wdn_bcast, WDN_MSG_CORE_REQUEST, 0U, n ? id : NULL, n);
}

static void wdn_send_heartbeat_(wdn_session_t *s)
{
    char id[WDN_PIGLET_ID_LEN];
    const size_t n = wdn_id_text_(s, id);
    wdn_home_(s);
    (void)wdn_send_(s, wdn_data_dst_(s), WDN_MSG_HEARTBEAT, s->hb_counter++, n ? id : NULL, n);
}

static void wdn_handle_event_(wdn_session_t *s, const wdn_evt_t *evt)
{
    if(!s->have_core || memcmp(evt->mac, s->core_mac, 6U) != 0) return;
    switch((wdn_evt_kind_t)evt->kind) {
        case WDN_EVT_JCMK_ADMIN:
            s_wdn_last_jcmk.valid = true;
            memcpy(s_wdn_last_jcmk.core_mac, s->core_mac, 6U);
            s_wdn_last_jcmk.admin = evt->admin;
            if(evt->admin.assignment_version != s->admin_version || !s->assigned) {
                s->assigned = true;
                s->admin_version = evt->admin.assignment_version;
                wdn_plan_jcmk_range_(s, evt->admin.start_channel_idx, evt->admin.end_channel_idx);
                ESP_LOGI(TAG, "JCMK plan v%u: node %u/%u idx %u-%u (%u ch)",
                         (unsigned)evt->admin.assignment_version, (unsigned)evt->admin.node_index + 1U,
                         (unsigned)evt->admin.node_count, (unsigned)evt->admin.start_channel_idx,
                         (unsigned)evt->admin.end_channel_idx, (unsigned)s->plan_len);
            }
            break;
        case WDN_EVT_BISCUIT_ROLE:
            /* A Biscuit node sits in "waiting for role" until its core
             * assigns one; ROLE_WIFI (1) starts Wi-Fi scanning.  Any role
             * without the Wi-Fi bit (0 = none, 2 = BLE only) parks us. */
            wdn_home_(s);
            (void)wdn_send_(s, s->core_mac, WDN_MSG_ROLE_ACK, evt->role, NULL, 0U);
            if(s->core == WDN_CORE_BISCUIT) {
                const bool wifi = (evt->role & 0x01U) != 0U;
                if(wifi != s->scan_enabled) {
                    ESP_LOGI(TAG, "Biscuit role %u: %s", (unsigned)evt->role, wifi ? "scanning" : "waiting");
                }
                s->scan_enabled = wifi;
                s->link = wifi ? WDN_LINK_LINKED : WDN_LINK_WAITING;
            }
            break;
        case WDN_EVT_BISCUIT_CONFIG:
            /* Biscuit and Piglet cores both plan with channel numbers. */
            if(s->core != WDN_CORE_JCMK && wdn_plan_biscuit_(s, evt->text)) {
                s->assigned = true;
                ESP_LOGI(TAG, "Channel plan: %u ch, dwell %u ms", (unsigned)s->plan_len, (unsigned)s->dwell_ms);
            }
            wdn_home_(s);
            (void)wdn_send_(s, s->core_mac, WDN_MSG_CONFIG_ACK, 0U, NULL, 0U);
            break;
        case WDN_EVT_OTHER:
            ESP_LOGI(TAG, "Core msg type %u len %u \"%s\"", (unsigned)evt->type, (unsigned)evt->len, evt->text);
            break;
        default:
            break;
    }
}

/* Listens on the home channel for up to `ms`, handling core messages. */
static void wdn_listen_(wdn_session_t *s, uint32_t ms)
{
    const uint32_t until = wdn_now_ms_() + ms;
    wdn_evt_t evt;
    wdn_home_(s);
    while(s->running) {
        const int32_t left = (int32_t)(until - wdn_now_ms_());
        if(left <= 0) break;
        if(xQueueReceive(s->events, &evt, pdMS_TO_TICKS((uint32_t)left) + 1U) == pdTRUE) wdn_handle_event_(s, &evt);
    }
}

/* Drains queued events without waiting (between channel scans). */
static void wdn_drain_events_(wdn_session_t *s)
{
    wdn_evt_t evt;
    while(s->running && xQueueReceive(s->events, &evt, 0U) == pdTRUE) wdn_handle_event_(s, &evt);
}

static void wdn_link_core_(wdn_session_t *s, const uint8_t mac[6], wdn_core_t core,
                           const wdn_evt_t *early, uint8_t early_count)
{
    const bool biscuit = core == WDN_CORE_BISCUIT;
    char hello[40];
    portENTER_CRITICAL(&s->lock);
    memcpy(s->core_mac, mac, 6U);
    s->have_core = true;
    portEXIT_CRITICAL(&s->lock);
    s->core = core;
    /* JCMK and Piglet nodes scan as soon as they are paired; a Biscuit node
     * waits for the core (and its manager app) to assign it a role. */
    s->scan_enabled = !biscuit;
    s->core_heard_ms = wdn_now_ms_();
    s->admin_version = 0U;
    s->assigned = false;
    s->dwell_ms = WDN_DWELL_DEFAULT_MS;
    wdn_plan_jcmk_range_(s, 0U, (uint8_t)(WDN_JCMK_CHANNEL_COUNT - 1U)); /* until the core assigns */
    if(core == WDN_CORE_JCMK && s_wdn_last_jcmk.valid && memcmp(s_wdn_last_jcmk.core_mac, mac, 6U) == 0) {
        s->admin_version = s_wdn_last_jcmk.admin.assignment_version;
        wdn_plan_jcmk_range_(s, s_wdn_last_jcmk.admin.start_channel_idx, s_wdn_last_jcmk.admin.end_channel_idx);
    }
    wdn_add_peer_(mac);
    s->link = biscuit ? WDN_LINK_WAITING : WDN_LINK_LINKED;
    ESP_LOGI(TAG, "Core %02X:%02X:%02X:%02X:%02X:%02X (%s)", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             s_wdn_core_names[core]);
    if(biscuit) {
        const esp_app_desc_t *app = esp_app_get_description();
        const int n = snprintf(hello, sizeof(hello), "POOM:%s", (app != NULL) ? app->version : "?");
        wdn_home_(s);
        (void)wdn_send_(s, mac, WDN_MSG_HELLO, 0U, hello, (n > 0) ? (size_t)n : 0U);
    }
    /* Core messages that arrived while we were deciding between cores. */
    for(uint8_t i = 0U; i < early_count; ++i) wdn_handle_event_(s, &early[i]);
    /* Let the core finish its handshake (JCMK ADMIN, or role + plan) before
     * the first scan takes the radio off the home channel. */
    wdn_listen_(s, (core == WDN_CORE_JCMK) ? WDN_ADMIN_WIN_MS : WDN_HANDSHAKE_MS);
}

static void wdn_unlink_(wdn_session_t *s)
{
    portENTER_CRITICAL(&s->lock);
    s->have_core = false;
    portEXIT_CRITICAL(&s->lock);
    if(esp_now_is_peer_exist(s->core_mac) && memcmp(s->core_mac, s_wdn_bcast, 6U) != 0) {
        (void)esp_now_del_peer(s->core_mac);
    }
    s->core = WDN_CORE_JCMK;
    s->scan_enabled = false;
    s->link = WDN_LINK_SEARCHING;
}

/* Searches until a core answers.  Each mode prefers its own core kind, but
 * falls back to whichever core answered if its preferred kind stays quiet. */
static void wdn_search_(wdn_session_t *s)
{
    uint32_t interval = WDN_REQ_INIT_MS;
    uint32_t next_req = wdn_now_ms_();
    uint32_t fallback_until = 0U;
    bool have_fallback = false;
    uint8_t fallback_mac[6] = {0};
    wdn_core_t fallback_core = WDN_CORE_JCMK;
    wdn_evt_t early[2];
    uint8_t early_count = 0U;
    s->link = WDN_LINK_SEARCHING;
    wdn_home_(s);
    while(s->running) {
        wdn_evt_t evt;
        const uint32_t now = wdn_now_ms_();
        if((int32_t)(now - next_req) >= 0) {
            wdn_send_core_request_(s);
            next_req = now + interval;
            interval = (interval * 2U > WDN_REQ_MAX_MS) ? WDN_REQ_MAX_MS : interval * 2U;
        }
        if(have_fallback && (int32_t)(now - fallback_until) >= 0) {
            wdn_link_core_(s, fallback_mac, fallback_core, early, early_count);
            return;
        }
        if(xQueueReceive(s->events, &evt, pdMS_TO_TICKS(50U)) != pdTRUE) continue;
        if(evt.kind != WDN_EVT_REPLY) {
            /* A JCMK core sends ADMIN right behind its CORE_REPLY; keep it
             * for the fallback core instead of losing the assignment. */
            if(have_fallback && early_count < 2U && memcmp(evt.mac, fallback_mac, 6U) == 0) early[early_count++] = evt;
            continue;
        }
        {
            static const wdn_core_t prefer[WDN_KIND_COUNT] = {WDN_CORE_BISCUIT, WDN_CORE_JCMK, WDN_CORE_PIGLET};
            if((wdn_core_t)evt.core == prefer[s->kind]) {
                wdn_link_core_(s, evt.mac, (wdn_core_t)evt.core, NULL, 0U);
                return;
            }
            if(!have_fallback) {
                have_fallback = true;
                memcpy(fallback_mac, evt.mac, 6U);
                fallback_core = (wdn_core_t)evt.core;
                fallback_until = now + WDN_PREFER_WAIT_MS;
            }
        }
    }
}

static void wdn_node_task_(void *arg)
{
    wdn_session_t *s = (wdn_session_t *)arg;
    /* Spread out nodes that power up together so their requests interleave. */
    vTaskDelay(pdMS_TO_TICKS(esp_random() % 400U));
    while(s->running) {
        if(!s->have_core) {
            wdn_search_(s);
            continue;
        }

        if(s->scan_enabled) {
            /* One full pass over the assigned channels. */
            for(uint8_t i = 0U; i < s->plan_len && s->running && s->have_core && s->scan_enabled; ++i) {
                const uint8_t ch = s->plan[s->plan_pos];
                s->plan_pos = (uint8_t)((s->plan_pos + 1U) % s->plan_len);
                if(wdn_channel_bad_(s, ch)) continue;
                if(!wdn_scan_channel_(s, ch)) {
                    ESP_LOGW(TAG, "Channel %u not allowed here, skipping", (unsigned)ch);
                    wdn_channel_mark_bad_(s, ch);
                }
                wdn_drain_events_(s);
            }
        }
        if(!s->running) break;

        /* Check in with the core and give it a window to reassign us.  While
         * a Biscuit core has not started us, stay on the home channel. */
        {
            const uint32_t quiet = wdn_now_ms_() - s->core_heard_ms;
            wdn_send_heartbeat_(s);
            if(quiet >= WDN_LOST_AFTER_MS) {
                ESP_LOGW(TAG, "Core silent for %lus, searching again", (unsigned long)(quiet / 1000U));
                wdn_unlink_(s);
                continue;
            }
            if(quiet >= WDN_PROBE_AFTER_MS) {
                /* JCMK cores stay silent when nothing changes; a CORE_REQUEST
                 * always earns a CORE_REPLY, which proves they are still there. */
                s->link = WDN_LINK_QUIET;
                wdn_send_core_request_(s);
            }
            wdn_listen_(s, (s->scan_enabled ? WDN_ADMIN_WIN_MS : WDN_IDLE_WIN_MS) + (esp_random() % 64U));
            if((wdn_now_ms_() - s->core_heard_ms) < WDN_PROBE_AFTER_MS) {
                s->link = s->scan_enabled ? WDN_LINK_LINKED : WDN_LINK_WAITING;
            }
        }
    }
    s->node_alive = false;
    vTaskDelete(NULL);
}

/* ======================================================================
 * Screens
 * ====================================================================== */

static void wdn_draw_pick_(const wdn_session_t *s)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(19, 2);
    (void)poom_arduboy_print(F("WARDRIVING NODE"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    for(uint8_t i = 0U; i < (uint8_t)WDN_KIND_COUNT; ++i) {
        const int16_t y = (int16_t)(14 + (i * 10));
        poom_arduboy_set_cursor(6, y);
        (void)poom_arduboy_print(s_wdn_titles[i]);
        if(i == s->pick) poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
    }
    {
        const char *hint = s_wdn_hints[s->pick];
        const int16_t w = poom_arduboy_tiny_text_width(hint);
        poom_arduboy_draw_tiny_text((int16_t)((ARDUBOY_WIDTH - w) / 2), 46, hint, WHITE);
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:START"));
    poom_arduboy_set_cursor(88, 56);
    (void)poom_arduboy_print(F("B:EXIT"));
    poom_arduboy_display();
}

static void wdn_format_count_(char *out, size_t size, uint32_t value)
{
    if(value < 100000U) (void)snprintf(out, size, "%lu", (unsigned long)value);
    else if(value < 10000000U) (void)snprintf(out, size, "%luK", (unsigned long)(value / 1000U));
    else (void)snprintf(out, size, "%luM", (unsigned long)(value / 1000000U));
}

/* Draws one pig frame; clear bits are transparent.  `left` mirrors it. */
static void wdn_draw_pig_frame_(int16_t x, int16_t y, uint8_t frame, bool left)
{
    const uint8_t *rows = s_wardrive_node_pig[frame];
    for(int16_t yy = 0; yy < WARDRIVE_NODE_PIG_H; ++yy) {
        for(int16_t xx = 0; xx < WARDRIVE_NODE_PIG_W; ++xx) {
            const uint8_t b = rows[(yy * (WARDRIVE_NODE_PIG_W / 8)) + (xx / 8)];
            if((b >> (7 - (xx & 7))) & 1U) {
                poom_arduboy_draw_pixel((int16_t)(x + (left ? (WARDRIVE_NODE_PIG_W - 1 - xx) : xx)),
                                        (int16_t)(y + yy), WHITE);
            }
        }
    }
}

/* PIGLET NODE stage (left 64x64): the pig walks the floor, dances when a
 * core pairs and every so often, and twerks when new networks come in. */
static void wdn_draw_pig_(wdn_session_t *s)
{
    const uint32_t now = wdn_now_ms_();
    const int16_t floor_y = 58;
    const int16_t max_x = 64 - WARDRIVE_NODE_PIG_W;
    const uint32_t count = s->unique_count;
    const wdn_link_t link = s->link;
    int16_t y = (int16_t)(floor_y - WARDRIVE_NODE_PIG_H);
    uint8_t frame;

    ++s->pig.tick;
    if(s->pig.mode != WARDRIVE_NODE_PIG_WALK && (int32_t)(now - s->pig.until_ms) >= 0) {
        s->pig.mode = WARDRIVE_NODE_PIG_WALK;
        s->pig.next_dance_ms = now + 20000U;
        s->pig.next_twerk_ms = now + 4000U;
    }
    if(link == WDN_LINK_LINKED && s->pig.last_link != WDN_LINK_LINKED) {
        s->pig.mode = WARDRIVE_NODE_PIG_DANCE;     /* paired */
        s->pig.until_ms = now + 3000U;
    } else if(count > s->pig.last_count && s->pig.mode == WARDRIVE_NODE_PIG_WALK &&
              (int32_t)(now - s->pig.next_twerk_ms) >= 0) {
        s->pig.mode = WARDRIVE_NODE_PIG_TWERK;     /* new networks */
        s->pig.until_ms = now + 1500U;
    } else if(s->pig.mode == WARDRIVE_NODE_PIG_WALK && link == WDN_LINK_LINKED &&
              (int32_t)(now - s->pig.next_dance_ms) >= 0) {
        s->pig.mode = WARDRIVE_NODE_PIG_DANCE;
        s->pig.until_ms = now + 2500U;
    }
    s->pig.last_link = link;
    if(s->pig.mode == WARDRIVE_NODE_PIG_WALK) s->pig.last_count = count;

    switch(s->pig.mode) {
        case WARDRIVE_NODE_PIG_TWERK:
            frame = (uint8_t)(WARDRIVE_NODE_PIG_TWERK + (s->pig.tick & 1U));
            poom_arduboy_draw_tiny_text((int16_t)(s->pig.left ? s->pig.x + 2 : s->pig.x + 12), 26, "OINK", WHITE);
            break;
        case WARDRIVE_NODE_PIG_DANCE:
            /* Hop on the beat and turn around every other hop. */
            frame = (uint8_t)(WARDRIVE_NODE_PIG_DANCE + ((s->pig.tick >> 1) & 1U));
            if(((s->pig.tick >> 1) & 1U) != 0U) y = (int16_t)(y - 3);
            if((s->pig.tick & 7U) == 0U) s->pig.left = !s->pig.left;
            {
                /* a little note bouncing over the pig */
                const int16_t nx = (int16_t)(s->pig.x + ((s->pig.tick & 4U) ? 4 : 24));
                const int16_t ny = (int16_t)(y - 8 - ((s->pig.tick >> 1) & 1U));
                poom_arduboy_fill_rect(nx, (int16_t)(ny + 4), 2, 2, WHITE);
                poom_arduboy_draw_fast_vline((int16_t)(nx + 1), ny, 5, WHITE);
                poom_arduboy_draw_pixel((int16_t)(nx + 2), ny, WHITE);
                poom_arduboy_draw_pixel((int16_t)(nx + 3), (int16_t)(ny + 1), WHITE);
            }
            break;
        default:
            /* Trot while scanning, amble while searching, stand when stuck. */
            frame = (uint8_t)(WARDRIVE_NODE_PIG_WALK + (s->pig.tick & 1U));
            if(link == WDN_LINK_RADIO_ERROR || link == WDN_LINK_WAITING) {
                frame = WARDRIVE_NODE_PIG_WALK + 1U;
            } else if(link == WDN_LINK_LINKED || (s->pig.tick & 1U) == 0U) {
                s->pig.x = (int16_t)(s->pig.x + (s->pig.left ? -1 : 1));
                if(s->pig.x <= 0) { s->pig.x = 0; s->pig.left = false; }
                if(s->pig.x >= max_x) { s->pig.x = max_x; s->pig.left = true; }
            }
            break;
    }
    wdn_draw_pig_frame_(s->pig.x, y, frame, s->pig.left);
    poom_arduboy_draw_fast_hline(0, floor_y, 64, WHITE);
    {
        static const char title[] = "PIGLET";
        poom_arduboy_set_cursor((int16_t)((64 - 36) / 2), 4);
        (void)poom_arduboy_print(title);
    }
}

static void wdn_draw_run_(wdn_session_t *s)
{
    const int16_t px = 66;      /* right panel origin */
    const int16_t pw = ARDUBOY_WIDTH - px;
    const bool blink = ((esp_timer_get_time() / 400000LL) & 1LL) == 0LL;
    const wdn_link_t link = s->link;
    char count[12];
    char line[24];
    int16_t w;

    poom_arduboy_clear();
    if(s->kind == WDN_KIND_PIGLET) wdn_draw_pig_(s);
    else poom_arduboy_draw_bitmap_rows(0, 0, (s->kind == WDN_KIND_BISCUIT) ? s_wardrive_node_logo_biscuit :
                                       s_wardrive_node_logo_jcmk, WARDRIVE_NODE_LOGO_W, WARDRIVE_NODE_LOGO_H, false);

    w = poom_arduboy_tiny_text_width(s_wdn_titles[s->kind]);
    poom_arduboy_draw_tiny_text((int16_t)(px + (pw - w) / 2), 1, s_wdn_titles[s->kind], WHITE);
    poom_arduboy_draw_fast_hline(px, 8, pw, WHITE);

    wdn_format_count_(count, sizeof(count), s->unique_count);
    poom_arduboy_set_text_size(2);
    w = (int16_t)(strlen(count) * 12U);
    poom_arduboy_set_cursor((int16_t)(px + (pw - w) / 2), 12);
    (void)poom_arduboy_print(count);
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor((int16_t)(px + (pw - 48) / 2), 30);
    (void)poom_arduboy_print(F("NETWORKS"));

    switch(link) {
        case WDN_LINK_LINKED: (void)snprintf(line, sizeof(line), "LINKED"); break;
        case WDN_LINK_WAITING: (void)snprintf(line, sizeof(line), "READY"); break;
        case WDN_LINK_QUIET: (void)snprintf(line, sizeof(line), "NO REPLY"); break;
        case WDN_LINK_RADIO_ERROR: (void)snprintf(line, sizeof(line), "RADIO ERR"); break;
        default: (void)snprintf(line, sizeof(line), blink ? "SEARCH.." : "SEARCH"); break;
    }
    w = (int16_t)(strlen(line) * 6U);
    poom_arduboy_set_cursor((int16_t)(px + (pw - w) / 2), 41);
    (void)poom_arduboy_print(line);
    if(link == WDN_LINK_LINKED && blink) poom_arduboy_fill_rect((int16_t)(px + 1), 43, 3, 3, WHITE);

    if(link == WDN_LINK_SEARCHING || link == WDN_LINK_RADIO_ERROR) (void)snprintf(line, sizeof(line), "HOME CH %u", (unsigned)WDN_HOME_CHANNEL);
    else if(link == WDN_LINK_WAITING) (void)snprintf(line, sizeof(line), "START IN APP");
    else (void)snprintf(line, sizeof(line), "CH %u TX %lu", (unsigned)s->channel, (unsigned long)s->sent_count);
    w = poom_arduboy_tiny_text_width(line);
    poom_arduboy_draw_tiny_text((int16_t)(px + (pw - w) / 2), 51, line, WHITE);
    w = poom_arduboy_tiny_text_width("B:STOP");
    poom_arduboy_draw_tiny_text((int16_t)(px + (pw - w) / 2), 58, "B:STOP", WHITE);
    poom_arduboy_display();
}

/* ======================================================================
 * Lifecycle
 * ====================================================================== */

static void wdn_stop_node_(wdn_session_t *s)
{
    const uint32_t start = wdn_now_ms_();
    if(s->node_task == NULL) {
        wdn_radio_stop_(s);
        return;
    }
    s->running = false;
    /* The node task leaves at the end of its current scan (< 0.5 s). */
    while(s->node_alive && (wdn_now_ms_() - start) < WDN_STOP_WAIT_MS) vTaskDelay(pdMS_TO_TICKS(20U));
    if(s->node_alive) {
        vTaskDelete(s->node_task);
        s->node_alive = false;
    }
    s->node_task = NULL;
    wdn_unlink_(s);
    wdn_radio_stop_(s);
    xQueueReset(s->events);
}

static void wdn_start_node_(wdn_session_t *s, wdn_kind_t kind)
{
    s->kind = kind;
    s->view = WDN_VIEW_RUN;
    s->link = WDN_LINK_SEARCHING;
    s->unique_count = 0U;
    s->sent_count = 0U;
    s->text_counter = 0U;
    s->hb_counter = 0U;
    s->plan_len = 0U;
    s->plan_pos = 0U;
    s->dwell_ms = WDN_DWELL_DEFAULT_MS;
    s->recent_pos = 0U;
    memset(s->recent, 0, sizeof(s->recent));
    if(s->seen != NULL) memset(s->seen, 0, (size_t)s->seen_cap * sizeof(uint64_t));
    s->seen_used = 0U;
    memset(&s->pig, 0, sizeof(s->pig));
    s->pig.x = (int16_t)((64 - WARDRIVE_NODE_PIG_W) / 2);
    s->pig.next_dance_ms = wdn_now_ms_() + 20000U;
    xQueueReset(s->events);
    wdn_draw_run_(s);
    if(!wdn_radio_start_(s)) {
        ESP_LOGE(TAG, "Wi-Fi / ESP-NOW start failed");
        s->link = WDN_LINK_RADIO_ERROR;
        return;
    }
    s->running = true;
    s->node_alive = true;
    if(xTaskCreate(wdn_node_task_, "wardrive_node", WDN_NODE_STACK, s, WDN_NODE_PRIO, &s->node_task) != pdPASS) {
        s->running = false;
        s->node_alive = false;
        s->node_task = NULL;
        wdn_radio_stop_(s);
        s->link = WDN_LINK_RADIO_ERROR;
    }
}

static void wdn_buttons_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t event;
    wdn_button_t button;
    (void)user;
    if(s_wdn == NULL || msg == NULL || msg->len < sizeof(event) || s_wdn->buttons == NULL) return;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    button.button = event.button;
    (void)xQueueSend(s_wdn->buttons, &button, 0U);
}

static void wdn_exit_(void)
{
    wdn_session_t *s = s_wdn;
    if(s == NULL) return;
    wdn_stop_node_(s);
    if(s->subscribed) (void)poom_sbus_unsubscribe_cb("input/button", wdn_buttons_, (void *)s_wdn_user);
    s_wdn = NULL;
    /* Let an in-flight receive callback that already read s_wdn finish. */
    vTaskDelay(pdMS_TO_TICKS(20U));
    if(s->buttons != NULL) vQueueDelete(s->buttons);
    if(s->events != NULL) vQueueDelete(s->events);
    free(s->seen);
    free(s);
    { const uint8_t token = 1U; (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U); }
}

static void wdn_ui_task_(void *arg)
{
    (void)arg;
    while(s_wdn != NULL) {
        wdn_session_t *s = s_wdn;
        wdn_button_t button;
        if(xQueueReceive(s->buttons, &button, pdMS_TO_TICKS(WDN_UI_TICK_MS)) == pdTRUE) {
            if(s->view == WDN_VIEW_PICK) {
                if(button.button == BTN_B) {
                    wdn_exit_();
                    break;
                }
                if(button.button == BTN_DOWN) {
                    s->pick = (uint8_t)((s->pick + 1U) % (uint8_t)WDN_KIND_COUNT);
                } else if(button.button == BTN_UP) {
                    s->pick = (uint8_t)((s->pick + (uint8_t)WDN_KIND_COUNT - 1U) % (uint8_t)WDN_KIND_COUNT);
                } else if(button.button == BTN_A) {
                    s_wdn_last_pick = s->pick;
                    wdn_start_node_(s, (wdn_kind_t)s->pick);
                }
            } else if(button.button == BTN_B) {
                wdn_stop_node_(s);
                s->view = WDN_VIEW_PICK;
            }
        }
        if(s->view == WDN_VIEW_PICK) wdn_draw_pick_(s);
        else wdn_draw_run_(s);
    }
    vTaskDelete(NULL);
}

void menu_wardrive_node_show(void)
{
    wdn_session_t *s;
    wdn_exit_();
    s = heap_caps_calloc(1U, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s == NULL) s = calloc(1U, sizeof(*s));
    if(s == NULL) { const uint8_t token = 1U; (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U); return; }
    s->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s->pick = (s_wdn_last_pick < (uint8_t)WDN_KIND_COUNT) ? s_wdn_last_pick : 0U;
    s->seen_cap = WDN_SEEN_CAP_PSRAM;
    s->seen = heap_caps_calloc(s->seen_cap, sizeof(uint64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s->seen == NULL) {
        s->seen_cap = WDN_SEEN_CAP_IRAM;
        s->seen = calloc(s->seen_cap, sizeof(uint64_t));
        if(s->seen == NULL) s->seen_cap = 0U;
    }
    s->buttons = xQueueCreate(8U, sizeof(wdn_button_t));
    s->events = xQueueCreate(6U, sizeof(wdn_evt_t));
    s_wdn = s;
    if(s->buttons == NULL || s->events == NULL ||
       !poom_sbus_subscribe_cb("input/button", wdn_buttons_, (void *)s_wdn_user)) {
        wdn_exit_();
        return;
    }
    s->subscribed = true;
    wdn_draw_pick_(s);
    if(xTaskCreate(wdn_ui_task_, "menu_wardrive_node", WDN_UI_STACK, NULL, WDN_UI_PRIO, &s->ui_task) != pdPASS) {
        wdn_exit_();
    }
}
