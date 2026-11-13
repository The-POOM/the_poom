// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Security Audit -- an Nmap-style LAN scanner for the POOM.
//
// Flow:
//   WIFI gate -> (saved profiles | scan networks -> password) -> connect
//     -> HOSTS (Full NetAudit / Ping Sweep / Scan All / Scan IP + device list)
//       -> HOST DETAIL -> port method -> port profile (Quick/Extended/Full/
//          Custom) -> SCANNING -> RESULTS
//       -> Scan All -> SCANNING across hosts -> NET SUMMARY
//          -> L: Assess All (one CSV pass over every unique product) -> NET VULN
//             SUMMARY (Potential/No match/Unassessed hosts) -> host list -> the
//             per-device Assess screens (the host's results projected in)
//       -> Full NetAudit: range prompt -> confirm/estimate -> Discover -> Scan
//          All Ports+Svc -> Assess All -> NET VULN SUMMARY (B cancels a phase
//          and shows what was gathered so far)
//
// Reuses existing firmware infrastructure only: poom_wifi_ctrl (STA connect),
// poom_wifi_profile_store (the shared 12-slot saved-Wi-Fi store -- no duplicate
// credential storage), poom_wifi_scanner (AP list), poom_ui_keyboard (OSK) and
// the poom_security_audit engine. Wi-Fi Detect is never touched.

#include "menu_security_audit.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"

#include "menu_wifi_detector_oui.h"
#include "poom_net_audit.h"
#include "poom_security_audit.h"
#include "poom_sbus.h"
#include "poom_ui_keyboard.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_profile_store.h"
#include "poom_wifi_scanner.h"
#include "sd_card.h"

/* poom_menu is not in this component's REQUIRES; sibling menus define the
 * resume topic locally the same way. */
#ifndef POOM_MENU_RESUME_TOPIC
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#endif

/* Managed-button long-press event codes (same values the sibling menus use as
 * a fallback when the component headers are not on the include path). */
#ifndef BUTTON_LONG_PRESS_START
#define BUTTON_LONG_PRESS_START (6U)
#endif
#ifndef BUTTON_LONG_PRESS_HOLD
#define BUTTON_LONG_PRESS_HOLD (7U)
#endif
#ifndef BUTTON_LONG_PRESS_UP
#define BUTTON_LONG_PRESS_UP (8U)
#endif

#define SA_TASK_STACK        (6144)  /* measured peak ~2.4 KB; ~3.7 KB margin */
#define SA_TASK_PRIO         (5)
#define SA_REFRESH_MS        (200)
#define SA_CONNECT_TIMEOUT_MS (15000)
#define SA_PROGRESS_MIN_MS   (120)   /* min gap between progress redraws */

#define SA_RESULT_CAP        (32U)   /* OPEN ports kept per single-host scan */
#define SA_PORT_CHUNK        (256U)  /* ports materialised per scan window   */
#define SA_HOST_PORTS        (12U)   /* open ports kept per host in Scan All */
#define SA_VISIBLE_ROWS      (4U)    /* list rows visible at once           */

/* UI-side device cap. Deliberately smaller than the engine's hard ceiling so
 * the per-session struct stays a modest, reliably-allocatable block (a large
 * contiguous calloc can fail on a fragmented heap after a run, which would
 * otherwise leave the menu frozen on re-entry). 64 devices is ample for a
 * handheld LAN scan UI. */
#define SA_MAX_HOSTS         (64U)

/* Assess All: unique (product, version) pairs across the network, and the
 * bounded match set from the one-pass CSV search (heap, allocated on demand). */
#define SA_NET_TERMS         (32U)
#define SA_NET_MATCH_CAP     (48U)
#define SA_NET_PER_TERM      (8U)

/* Full NetAudit estimate assumptions (shown as an upper bound). Service
 * detect is ~1.5 s connect + up to 3 s read per open port; assume ~2 open
 * ports per host. The CSV pass is SDSPI-bound (~3-10 s measured per pass on
 * the direct scan); allow 30 s. */
#define SA_EST_SVC_S_PER_HOST (9U)
#define SA_EST_ASSESS_S       (30U)

/* Fixed action rows at the top of the HOSTS screen. */
enum
{
    SA_HOST_ACT_AUDIT = 0, /* Full NetAudit (one-tap wrapper) */
    SA_HOST_ACT_SWEEP,     /* Ping Sweep / Rescan */
    SA_HOST_ACT_ALL,       /* Scan All Devices    */
    SA_HOST_ACT_IP,        /* Scan Specific IP    */
    SA_HOST_ACT_COUNT
};

typedef enum
{
    SA_SCR_WIFI = 0,      /* Wi-Fi gate (connected vs. not)        */
    SA_SCR_PROFILES,      /* saved profile list                    */
    SA_SCR_SCAN_APS,      /* scan networks list                    */
    SA_SCR_PASSWORD,      /* OSK: password                         */
    SA_SCR_CONNECTING,    /* waiting for DHCP/IP                    */
    SA_SCR_HOSTS,         /* device selection + discovered list    */
    SA_SCR_SWEEPING,      /* discovery in progress                 */
    SA_SCR_HOST_DETAIL,   /* one host's details + Port Scan        */
    SA_SCR_SPECIFIC_IP,   /* OSK: specific IP                      */
    SA_SCR_PORT_METHOD,   /* TCP Connect / TCP SYN                 */
    SA_SCR_PORT_PROFILE,  /* Quick / Extended / Full / Custom      */
    SA_SCR_PORT_CUSTOM,   /* OSK: custom ports                     */
    SA_SCR_SCANNING,      /* port scan in progress                 */
    SA_SCR_RESULTS,       /* single-host results                   */
    SA_SCR_NET_SUMMARY,   /* network-wide summary (Scan All)       */
    SA_SCR_HOST_OPEN,     /* one host's open ports (from Scan All) */
    SA_SCR_PORT_DETAIL,   /* one port's full record                */
    SA_SCR_VULN_SUMMARY,  /* Assess result: Potential/No match/Unassessed */
    SA_SCR_VULN_POTENTIAL,/* chooser: By exploit / By service       */
    SA_SCR_VULN_SERVICES, /* by-service list                        */
    SA_SCR_VULN_GAPS,     /* no-match / unassessed list             */
    SA_SCR_SS_RESULTS,    /* By-exploit list (CVE-grouped, tiered)  */
    SA_SCR_SS_DETAIL,     /* one exploit match                      */
    SA_SCR_NET_VULN,      /* Assess All: Potential/No match/Unassessed hosts */
    SA_SCR_NET_VULN_HOSTS,/* hosts in one network category           */
    SA_SCR_AUDIT_CONFIRM, /* Full NetAudit: range + estimate, A to start */
    SA_SCR_VULN_EXPOSURES,/* exposure findings of one host (text page)  */
    SA_SCR_DEVICE_INFO,   /* what a host is: identity + open services (page) */
    SA_SCR_SAVE_CONFIRM,  /* offer to save a freshly-entered network */
    SA_SCR_INFO,          /* transient message                     */
} sa_screen_t;

/* The session is split into a small core plus three side blocks, each its
 * own allocation, so (re-)entering the app never needs one ~20 KB contiguous
 * block on a fragmented heap - the largest piece is ~6 KB. */

/* Port-scan working buffers. */
typedef struct
{
    uint16_t                       ports[SA_PORT_CHUNK];
    poom_security_audit_port_result_t results[SA_RESULT_CAP];
} sa_scanbuf_t;

/* Per-device Assess (also the projection target for an Assess-All host). */
typedef struct
{
    /* Per-open-port services detected for the current single-device scan. */
    poom_security_audit_service_t svcs[16];
    size_t                     svc_count;

    /* Exploit-DB matches across the device's services. */
    poom_net_audit_match_t     ss[24];          /* collected matches          */
    size_t                     ss_count;
    poom_net_audit_term_t      ss_terms[16];    /* one per searchable service */
    uint8_t                    ss_term_svc[16]; /* term -> svcs[] index        */
    uint8_t                    ss_n_terms;
    /* Display list for the By-exploit view (CVE-grouped, tier/service filtered). */
    struct { uint8_t idx; uint8_t count; } ss_disp[24];
    uint8_t                    ss_disp_count;
    /* Assess summary counts. */
    uint8_t                    vuln_potential;   /* services with >=1 match    */
    uint8_t                    vuln_nomatch;     /* product services, 0 hits   */
    uint8_t                    vuln_unassessed;  /* open ports, no product     */
} sa_assess_t;

/* Scan-All / Assess-All network results. */
typedef struct
{
    /* Per-host open-port lists captured during Scan All, so the network
     * summary can drill into each host (index-aligned with hosts[]). */
    struct
    {
        uint16_t ports[SA_HOST_PORTS];
        uint8_t  svc_id[SA_HOST_PORTS]; /* compact service id per port (0 = unknown) */
        uint8_t  prod[SA_HOST_PORTS];   /* interned product id per port (0 = none)   */
        uint8_t  count;
        bool     overflow;
    } host_open[SA_MAX_HOSTS];

    /* Assess All: unique products seen during Scan All (host_open[].prod ids
     * index this, 1-based) and per-term hit counts after the CSV pass. */
    poom_net_audit_prodver_t net_prod[SA_NET_TERMS];
    uint8_t                  net_prod_count;
    uint8_t                  net_hits[SA_NET_TERMS];
    uint8_t                  net_cls_count[3];  /* hosts: potential/nomatch/unassessed */

    /* Device product from a UPnP description (e.g. "R7000" fw "V1.0.11"),
     * searched like one more service on that host. 0 = none. */
    uint8_t                  dev_prod[SA_MAX_HOSTS];
    uint16_t                 dev_port[SA_MAX_HOSTS];

    /* Exposure findings per host (poom_net_audit_exp_t bit mask). */
    uint32_t                 exposures[SA_MAX_HOSTS];
    uint8_t                  net_exp_hosts;  /* hosts with >= 1 finding (incl. confirmed) */

    /* Active read-only web checks: hit mask per host (bit = check index) and
     * the web port they were seen on. */
    uint16_t                 confirmed[SA_MAX_HOSTS];
    uint16_t                 web_port[SA_MAX_HOSTS];
} sa_netbuf_t;

typedef struct
{
    /* Wi-Fi / credentials (shared store). */
    poom_wifi_profile_t *profiles;    /* loaded only while needed (1.2 KB) */
    uint16_t            profile_count;
    char                edit_ssid[POOM_WIFI_PROFILE_SSID_CAP];
    char                edit_pass[POOM_WIFI_PROFILE_PASS_CAP];
    bool                offer_save;   /* password came from scan, not store */

    /* On-screen keyboard (reused for password / IP / ports). */
    poom_ui_keyboard_t  kb;
    char                kb_text[72];

    /* Discovery. */
    poom_security_audit_subnet_t subnet;
    poom_security_audit_host_t   hosts[SA_MAX_HOSTS];
    size_t                    host_count;
    poom_security_audit_host_t   detail_host;  /* host shown in HOST_DETAIL */
    poom_security_audit_ident_t  detail_id;    /* identity of a Manual Target */
    uint16_t                  detail_conf;  /* web check hits of the single-device scan */
    uint16_t                  detail_web_port;
    poom_security_audit_service_t detail_svc;  /* last service-detect result */
    bool                       detail_svc_valid;
    bool                       auto_swept;  /* first-arrival auto Ping Sweep done */
    bool                       net_results; /* Scan All data in nb (aligned with hosts[]) */

    /* Port scan. */
    size_t                         result_count;
    size_t                         open_count;   /* current target open ports */
    uint32_t                       n_closed;     /* counted, not stored (RAM) */
    uint32_t                       n_filtered;
    char                           target_ip[POOM_SECURITY_AUDIT_IP_STR_CAP];

    /* Scan-All accumulation. */
    size_t   all_idx;          /* host index while scanning all      */
    uint32_t all_hosts_up;     /* hosts with >=1 open port           */
    uint32_t all_open_total;   /* open ports across all hosts        */

    /* Side blocks (separate allocations, see above). */
    sa_scanbuf_t *sb;
    sa_assess_t  *as;
    sa_netbuf_t  *nb;
    poom_security_audit_ident_t *ids; /* per-host name/OS, index-aligned with hosts[] (own block) */
} sa_session_t;

/* ---- module state ------------------------------------------------------- */

static sa_session_t *s_ses = NULL;
static TaskHandle_t   s_task = NULL;
static bool           s_active = false;
static bool           s_buttons_subscribed = false;

static volatile bool s_exit_requested = false; /* leave the app          */
static volatile bool s_cancel = false;         /* cancel engine op       */

static volatile bool s_connect_pending = false;
static volatile bool s_relink_pending = false;  /* fresh session, then rejoin */
static volatile bool s_relinking = false;       /* session being rebuilt: ignore buttons */

/* What to carry on with once a rejoin (after a DB pass) has connected. */
enum { SA_RESUME_NONE = 0, SA_RESUME_HOSTS_ROW, SA_RESUME_PORTSCAN, SA_RESUME_DETECT };
static uint8_t s_resume = SA_RESUME_NONE;
static int     s_resume_row = 0;
static bool    s_relink_keep = false;      /* light rejoin in progress: keep the list */
static bool    s_relink_full_tried = false;
static volatile bool s_sweep_pending = false;
static volatile bool s_scan_aps_pending = false;
static volatile bool s_portscan_pending = false;
static volatile bool s_scanall_pending = false;
static volatile bool s_resolve_ip_pending = false;
static volatile bool s_detect_pending = false;  /* run service detect in task */
static volatile bool s_detecting = false;        /* detect in progress (draw)  */
static volatile bool s_aps_scanning = false;     /* AP scan in progress (draw) */
static volatile bool s_svc_phase = false;        /* identifying services (draw) */
static volatile bool s_assess_pending = false;   /* run Assess in task          */
static volatile bool s_ss_searching = false;     /* Exploit-DB scan (draw)      */
static bool s_idx_building = false;              /* this pass also builds the index */
static int   s_ss_service_filter = -1;           /* term_id, or -1 = all        */
static uint8_t s_ss_tier_filter = 0;             /* 0 ALL,1 STRONG,2 MED,3 WEAK */
static uint8_t s_gaps_mode = 0;                  /* 0 no-match, 1 unassessed    */

/* Assess All (network-wide) state. The match buffer is heap-allocated only
 * when Assess All runs (keeps the session block small) and freed on exit. */
static poom_net_audit_match_t *s_net_ss = NULL;
static size_t  s_net_ss_count = 0;
static bool    s_net_assessed = false;           /* net results valid           */
static bool    s_net_partial = false;            /* last Assess All cancelled   */
static bool    s_assess_net = false;             /* per-device screens show a host projection */
static bool    s_net_direct = false;             /* opened straight from a NET VULN category */
static int     s_proj_host = -1;                 /* hosts[] index shown by the per-host views, -1 = single-device scan */
static volatile bool s_assess_all_pending = false;
static uint8_t s_net_cat = 0;                    /* 0 potential,1 no match,2 unassessed */
static int     s_net_host_sel = 0;               /* remembered row in NET_VULN_HOSTS */

/* Full NetAudit (Discover -> Scan All Ports+Svc -> Assess All). */
static bool    s_audit_flow = false;             /* range/confirm screens belong to NetAudit */
static uint8_t s_audit_profile = 0;              /* 0 Quick,1 Extended,2 Full,3 Custom */
static uint32_t s_audit_ports_n = 0;             /* ports per host for the estimate */
static uint32_t s_audit_custom_n = 0;            /* parsed custom count (ports[] holds them) */
static volatile bool s_audit_pending = false;
static volatile bool s_audit_running = false;    /* pipeline in progress (B = cancel only) */
static volatile bool s_audit_cancel = false;     /* sticky across phases */

static sa_screen_t s_screen = SA_SCR_WIFI;
static sa_screen_t s_info_return = SA_SCR_WIFI;
static char        s_info[22] = {0};

static int  s_sel = 0;              /* generic list selection            */
static int  s_list_top = 0;         /* list scroll window top            */
static int  s_result_top = 0;       /* results scroll                    */
static int  s_host_sel = 0;         /* remembered Devices-list row        */
static int  s_summary_sel = 0;      /* remembered Net-Summary row          */
static int  s_ss_sel = 0;           /* selected SearchSploit match          */
static bool s_services_on = true;   /* identify services after a port scan */
static uint8_t s_method = 0;        /* 0=Connect, 1=SYN                  */

static int64_t s_connect_start_ms = 0;
static char    s_ssid[33] = {0};

static uint32_t s_prog_done = 0;
static uint32_t s_prog_total = 0;
static int64_t  s_prog_last_draw_ms = 0;

static volatile bool s_scanall_active = false; /* in the Scan-All loop      */
static char s_scan_ip[POOM_SECURITY_AUDIT_IP_STR_CAP] = {0}; /* host being scanned */
static int  s_open_host = 0;                   /* host index shown in HOST_OPEN */
static int  s_detail_hi = -1;                  /* hosts[] index of HOST_DETAIL, -1 = manual */
static volatile bool s_id_phase = false;       /* identify pass running (draw) */
static uint8_t s_cur_os_class = 0;             /* OS class of the host whose exploits are shown */
static uint32_t s_cur_exp = 0;                 /* exposure findings of that host */
static uint16_t s_cur_conf = 0;                /* confirmed web-check hits of that host */
static uint16_t s_cur_web_port = 0;
static volatile bool s_chk_phase = false;      /* web checks running (draw) */
static volatile bool s_report_pending = false; /* "Save report" chosen (SD I/O in task) */
static bool s_last_run_full = false;           /* last Assess All was part of a Full NetAudit */

/* Selected port shown in PORT_DETAIL. */
static char s_detail_ip[POOM_SECURITY_AUDIT_IP_STR_CAP] = {0};
static uint16_t s_detail_port = 0;
static poom_security_audit_port_state_t s_detail_state = POOM_SECURITY_AUDIT_PORT_OPEN;
static sa_screen_t s_detail_return = SA_SCR_HOST_OPEN;

static void sa_button_cb_(const poom_sbus_msg_t *msg, void *user);
static void sa_task_(void *arg);
static void sa_draw_(void);
static void sa_run_scan_all_(uint8_t profile_choice);
static void sa_log_heap_(const char *when);
static void sa_release_wifi_(void);
static uint8_t sa_audit_class_of_(const poom_security_audit_ident_t *id);
static void sa_handle_select_(void);
static uint32_t sa_svcs_exposures_(void);
static void sa_host_label_(size_t h, char *out, size_t cap);
static poom_security_audit_ident_t *sa_detail_ident_(void);

/* Wi-Fi event callback: clears our live SSID marker on disconnect. has_ip /
 * subnet are polled via poom_wifi_ctrl, which tracks them in its own handlers
 * installed by poom_wifi_ctrl_init_sta(); registering this cb mirrors the
 * proven pattern in menu_wifi_profiles.c. */
static void sa_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user)
{
    (void)user;
    if (info != NULL && info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        s_ssid[0] = '\0';
    }
}

/* ---- helpers ------------------------------------------------------------ */

static int64_t sa_now_ms_(void) { return esp_timer_get_time() / 1000; }

static void sa_wake_(void)
{
    if (s_task != NULL)
    {
        (void)xTaskNotifyGive(s_task);
    }
}

static void sa_set_info_(const char *text, sa_screen_t return_to)
{
    (void)snprintf(s_info, sizeof(s_info), "%.21s", text);
    s_info_return = return_to;
    s_screen = SA_SCR_INFO;
}

static void sa_load_ssid_(void)
{
    s_ssid[0] = '\0';
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK)
    {
        (void)snprintf(s_ssid, sizeof(s_ssid), "%.32s", (const char *)cfg.sta.ssid);
    }
}

static void sa_reset_list_(void)
{
    s_sel = 0;
    s_list_top = 0;
}

/* Saved Wi-Fi profiles are loaded on demand (Wi-Fi screens / save prompt)
 * and dropped when a scan starts, so they never sit on the heap during one. */
static poom_wifi_profile_t *sa_profiles_(void)
{
    if (s_ses->profiles == NULL)
    {
        s_ses->profiles = (poom_wifi_profile_t *)calloc(POOM_WIFI_PROFILE_MAX,
                                                        sizeof(poom_wifi_profile_t));
        s_ses->profile_count = 0;
        if (s_ses->profiles != NULL)
        {
            poom_wifi_profile_store_load(s_ses->profiles, POOM_WIFI_PROFILE_MAX,
                                         &s_ses->profile_count);
        }
    }
    return s_ses->profiles;
}

static void sa_profiles_free_(void)
{
    free(s_ses->profiles);
    s_ses->profiles = NULL;
    s_ses->profile_count = 0;
}

static bool sa_ssid_saved_(const char *ssid)
{
    if (sa_profiles_() == NULL) { return false; }
    for (uint16_t i = 0; i < s_ses->profile_count; ++i)
    {
        if (strncmp(s_ses->profiles[i].ssid, ssid, POOM_WIFI_PROFILE_SSID_CAP) == 0)
        {
            return true;
        }
    }
    return false;
}

/* After a successful connect, go to HOSTS -- but first offer to save a
 * freshly-entered network that is not already in the shared store. */
/* After a light rejoin: continue with what the user pressed. */
static void sa_resume_(void)
{
    uint8_t what = s_resume;
    s_resume = SA_RESUME_NONE;
    switch (what)
    {
    case SA_RESUME_PORTSCAN:
        s_audit_flow = false;
        s_screen = SA_SCR_PORT_PROFILE;
        sa_reset_list_();
        break;
    case SA_RESUME_DETECT:
        s_screen = SA_SCR_PORT_DETAIL;
        s_detect_pending = true;
        break;
    case SA_RESUME_HOSTS_ROW:
        s_screen = SA_SCR_HOSTS;
        s_sel = s_resume_row;
        sa_handle_select_(); /* re-dispatch the Devices action (link is up now) */
        break;
    default:
        s_screen = SA_SCR_HOSTS;
        s_sel = s_host_sel;
        break;
    }
}

static void sa_after_connect_(void)
{
    sa_load_ssid_();
    if (poom_security_audit_subnet_current(&s_ses->subnet) != ESP_OK)
    {
        sa_set_info_("No subnet", SA_SCR_WIFI);
        return;
    }
    if (s_relink_keep)
    {
        /* Same network: the device list, names and scan results still hold. */
        s_relink_keep = false;
        sa_resume_();
        return;
    }
    s_ses->host_count = 0;
    s_ses->net_results = false; /* index-aligned with the (now cleared) list */
    s_net_assessed = false;
    if (s_ses->offer_save && !sa_ssid_saved_(s_ses->edit_ssid))
    {
        s_screen = SA_SCR_SAVE_CONFIRM;
    }
    else
    {
        s_ses->offer_save = false;
        s_screen = SA_SCR_HOSTS;
        sa_reset_list_();
    }
}

/* Clamp the scroll window so s_sel stays visible among `count` rows. */
static void sa_fix_window_(int count)
{
    if (s_sel < 0) { s_sel = 0; }
    if (s_sel >= count) { s_sel = (count > 0) ? count - 1 : 0; }
    if (s_sel < s_list_top) { s_list_top = s_sel; }
    if (s_sel >= s_list_top + (int)SA_VISIBLE_ROWS)
    {
        s_list_top = s_sel - (int)SA_VISIBLE_ROWS + 1;
    }
    if (s_list_top < 0) { s_list_top = 0; }
}

/* ---- progress callback (runs in the worker task during a blocking op) --- */

static void sa_progress_cb_(uint32_t done, uint32_t total, void *user)
{
    (void)user;
    /* During Scan All the bar tracks hosts (k/N), driven by the loop itself;
     * ignore per-port updates so it doesn't look like one host repeating. */
    if (s_scanall_active) { return; }
    s_prog_done = done;
    s_prog_total = total;
    int64_t now = sa_now_ms_();
    if (now - s_prog_last_draw_ms >= SA_PROGRESS_MIN_MS)
    {
        s_prog_last_draw_ms = now;
        sa_draw_();
    }
}

/* ---- discovery / scan workers (called from the task, blocking) ---------- */

static void sa_run_sweep_(void)
{
    sa_profiles_free_(); /* not needed while scanning; reloads on demand */
    s_cancel = false;
    s_prog_done = 0;
    s_prog_total = 0;
    s_screen = SA_SCR_SWEEPING;

    /* hosts[] is about to be rebuilt; Scan All data is index-aligned with it,
     * so the last results (and any Assess All over them) are now stale. */
    s_ses->net_results = false;
    s_net_assessed = false;
    s_net_partial = false;
    s_net_ss_count = 0;
    memset(s_ses->nb->host_open, 0, sizeof(s_ses->nb->host_open));
    memset(s_ses->nb->dev_prod, 0, sizeof(s_ses->nb->dev_prod));
    memset(s_ses->nb->exposures, 0, sizeof(s_ses->nb->exposures));
    memset(s_ses->nb->confirmed, 0, sizeof(s_ses->nb->confirmed));
    memset(s_ses->ids, 0, SA_MAX_HOSTS * sizeof(s_ses->ids[0]));
    s_ses->nb->net_prod_count = 0;

    if (poom_security_audit_subnet_current(&s_ses->subnet) != ESP_OK)
    {
        sa_set_info_("No IP / subnet", SA_SCR_HOSTS);
        return;
    }

    /* ARP-only discovery. The previous ICMP pass spun up an esp_ping session
     * (and task) per address - up to ~254 per sweep - which churned/pressured
     * the heap. ARP finds every L2-reachable host on the subnet without that
     * cost; the OS hint falls back to the MAC vendor (TTL ICMP removed here). */
    poom_security_audit_discovery_opts_t opts = {
        .use_icmp = false, .use_arp = true,
    };
    size_t n = 0;
    (void)poom_security_audit_discover(&s_ses->subnet, &opts,
                                    s_ses->hosts, SA_MAX_HOSTS,
                                    &n, &s_cancel, sa_progress_cb_, NULL);
    s_ses->host_count = n;

    s_screen = SA_SCR_HOSTS;
    sa_reset_list_();
    if (s_cancel)
    {
        sa_set_info_(n ? "Sweep cancelled" : "Cancelled", SA_SCR_HOSTS);
    }
    else if (n == 0)
    {
        sa_set_info_("No hosts found", SA_SCR_HOSTS);
    }
}

/* Scan a prepared port list (already in s_ses->sb->ports) for `target_ip`,
 * appending results. Returns ESP_ERR_NOT_SUPPORTED for an unavailable method. */
static esp_err_t sa_scan_port_list_(const char *ip, size_t n_ports, bool open_only)
{
    poom_security_audit_scan_opts_t opts = {
        .method = (s_method == 1) ? POOM_SECURITY_AUDIT_METHOD_TCP_SYN
                                  : POOM_SECURITY_AUDIT_METHOD_TCP_CONNECT,
        .connect_timeout_ms = 0, /* engine default */
        .fanout = 0,             /* engine default */
        .open_only = open_only,
        .n_closed = &s_ses->n_closed,
        .n_filtered = &s_ses->n_filtered,
    };
    size_t got = 0;
    esp_err_t err = poom_security_audit_port_scan(
        ip, s_ses->sb->ports, n_ports, &opts,
        &s_ses->sb->results[s_ses->result_count],
        SA_RESULT_CAP - s_ses->result_count, &got,
        &s_cancel, sa_progress_cb_, NULL);
    if (err == ESP_OK)
    {
        s_ses->result_count += got;
    }
    return err;
}

/* Scan a single host across the selected profile, windowing FULL so the port
 * buffer stays bounded. profile_choice: 0=Quick 1=Extended 2=Full 3=Custom. */
static esp_err_t sa_scan_host_(const char *ip, uint8_t profile_choice, bool open_only)
{
    esp_err_t err = ESP_OK;

    if (profile_choice == 3) /* Custom: ports already parsed into ports[] */
    {
        /* custom count stashed in s_prog_total before call */
        err = sa_scan_port_list_(ip, (size_t)s_prog_total, open_only);
        return err;
    }

    /* Quick / Extended / Full through the small port buffer, one window at a
     * time (Extended no longer needs a 1000-entry buffer). Stops early once
     * the open-port list is full. */
    static const poom_security_audit_profile_t k_prof[3] = {
        POOM_SECURITY_AUDIT_PROFILE_QUICK, POOM_SECURITY_AUDIT_PROFILE_EXTENDED,
        POOM_SECURITY_AUDIT_PROFILE_FULL};
    poom_security_audit_profile_t prof = k_prof[(profile_choice <= 2U) ? profile_choice : 0U];
    uint32_t offset = 0;
    while (!s_cancel && s_ses->result_count < SA_RESULT_CAP)
    {
        size_t n = poom_security_audit_build_ports_window(prof, offset, s_ses->sb->ports, SA_PORT_CHUNK);
        if (n == 0U) { break; }
        err = sa_scan_port_list_(ip, n, open_only);
        if (err != ESP_OK) { return err; }
        offset += (uint32_t)n;
    }
    return ESP_OK;
}

static size_t sa_count_open_(void)
{
    size_t open = 0;
    for (size_t i = 0; i < s_ses->result_count; ++i)
    {
        if (s_ses->sb->results[i].state == POOM_SECURITY_AUDIT_PORT_OPEN) { open++; }
    }
    return open;
}

/* Service detected for a port in the current single-device scan, or NULL. */
static poom_security_audit_service_t *sa_find_svc_(uint16_t port)
{
    for (size_t i = 0; i < s_ses->as->svc_count; ++i)
    {
        if (s_ses->as->svcs[i].port == port) { return &s_ses->as->svcs[i]; }
    }
    return NULL;
}

/* After a single-device port scan, identify the service on each open port
 * (when enabled). Bounded by the svcs[] capacity; honors cancel. */
static void sa_detect_open_services_(void)
{
    s_ses->as->svc_count = 0;
    if (!s_services_on || s_cancel) { return; }

    size_t open = sa_count_open_();
    size_t cap = sizeof(s_ses->as->svcs) / sizeof(s_ses->as->svcs[0]);
    uint32_t done = 0;

    s_svc_phase = true;
    for (size_t i = 0; i < s_ses->result_count && !s_cancel; ++i)
    {
        if (s_ses->sb->results[i].state != POOM_SECURITY_AUDIT_PORT_OPEN) { continue; }
        if (s_ses->as->svc_count >= cap) { break; }

        s_prog_done = done;
        s_prog_total = (uint32_t)open;
        sa_draw_();

        poom_security_audit_detect_service(s_ses->target_ip, s_ses->sb->results[i].port, 0,
                                        &s_ses->as->svcs[s_ses->as->svc_count], &s_cancel);
        s_ses->as->svcs[s_ses->as->svc_count].port = s_ses->sb->results[i].port;
        s_ses->as->svc_count++;
        done++;
    }
    s_svc_phase = false;
}

/* Count how many of the current matches belong to a term (service). */
static int sa_term_match_count_(uint8_t term_id)
{
    int n = 0;
    for (size_t i = 0; i < s_ses->as->ss_count; ++i)
    {
        if (s_ses->as->ss[i].term_id == term_id) { n++; }
    }
    return n;
}

/* Does the match at index i pass the active tier filter? */
static bool sa_tier_ok_(size_t i)
{
    switch (s_ss_tier_filter)
    {
    case 1: return s_ses->as->ss[i].tier == POOM_NET_AUDIT_TIER_STRONG;
    case 2: return s_ses->as->ss[i].tier == POOM_NET_AUDIT_TIER_MEDIUM;
    case 3: return s_ses->as->ss[i].tier == POOM_NET_AUDIT_TIER_WEAK;
    default: return true; /* ALL */
    }
}

/* Rebuild the By-exploit display list: apply the service + tier filters,
 * de-duplicate by CVE (one row per CVE with a count; non-CVE kept each), and
 * sort strongest-tier first. */
static void sa_build_ss_disp_(void)
{
    s_ses->as->ss_disp_count = 0;
    for (size_t i = 0; i < s_ses->as->ss_count; ++i)
    {
        if (s_ss_service_filter >= 0 && s_ses->as->ss[i].term_id != (uint8_t)s_ss_service_filter)
        {
            continue;
        }
        if (!sa_tier_ok_(i)) { continue; }

        /* CVE de-dupe: if this CVE is already shown, bump its count. */
        if (s_ses->as->ss[i].cve[0] != '\0')
        {
            bool dup = false;
            for (uint8_t d = 0; d < s_ses->as->ss_disp_count; ++d)
            {
                const char *ex = s_ses->as->ss[s_ses->as->ss_disp[d].idx].cve;
                if (ex[0] != '\0' && strcmp(ex, s_ses->as->ss[i].cve) == 0)
                {
                    s_ses->as->ss_disp[d].count++;
                    dup = true;
                    break;
                }
            }
            if (dup) { continue; }
        }
        if (s_ses->as->ss_disp_count < sizeof(s_ses->as->ss_disp) / sizeof(s_ses->as->ss_disp[0]))
        {
            s_ses->as->ss_disp[s_ses->as->ss_disp_count].idx = (uint8_t)i;
            s_ses->as->ss_disp[s_ses->as->ss_disp_count].count = 1;
            s_ses->as->ss_disp_count++;
        }
    }
    /* sort strongest first (insertion sort, small n) */
    for (int a = 1; a < (int)s_ses->as->ss_disp_count; ++a)
    {
        uint8_t ki = s_ses->as->ss_disp[a].idx;
        uint8_t kc = s_ses->as->ss_disp[a].count;
        int b = a - 1;
        while (b >= 0 && s_ses->as->ss[s_ses->as->ss_disp[b].idx].tier < s_ses->as->ss[ki].tier)
        {
            s_ses->as->ss_disp[b + 1] = s_ses->as->ss_disp[b];
            b--;
        }
        s_ses->as->ss_disp[b + 1].idx = ki;
        s_ses->as->ss_disp[b + 1].count = kc;
    }
    s_sel = 0;
    s_result_top = 0;
}

/* ---- Wi-Fi pause for the CSV pass ---------------------------------------
 * Measured on device: Wi-Fi connected + the session leave ~21 KB free, ~11 KB
 * after a scan - too little for the FAT mount (raw ESP_ERR_NO_MEM). The CSV
 * pass needs no network, so the radio is switched off for it (frees ~40 KB)
 * and stays off; results are in RAM. A later network action rejoins on
 * demand (sa_need_link_). An automatic restart right after the pass
 * fragmented the heap so badly the next app entry could not start Wi-Fi. */
static bool s_wifi_suspended = false;

static void sa_wifi_suspend_(void)
{
    bool linked = poom_wifi_ctrl_sta_has_ip() || poom_wifi_ctrl_sta_is_connected();
    if (linked)
    {
        /* Remember the live network so we can rejoin it (works whether we
         * connected it or it was already up when the app opened). */
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK)
        {
            (void)snprintf(s_ses->edit_ssid, sizeof(s_ses->edit_ssid), "%.32s",
                           (const char *)cfg.sta.ssid);
            (void)snprintf(s_ses->edit_pass, sizeof(s_ses->edit_pass), "%.63s",
                           (const char *)cfg.sta.password);
        }
    }
    sa_release_wifi_();
    s_wifi_suspended = linked;
    sa_log_heap_("wifi off");
}

/* Network actions call this first. Linked -> true. Paused by a CSV pass ->
 * rejoin the remembered network (CONNECTING screen; the user re-picks the
 * action once connected) and return false. Otherwise send them to the Wi-Fi
 * screen. One Wi-Fi start per need, never a silent background restart: a
 * mid-session restart is what fragmented the heap on device. */
static bool sa_need_link_(uint8_t resume)
{
    if (poom_wifi_ctrl_sta_has_ip()) { return true; }
    if (s_wifi_suspended && s_ses->edit_ssid[0] != '\0')
    {
        /* Rejoin in the task, then carry on with @p resume. */
        s_resume = resume;
        s_resume_row = s_sel;
        s_relink_pending = true;
        return false;
    }
    sa_set_info_("Not connected", SA_SCR_WIFI);
    return false;
}

static void sa_log_db_pass_(void)
{
    static const char *const k_how[] = {"full scan", "indexed", "full scan + index built"};
    poom_net_audit_index_use_t how = poom_net_audit_last_index_use();
    printf("[security_audit] DB pass: %s\n", k_how[(how <= POOM_NET_AUDIT_IDX_BUILT) ? how : 0]);
    s_idx_building = false;
}

/* Mount the SD (if needed) and check the Exploit-DB CSV. On failure shows a
 * message that says WHICH problem it is, returning to @p back. */
static bool sa_db_ready_(sa_screen_t back)
{
    bool was_mounted = sd_card_is_mounted();
    if (!was_mounted) { sa_log_heap_("pre-mount"); }
    esp_err_t st = poom_net_audit_db_status();
    if (st == ESP_OK)
    {
        if (!was_mounted) { sa_log_heap_("mounted"); }
        return true;
    }
    poom_net_audit_release();
    if (st == ESP_ERR_NOT_FOUND) { sa_set_info_("No EDB on SD card", back); return false; }

    esp_err_t raw = sd_card_last_raw_err();
    printf("[security_audit] SD mount failed: %s (raw %s)\n",
           esp_err_to_name(poom_net_audit_last_mount_err()), esp_err_to_name(raw));
    sa_log_heap_("mount-fail");
    if (raw == ESP_ERR_NO_MEM)    { sa_set_info_("No RAM to open SD", back); }
    else if (raw == ESP_FAIL)     { sa_set_info_("SD: no FAT volume", back); }
    else                          { sa_set_info_("SD mount failed", back); }
    return false;
}

/* Allocate the Assess-All match buffer if not already held. */
static bool sa_net_ss_alloc_(void)
{
    if (s_net_ss == NULL)
    {
        s_net_ss = (poom_net_audit_match_t *)calloc(SA_NET_MATCH_CAP, sizeof(*s_net_ss));
    }
    return s_net_ss != NULL;
}

/* Per-device Assess: build terms from detected services, one CSV pass, counts. */
static void sa_run_assess_(void)
{
    s_assess_net = false; /* per-device results, not a network projection */
    s_net_direct = false;
    s_proj_host = -1;
    s_cur_exp = sa_svcs_exposures_(); /* findings need no Exploit-DB */
    s_cur_conf = s_ses->detail_conf;
    s_cur_web_port = s_ses->detail_web_port;
    s_cancel = false;
    s_prog_done = 0;
    s_prog_total = 0;
    s_ses->as->ss_count = 0;
    s_ses->as->ss_n_terms = 0;
    s_ses->as->vuln_potential = 0;
    s_ses->as->vuln_nomatch = 0;
    s_ses->as->vuln_unassessed = 0;

    if (s_ses->as->svc_count == 0)
    {
        sa_set_info_("Scan with Svc:ON first", SA_SCR_RESULTS);
        return;
    }

    /* Build one search term per open-port service that has a product; count the
     * product-less ones as "unassessed". */
    uint8_t cap_terms = sizeof(s_ses->as->ss_terms) / sizeof(s_ses->as->ss_terms[0]);
    for (size_t i = 0; i < s_ses->as->svc_count; ++i)
    {
        if (s_ses->as->svcs[i].product[0] != '\0')
        {
            if (s_ses->as->ss_n_terms < cap_terms)
            {
                s_ses->as->ss_terms[s_ses->as->ss_n_terms].term = s_ses->as->svcs[i].product;
                s_ses->as->ss_terms[s_ses->as->ss_n_terms].version = s_ses->as->svcs[i].version;
                s_ses->as->ss_term_svc[s_ses->as->ss_n_terms] = (uint8_t)i;
                s_ses->as->ss_n_terms++;
            }
        }
        else
        {
            s_ses->as->vuln_unassessed++;
        }
    }

    if (s_ses->as->ss_n_terms == 0)
    {
        /* Nothing searchable - still show the summary (all unassessed). */
        s_ss_service_filter = -1;
        s_sel = 0;
        s_result_top = 0;
        s_screen = SA_SCR_VULN_SUMMARY;
        return;
    }

    s_ss_searching = true;
    s_screen = SA_SCR_SS_RESULTS; /* reuse this screen's "searching" draw */
    sa_draw_();
    sa_wifi_suspend_(); /* free RAM for the FAT mount; rejoined below */
    bool ready = sa_db_ready_(SA_SCR_RESULTS);
    esp_err_t err = ESP_FAIL;
    if (ready)
    {
        s_idx_building = !poom_net_audit_index_current(POOM_NET_AUDIT_CSV_PATH, POOM_NET_AUDIT_IDX_PATH);
        sa_draw_();
        err = poom_net_audit_search_multi(
            s_ses->as->ss_terms, s_ses->as->ss_n_terms, s_ses->as->ss,
            sizeof(s_ses->as->ss) / sizeof(s_ses->as->ss[0]), 8 /* per-term cap */,
            &s_ses->as->ss_count, &s_cancel, sa_progress_cb_, NULL);
        sa_log_db_pass_();
        poom_net_audit_release(); /* SD mounted only for the CSV pass */
    }
    s_ss_searching = false;
    /* Wi-Fi stays off; sa_need_link_() rejoins when a network action needs it. */
    if (!ready) { return; } /* message already set */

    if (err == ESP_ERR_NOT_FOUND) { sa_set_info_("EDB file missing", SA_SCR_RESULTS); return; }
    if (err != ESP_OK)            { sa_set_info_("SD read error", SA_SCR_RESULTS); return; }

    /* Drop leads whose platform cannot run on this host's known OS. */
    s_cur_os_class = sa_audit_class_of_(sa_detail_ident_());
    {
        size_t k = 0;
        for (size_t i = 0; i < s_ses->as->ss_count; ++i)
        {
            if (poom_net_audit_platform_fit(s_ses->as->ss[i].platform, s_cur_os_class) ==
                POOM_NET_AUDIT_FIT_NO)
            {
                continue;
            }
            s_ses->as->ss[k++] = s_ses->as->ss[i];
        }
        s_ses->as->ss_count = k;
    }

    /* Per-service flagged counts. */
    for (uint8_t t = 0; t < s_ses->as->ss_n_terms; ++t)
    {
        if (sa_term_match_count_(t) > 0) { s_ses->as->vuln_potential++; }
        else { s_ses->as->vuln_nomatch++; }
    }

    s_ss_service_filter = -1;
    s_ss_tier_filter = 0;
    s_sel = 0;
    s_result_top = 0;
    s_screen = SA_SCR_VULN_SUMMARY;
}

/* Number of hosts that have >=1 open port (for the network summary list). */
static int sa_open_host_count_(void)
{
    int n = 0;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_ses->nb->host_open[h].count > 0) { n++; }
    }
    return n;
}

/* Host index of the n-th host-with-open, or -1. */
static int sa_nth_open_host_(int n)
{
    int k = 0;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_ses->nb->host_open[h].count > 0)
        {
            if (k == n) { return (int)h; }
            k++;
        }
    }
    return -1;
}

/* ---- identity (names + OS) ----------------------------------------------- */

/* OS class for exploit platform filtering. Only inferred/confirmed identities
 * filter; a port/vendor guess is shown but never hides a lead. */
static uint8_t sa_audit_class_of_(const poom_security_audit_ident_t *id)
{
    if (id == NULL || id->os_ev < POOM_NS_OSEV_INFERRED) { return POOM_NET_AUDIT_OS_UNKNOWN; }
    switch (id->os)
    {
    case POOM_NS_OS_WINDOWS:  return POOM_NET_AUDIT_OS_WINDOWS;
    case POOM_NS_OS_LINUX:    case POOM_NS_OS_UBUNTU:  case POOM_NS_OS_DEBIAN:
    case POOM_NS_OS_RHEL:     case POOM_NS_OS_RPI:     case POOM_NS_OS_OPENWRT:
    case POOM_NS_OS_ROUTEROS: return POOM_NET_AUDIT_OS_LINUX;
    case POOM_NS_OS_ANDROID:  return POOM_NET_AUDIT_OS_ANDROID;
    case POOM_NS_OS_BSD:      return POOM_NET_AUDIT_OS_BSD;
    case POOM_NS_OS_MACOS:    return POOM_NET_AUDIT_OS_MACOS;
    case POOM_NS_OS_IOS:      return POOM_NET_AUDIT_OS_IOS;
    case POOM_NS_OS_POSIX:    case POOM_NS_OS_APPLE:
                              return POOM_NET_AUDIT_OS_POSIX;
    case POOM_NS_OS_UNKNOWN:  return POOM_NET_AUDIT_OS_UNKNOWN;
    default:                  return POOM_NET_AUDIT_OS_HARDWARE; /* embedded + device types */
    }
}

static const char *sa_vendor_of_(const poom_security_audit_host_t *h)
{
    if (!h->mac_known || menu_wifi_detector_oui_is_random(h->mac)) { return NULL; }
    return menu_wifi_detector_oui_vendor(h->mac);
}

/* Fold the clues that need no extra traffic: TTL family, open-port
 * signature, "is the gateway", MAC vendor. */
static void sa_fold_local_clues_(poom_security_audit_ident_t *id, const poom_security_audit_host_t *h,
                                 const uint16_t *ports, size_t n)
{
    if (id->ttl_known) { poom_ns_os_from_ttl(id, id->ttl); }
    poom_ns_os_from_ports(id, ports, n);
    if (h->ip != 0U && h->ip == s_ses->subnet.gateway)
    {
        poom_ns_os_add(id, POOM_NS_OS_NETDEV, POOM_NS_OSEV_GUESS);
    }
    poom_ns_os_from_vendor(id, sa_vendor_of_(h));
}

/* Identity shown for the HOST_DETAIL target. */
static poom_security_audit_ident_t *sa_detail_ident_(void)
{
    if (s_detail_hi >= 0 && (size_t)s_detail_hi < s_ses->host_count)
    {
        return &s_ses->ids[s_detail_hi];
    }
    return &s_ses->detail_id;
}

static uint32_t sa_parse_ip_(const char *s)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255U || b > 255U ||
        c > 255U || d > 255U)
    {
        return 0U;
    }
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ---- Assess All (network-wide) ---------------------------------------- */

/* Network category of host h after Assess All. */
/* Product ids searched for host h: one per open port + the UPnP device
 * product. @return count written to @p out (cap SA_HOST_PORTS + 1). */
static uint8_t sa_host_prods_(size_t h, uint8_t *out)
{
    uint8_t n = s_ses->nb->host_open[h].count;
    memcpy(out, s_ses->nb->host_open[h].prod, n);
    if (s_ses->nb->dev_prod[h] != 0U) { out[n++] = s_ses->nb->dev_prod[h]; }
    return n;
}

/* Exposure findings for host h from what Scan All observed (no probing). */
/* Observed services of host h (from Scan All) for rule evaluation. */
static uint8_t sa_host_svc_list_(size_t h, poom_net_audit_svc_t *v)
{
    uint8_t n = s_ses->nb->host_open[h].count;
    for (uint8_t i = 0; i < n; ++i)
    {
        uint8_t sid = s_ses->nb->host_open[h].svc_id[i];
        uint8_t pid = s_ses->nb->host_open[h].prod[i];
        v[i].port = s_ses->nb->host_open[h].ports[i];
        v[i].service = (sid != 0U) ? poom_security_audit_service_by_id(sid) : NULL;
        bool p = (pid != 0U && pid != POOM_NET_AUDIT_INTERN_FULL);
        v[i].product = p ? s_ses->nb->net_prod[pid - 1U].product : NULL;
        v[i].version = p ? s_ses->nb->net_prod[pid - 1U].version : NULL;
    }
    return n;
}

static uint32_t sa_host_exposures_(size_t h)
{
    poom_net_audit_svc_t v[SA_HOST_PORTS];
    uint8_t n = sa_host_svc_list_(h, v);
    return poom_net_audit_exposures(v, n, s_ses->nb->dev_prod[h] != 0U);
}

/* Exposure findings for the single-device scan (svcs[] of the target). */
/* Observed services of the single-device target for rule evaluation. */
static size_t sa_svcs_list_(poom_net_audit_svc_t *v, bool *upnp_out)
{
    size_t n = 0;
    bool upnp = false;
    for (size_t i = 0; i < s_ses->as->svc_count && n < 16U; ++i)
    {
        const poom_security_audit_service_t *sv = &s_ses->as->svcs[i];
        if (strcmp(sv->service, "upnp") == 0) { upnp = true; continue; }
        v[n].port = sv->port;
        v[n].service = (sv->evidence >= POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE) ? sv->service : NULL;
        v[n].product = sv->product;
        v[n].version = sv->version;
        n++;
    }
    /* open ports without a detected service still count by port number */
    for (size_t i = 0; i < s_ses->result_count && n < 16U; ++i)
    {
        uint16_t port = s_ses->sb->results[i].port;
        bool have = false;
        for (size_t j = 0; j < n; ++j) { if (v[j].port == port) { have = true; } }
        if (!have) { v[n].port = port; v[n].service = NULL; v[n].product = NULL; v[n].version = NULL; n++; }
    }
    *upnp_out = upnp;
    return n;
}

static uint32_t sa_svcs_exposures_(void)
{
    poom_net_audit_svc_t v[16];
    bool upnp = false;
    size_t n = sa_svcs_list_(v, &upnp);
    return poom_net_audit_exposures(v, n, upnp);
}

/* ---- exposure rules (built-in table in poom_net_audit_rules.c) ----------- */

static uint8_t sa_rule_count_(void) { return (uint8_t)poom_net_audit_builtin_count(); }
static const char *sa_rule_title_(uint8_t b) { return poom_net_audit_exp_title(b); }
static const char *sa_rule_detail_(uint8_t b) { return poom_net_audit_exp_detail(b); }
static bool sa_rule_detected_(uint8_t b)
{
    return poom_net_audit_exp_severity(b) == POOM_NET_AUDIT_SEV_DETECTED;
}
static unsigned sa_popcount_(uint32_t m)
{
    unsigned n = 0;
    while (m != 0U) { n += m & 1U; m >>= 1; }
    return n;
}

/* Findings for every host from what Scan All observed. */
static void sa_net_exposures_builtin_(void)
{
    s_ses->nb->net_exp_hosts = 0;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        s_ses->nb->exposures[h] = sa_host_exposures_(h);
        if (s_ses->nb->exposures[h] != 0U || s_ses->nb->confirmed[h] != 0U)
        {
            s_ses->nb->net_exp_hosts++;
        }
    }
}

/* ---- active read-only web checks -------------------------------------------- */

/* Plain-HTTP ports worth checking (identified as http, or a common web port);
 * HTTPS ports are skipped (no TLS). At most @p cap. */
static size_t sa_web_ports_(const uint16_t *ports, const char *const *services, size_t n,
                            uint16_t *out, size_t cap)
{
    static const uint16_t k_web[] = {80, 8080, 8000, 8008, 8081, 8888, 5000, 9000, 1400};
    size_t k = 0;
    for (size_t i = 0; i < n && k < cap; ++i)
    {
        uint16_t p = ports[i];
        if (p == 443U || p == 8443U) { continue; }
        bool web = (services != NULL && services[i] != NULL && strcmp(services[i], "http") == 0);
        for (size_t j = 0; !web && j < sizeof(k_web) / sizeof(k_web[0]); ++j) { web = (p == k_web[j]); }
        if (web) { out[k++] = p; }
    }
    return k;
}

/* GET a random nonsense path (baseline) then each built-in check on every
 * port; returns the hit mask and the port of the first hit. GET only. */
static uint16_t sa_run_web_checks_(uint32_t ip, const uint16_t *ports, size_t n, uint16_t *hit_port)
{
    uint16_t mask = 0;
    *hit_port = (n > 0U) ? ports[0] : 0U;
    if (ip == 0U || n == 0U) { return 0; }
    char *base = (char *)malloc(768);
    char *resp = (char *)malloc(768);
    if (base == NULL || resp == NULL)
    {
        free(base);
        free(resp);
        printf("[security_audit] web checks: no RAM\n");
        return 0;
    }
    for (size_t pi = 0; pi < n && !s_cancel; ++pi)
    {
        char nx[40];
        (void)snprintf(nx, sizeof(nx), "/poom-%08lx-nx.html",
                       (unsigned long)(esp_timer_get_time() & 0xFFFFFFFFLL));
        int bl = poom_security_audit_http_get(ip, ports[pi], nx, base, 768);
        if (bl <= 0) { continue; } /* port does not answer HTTP */
        for (size_t c = 0; c < poom_net_audit_check_count() && !s_cancel; ++c)
        {
            const poom_net_audit_check_t *ck = poom_net_audit_check(c);
            if ((mask >> c) & 1U) { continue; } /* already seen on another port */
            int rl = poom_security_audit_http_get(ip, ports[pi], ck->path, resp, 768);
            if (rl > 0 && poom_net_audit_check_match(ck, resp, (size_t)rl, base, (size_t)bl))
            {
                if (mask == 0U) { *hit_port = ports[pi]; }
                mask |= (uint16_t)(1U << c);
                printf("[security_audit] check hit: %u.%u.%u.%u:%u %s\n",
                       (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFFU), (unsigned)((ip >> 8) & 0xFFU),
                       (unsigned)(ip & 0xFFU), (unsigned)ports[pi], ck->id);
            }
        }
    }
    free(base);
    free(resp);
    return mask;
}

/* Hosts that have anything to assess (open ports or a UPnP device product). */
static int sa_assessable_host_count_(void)
{
    int n = 0;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_ses->nb->host_open[h].count > 0U || s_ses->nb->dev_prod[h] != 0U) { n++; }
    }
    return n;
}

/* Per-host hit counts per network term, ignoring matches whose platform
 * cannot run on this host's (inferred/confirmed) OS. */
static void sa_host_term_counts_(size_t h, uint8_t *counts)
{
    memset(counts, 0, SA_NET_TERMS);
    uint8_t cls = sa_audit_class_of_(&s_ses->ids[h]);
    for (size_t i = 0; i < s_net_ss_count; ++i)
    {
        const poom_net_audit_match_t *m = &s_net_ss[i];
        if (m->term_id >= s_ses->nb->net_prod_count) { continue; }
        if (poom_net_audit_platform_fit(m->platform, cls) == POOM_NET_AUDIT_FIT_NO) { continue; }
        if (counts[m->term_id] < 255U) { counts[m->term_id]++; }
    }
}

static poom_net_audit_host_class_t sa_host_class_(size_t h)
{
    uint8_t counts[SA_NET_TERMS];
    uint8_t prods[SA_HOST_PORTS + 1U];
    sa_host_term_counts_(h, counts);
    uint8_t np = sa_host_prods_(h, prods);
    return poom_net_audit_host_class(prods, np, counts, s_ses->nb->net_prod_count);
}

/* Map a NET_VULN row (0 potential, 1 no match, 2 unassessed) to the class. */
static poom_net_audit_host_class_t sa_cat_class_(uint8_t cat)
{
    switch (cat)
    {
    case 0:  return POOM_NET_AUDIT_HOST_POTENTIAL;
    case 1:  return POOM_NET_AUDIT_HOST_NOMATCH;
    default: return POOM_NET_AUDIT_HOST_UNASSESSED;
    }
}

/* Hosts in category s_net_cat: count, and the n-th one's index (-1). */
static int sa_net_cat_host_(int n, int *count_out)
{
    poom_net_audit_host_class_t want = sa_cat_class_(s_net_cat);
    int k = 0, found = -1;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_net_cat == 3)
        {
            if (s_ses->nb->exposures[h] == 0U && s_ses->nb->confirmed[h] == 0U) { continue; }
        }
        else if (sa_host_class_(h) != want) { continue; }
        if (k == n) { found = (int)h; }
        k++;
    }
    if (count_out != NULL) { *count_out = k; }
    return found;
}

/* Number of host h's open ports whose product matched (Potential services). */
static unsigned sa_host_hit_svcs_(size_t h)
{
    uint8_t counts[SA_NET_TERMS];
    uint8_t prods[SA_HOST_PORTS + 1U];
    sa_host_term_counts_(h, counts);
    uint8_t np = sa_host_prods_(h, prods);
    unsigned n = 0;
    for (uint8_t i = 0; i < np; ++i)
    {
        uint8_t id = prods[i];
        if (id == 0U || id == POOM_NET_AUDIT_INTERN_FULL) { continue; }
        if (counts[id - 1U] > 0U) { n++; }
    }
    return n;
}

static void sa_net_classify_(void)
{
    poom_net_audit_count_terms(s_net_ss, s_net_ss_count, s_ses->nb->net_hits, s_ses->nb->net_prod_count);
    memset(s_ses->nb->net_cls_count, 0, sizeof(s_ses->nb->net_cls_count));
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        switch (sa_host_class_(h))
        {
        case POOM_NET_AUDIT_HOST_POTENTIAL:  s_ses->nb->net_cls_count[0]++; break;
        case POOM_NET_AUDIT_HOST_NOMATCH:    s_ses->nb->net_cls_count[1]++; break;
        case POOM_NET_AUDIT_HOST_UNASSESSED: s_ses->nb->net_cls_count[2]++; break;
        default: break;
        }
    }
}

/* ---- audit report (plain text on the SD card) ------------------------------ */

#define SA_REPORT_DIR "/sdcard/poom_netaudit"

/* Write one device's section. Only facts the scan gathered; exploit leads are
 * labelled potential. */
static void sa_report_host_(FILE *f, size_t h)
{
    const poom_security_audit_host_t *ho = &s_ses->hosts[h];
    const poom_security_audit_ident_t *id = &s_ses->ids[h];
    char lab[POOM_NS_NAME_CAP + 8];
    sa_host_label_(h, lab, sizeof(lab));
    fprintf(f, "\n[%s]  %s\n", ho->ip_str, lab);
    if (id->name[0] != '\0') { fprintf(f, "  Name: %s\n", id->name); }
    if (ho->mac_known)
    {
        const char *v = sa_vendor_of_(ho);
        fprintf(f, "  MAC:  %02X:%02X:%02X:%02X:%02X:%02X  %s\n", ho->mac[0], ho->mac[1], ho->mac[2],
                ho->mac[3], ho->mac[4], ho->mac[5],
                menu_wifi_detector_oui_is_random(ho->mac) ? "(random MAC)" : (v ? v : ""));
    }
    if (id->os != POOM_NS_OS_UNKNOWN)
    {
        fprintf(f, "  OS:   %s (%s)", poom_ns_os_label(id->os), poom_ns_osev_label(id->os_ev));
        if (id->ttl_known) { fprintf(f, "  ping TTL %u", (unsigned)id->ttl); }
        fprintf(f, "\n");
    }
    uint8_t dp = s_ses->nb->dev_prod[h];
    if (dp != 0U && dp != POOM_NET_AUDIT_INTERN_FULL)
    {
        fprintf(f, "  Model (UPnP): %s %s  [port %u]\n", s_ses->nb->net_prod[dp - 1U].product,
                s_ses->nb->net_prod[dp - 1U].version, (unsigned)s_ses->nb->dev_port[h]);
    }

    uint8_t n = s_ses->nb->host_open[h].count;
    if (n > 0U)
    {
        fprintf(f, "  Open ports%s:\n", s_ses->nb->host_open[h].overflow ? " (first 12)" : "");
        for (uint8_t i = 0; i < n; ++i)
        {
            uint8_t sid = s_ses->nb->host_open[h].svc_id[i];
            uint8_t pid = s_ses->nb->host_open[h].prod[i];
            uint16_t port = s_ses->nb->host_open[h].ports[i];
            fprintf(f, "    %5u/tcp  %-10s", (unsigned)port,
                    (sid != 0U) ? poom_security_audit_service_by_id(sid) : poom_security_audit_service_name(port));
            if (pid != 0U && pid != POOM_NET_AUDIT_INTERN_FULL)
            {
                fprintf(f, " %s %s", s_ses->nb->net_prod[pid - 1U].product, s_ses->nb->net_prod[pid - 1U].version);
            }
            fprintf(f, "%s\n", (sid != 0U) ? "" : "  (guess from port)");
        }
    }

    uint16_t conf = s_ses->nb->confirmed[h];
    if (conf != 0U)
    {
        fprintf(f, "  CONFIRMED (read-only GET on port %u):\n", (unsigned)s_ses->nb->web_port[h]);
        for (uint8_t c = 0; c < poom_net_audit_check_count(); ++c)
        {
            if (((conf >> c) & 1U) == 0U) { continue; }
            const poom_net_audit_check_t *ck = poom_net_audit_check(c);
            fprintf(f, "    - %s (GET %s): %s\n", ck->title, ck->path, ck->detail);
        }
    }
    uint32_t exp = s_ses->nb->exposures[h];
    for (int pass = 0; pass < 2; ++pass)
    {
        bool want = (pass == 0);
        bool any = false;
        for (uint8_t b = 0; b < poom_net_audit_builtin_count(); ++b)
        {
            if (((exp >> b) & 1UL) == 0U ||
                (poom_net_audit_exp_severity(b) == POOM_NET_AUDIT_SEV_DETECTED) != want)
            {
                continue;
            }
            if (!any) { fprintf(f, "  %s\n", want ? "Weaknesses:" : "Good to know:"); any = true; }
            fprintf(f, "    - %s: %s\n", poom_net_audit_exp_title(b), poom_net_audit_exp_detail(b));
        }
    }

    /* Exploit leads for this device's products that fit its OS. */
    if (s_net_ss != NULL && s_net_ss_count > 0U)
    {
        uint8_t prods[SA_HOST_PORTS + 1U];
        uint8_t np = sa_host_prods_(h, prods);
        uint8_t cls = sa_audit_class_of_(id);
        unsigned shown = 0, total = 0;
        static const char *const k_tier[3] = {"WEAK", "MEDIUM", "STRONG"};
        for (int tier = 2; tier >= 0; --tier)
        {
            for (size_t i = 0; i < s_net_ss_count; ++i)
            {
                const poom_net_audit_match_t *m = &s_net_ss[i];
                if ((int)m->tier != tier) { continue; }
                bool mine = false;
                for (uint8_t k = 0; k < np && !mine; ++k)
                {
                    mine = (prods[k] != 0U && prods[k] != POOM_NET_AUDIT_INTERN_FULL &&
                            (uint8_t)(prods[k] - 1U) == m->term_id);
                }
                if (!mine || poom_net_audit_platform_fit(m->platform, cls) == POOM_NET_AUDIT_FIT_NO)
                {
                    continue;
                }
                if (total++ == 0U) { fprintf(f, "  Exploit leads (POTENTIAL - verify):\n"); }
                if (shown < 10U)
                {
                    fprintf(f, "    - EDB-%s %-6s %s  %s [%s]\n", m->edb_id, k_tier[m->tier & 3U],
                            m->cve[0] ? m->cve : "no-CVE", m->title, m->platform);
                    shown++;
                }
            }
        }
        if (total > shown) { fprintf(f, "    ... %u more\n", total - shown); }
    }
}

/* Write the report for the network just assessed. Mounts the SD (lean; Wi-Fi
 * is normally off at this point) and releases it again. Returns the report
 * number, or 0 if it could not be written. */
static unsigned sa_write_report_(void)
{
    if (poom_net_audit_sd_open() != ESP_OK)
    {
        printf("[security_audit] report: SD not mountable\n");
        return 0;
    }
    (void)mkdir(SA_REPORT_DIR, 0775);
    char path[48];
    unsigned num = 0;
    for (unsigned k = 1; k <= 999U; ++k)
    {
        (void)snprintf(path, sizeof(path), SA_REPORT_DIR "/audit_%03u.txt", k);
        FILE *probe = fopen(path, "r");
        if (probe == NULL) { num = k; break; }
        (void)fclose(probe);
    }
    FILE *f = (num != 0U) ? fopen(path, "w") : NULL;
    if (f == NULL)
    {
        printf("[security_audit] report: cannot create file\n");
        poom_net_audit_release();
        return 0;
    }

    const char *ssid = (s_ses->edit_ssid[0] != '\0') ? s_ses->edit_ssid : s_ssid;
    char net[16], gw[16];
    poom_security_audit_ip_to_str(s_ses->subnet.network, net, sizeof(net));
    poom_security_audit_ip_to_str(s_ses->subnet.gateway, gw, sizeof(gw));
    fprintf(f, "POOM Security Audit - audit report #%03u\n", num);
    time_t now = time(NULL);
    if (now > (time_t)1700000000) /* only when the clock has been set */
    {
        char ts[24];
        struct tm tmv;
        (void)localtime_r(&now, &tmv);
        (void)strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tmv);
        fprintf(f, "Date:    %s\n", ts);
    }
    fprintf(f, "Network: %s   %s/%u   gateway %s\n", ssid[0] ? ssid : "(unknown)", net,
            (unsigned)s_ses->subnet.prefix_len, gw);
    fprintf(f, "Run:     %s%s\n", s_last_run_full ? "Full NetAudit" : "Assess All",
            s_net_partial ? " (cancelled - partial results)" : "");
    fprintf(f, "Devices: %u found, %d with something to report, %u with exposures\n",
            (unsigned)s_ses->host_count, sa_assessable_host_count_(), (unsigned)s_ses->nb->net_exp_hosts);
    fprintf(f, "Exploit leads are POTENTIAL matches from Exploit-DB titles - verify before acting.\n");
    fprintf(f, "Checks were read-only (GET requests); nothing on the devices was changed.\n");
    fprintf(f, "============================================================\n");

    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_ses->nb->host_open[h].count > 0U || s_ses->nb->dev_prod[h] != 0U ||
            s_ses->nb->exposures[h] != 0U || s_ses->nb->confirmed[h] != 0U)
        {
            sa_report_host_(f, h);
        }
    }
    bool hdr = false;
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        if (s_ses->nb->host_open[h].count > 0U || s_ses->nb->dev_prod[h] != 0U ||
            s_ses->nb->exposures[h] != 0U || s_ses->nb->confirmed[h] != 0U)
        {
            continue;
        }
        if (!hdr) { fprintf(f, "\nOther devices (no open ports found):\n"); hdr = true; }
        char lab[POOM_NS_NAME_CAP + 8];
        sa_host_label_(h, lab, sizeof(lab));
        fprintf(f, "  %-15s %s\n", s_ses->hosts[h].ip_str, lab);
    }
    bool ok = (fclose(f) == 0);
    poom_net_audit_release();
    if (!ok)
    {
        printf("[security_audit] report: write error\n");
        return 0;
    }
    printf("[security_audit] report saved: %s\n", path);
    return num;
}

/* Assess All: ONE CSV pass over the unique products of every scanned host,
 * then classify hosts. Cancel (B) keeps the partial match set. Returns true
 * when the Network Vuln Summary is ready to show. */
static bool sa_run_assess_all_(void)
{
    s_last_run_full = s_audit_running; /* for the report's "Run:" line */
    s_cancel = false;
    s_prog_done = 0;
    s_prog_total = 0;
    s_net_ss_count = 0;
    s_net_partial = false;
    s_net_assessed = false;

    if (sa_assessable_host_count_() == 0)
    {
        sa_set_info_("No open ports", SA_SCR_NET_SUMMARY);
        return false;
    }

    if (s_ses->nb->net_prod_count > 0)
    {
        poom_net_audit_term_t terms[SA_NET_TERMS];
        for (uint8_t t = 0; t < s_ses->nb->net_prod_count; ++t)
        {
            terms[t].term = s_ses->nb->net_prod[t].product;
            terms[t].version = s_ses->nb->net_prod[t].version;
        }

        s_ss_searching = true;
        s_screen = SA_SCR_NET_VULN; /* draws progress while searching */
        sa_draw_();

        /* Radio off for the pass (frees the RAM the FAT mount needs), then the
         * match buffer + mount; rejoin the network afterwards either way. */
        sa_wifi_suspend_();
        bool ready = sa_net_ss_alloc_();
        if (!ready) { sa_set_info_("Low memory", SA_SCR_NET_SUMMARY); }
        else { ready = sa_db_ready_(SA_SCR_NET_SUMMARY); }
        esp_err_t err = ESP_FAIL;
        if (ready)
        {
            s_idx_building = !poom_net_audit_index_current(POOM_NET_AUDIT_CSV_PATH, POOM_NET_AUDIT_IDX_PATH);
            sa_draw_();
            err = poom_net_audit_search_multi(
                terms, s_ses->nb->net_prod_count, s_net_ss, SA_NET_MATCH_CAP, SA_NET_PER_TERM,
                &s_net_ss_count, &s_cancel, sa_progress_cb_, NULL);
            sa_log_db_pass_();
            poom_net_audit_release(); /* SD mounted only for the CSV pass */
        }
        s_ss_searching = false;
        /* Wi-Fi stays off; sa_need_link_() rejoins when a network action needs it. */
        if (!ready) { return false; } /* message already set */

        if (err == ESP_ERR_NOT_FOUND) { sa_set_info_("EDB file missing", SA_SCR_NET_SUMMARY); return false; }
        if (err != ESP_OK)            { sa_set_info_("SD read error", SA_SCR_NET_SUMMARY); return false; }
        s_net_partial = s_cancel;
    }
    /* else: nothing identified (Svc:OFF or no product banners) - every open
     * host is Unassessed; no CSV pass needed. */

    sa_net_classify_();
    s_net_assessed = true;
    s_sel = 0;
    s_result_top = 0;
    s_screen = SA_SCR_NET_VULN;
    return true; /* the report is saved on request: NET VULN -> "Save report" */
}

/* Project host h's Assess All results into the per-device structures
 * (svcs/ss_terms/ss), so the existing Assess screens show that host. */
static void sa_project_host_(size_t h)
{
    uint8_t local_to_net[16];
    uint8_t cap_terms = sizeof(s_ses->as->ss_terms) / sizeof(s_ses->as->ss_terms[0]);

    (void)snprintf(s_ses->target_ip, sizeof(s_ses->target_ip), "%.15s", s_ses->hosts[h].ip_str);
    memset(s_ses->as->svcs, 0, sizeof(s_ses->as->svcs));
    s_ses->as->svc_count = 0;
    s_ses->as->ss_n_terms = 0;
    s_ses->as->ss_count = 0;
    s_ses->as->vuln_potential = 0;
    s_ses->as->vuln_nomatch = 0;
    s_ses->as->vuln_unassessed = 0;

    for (uint8_t i = 0; i < s_ses->nb->host_open[h].count; ++i)
    {
        poom_security_audit_service_t *sv = &s_ses->as->svcs[s_ses->as->svc_count++];
        uint16_t port = s_ses->nb->host_open[h].ports[i];
        uint8_t sid = s_ses->nb->host_open[h].svc_id[i];
        uint8_t pid = s_ses->nb->host_open[h].prod[i];
        sv->port = port;
        (void)snprintf(sv->service, sizeof(sv->service), "%.15s",
                       (sid != 0) ? poom_security_audit_service_by_id(sid)
                                  : poom_security_audit_service_name(port));
        sv->evidence = (sid != 0) ? POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE
                                  : POOM_SECURITY_AUDIT_EV_PORT_GUESS;

        if (pid != 0U && pid != POOM_NET_AUDIT_INTERN_FULL && s_ses->as->ss_n_terms < cap_terms)
        {
            const poom_net_audit_prodver_t *pv = &s_ses->nb->net_prod[pid - 1U];
            (void)snprintf(sv->product, sizeof(sv->product), "%.27s", pv->product);
            (void)snprintf(sv->version, sizeof(sv->version), "%.23s", pv->version);
            if (pv->version[0] != '\0') { sv->evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED; }

            uint8_t t = s_ses->as->ss_n_terms++;
            s_ses->as->ss_terms[t].term = sv->product;
            s_ses->as->ss_terms[t].version = sv->version;
            s_ses->as->ss_term_svc[t] = (uint8_t)(s_ses->as->svc_count - 1U);
            local_to_net[t] = (uint8_t)(pid - 1U);
        }
        else
        {
            s_ses->as->vuln_unassessed++;
        }
    }

    /* UPnP device product (searched like one more service). */
    uint8_t dp = s_ses->nb->dev_prod[h];
    if (dp != 0U && dp != POOM_NET_AUDIT_INTERN_FULL && s_ses->as->ss_n_terms < cap_terms &&
        s_ses->as->svc_count < sizeof(s_ses->as->svcs) / sizeof(s_ses->as->svcs[0]))
    {
        const poom_net_audit_prodver_t *pv = &s_ses->nb->net_prod[dp - 1U];
        poom_security_audit_service_t *sv = &s_ses->as->svcs[s_ses->as->svc_count++];
        sv->port = s_ses->nb->dev_port[h];
        (void)snprintf(sv->service, sizeof(sv->service), "upnp");
        (void)snprintf(sv->product, sizeof(sv->product), "%.27s", pv->product);
        (void)snprintf(sv->version, sizeof(sv->version), "%.23s", pv->version);
        sv->evidence = (pv->version[0] != '\0') ? POOM_SECURITY_AUDIT_EV_VERSION_REPORTED
                                                : POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        uint8_t t = s_ses->as->ss_n_terms++;
        s_ses->as->ss_terms[t].term = sv->product;
        s_ses->as->ss_terms[t].version = sv->version;
        s_ses->as->ss_term_svc[t] = (uint8_t)(s_ses->as->svc_count - 1U);
        local_to_net[t] = (uint8_t)(dp - 1U);
    }

    s_cur_os_class = sa_audit_class_of_(&s_ses->ids[h]);
    s_cur_exp = s_ses->nb->exposures[h];
    s_proj_host = (int)h;
    s_cur_conf = s_ses->nb->confirmed[h];
    s_cur_web_port = s_ses->nb->web_port[h];
    s_ses->as->ss_count = poom_net_audit_project(
        s_net_ss, s_net_ss_count, local_to_net, s_ses->as->ss_n_terms,
        s_cur_os_class,
        s_ses->as->ss, sizeof(s_ses->as->ss) / sizeof(s_ses->as->ss[0]));
    /* Potential / No match per service from what fits this host (projection
     * keeps >=1 lead for every service that has a fitting match). */
    for (uint8_t t = 0; t < s_ses->as->ss_n_terms; ++t)
    {
        if (sa_term_match_count_(t) > 0) { s_ses->as->vuln_potential++; }
        else { s_ses->as->vuln_nomatch++; }
    }

    s_assess_net = true;
    s_ss_service_filter = -1;
    s_ss_tier_filter = 0;
    s_sel = 0;
    s_result_top = 0;
    s_screen = SA_SCR_VULN_SUMMARY;
}

/* ---- Full NetAudit ---------------------------------------------------- */

static uint32_t sa_audit_hosts_est_(void)
{
    return (s_ses->host_count > 0) ? (uint32_t)s_ses->host_count : s_ses->subnet.host_count;
}

/* Upper-bound estimate: sweep + worst-case port scan + service id + CSV pass. */
static uint32_t sa_audit_estimate_s_(void)
{
    uint32_t hosts = sa_audit_hosts_est_();
    uint64_t s = poom_security_audit_estimate_scan_s(hosts, s_audit_ports_n, 0, 0);
    s += (uint64_t)hosts * SA_EST_SVC_S_PER_HOST;
    s += (uint64_t)s_ses->subnet.host_count / 50U + 1U; /* ARP: ~10 ms/addr + settle */
    s += SA_EST_ASSESS_S;
    return (s > UINT32_MAX) ? UINT32_MAX : (uint32_t)s;
}

/* Port count per host for a range choice (for the estimate only). */
static uint32_t sa_audit_ports_for_(uint8_t choice)
{
    static const poom_security_audit_profile_t k_prof[2] = {
        POOM_SECURITY_AUDIT_PROFILE_QUICK, POOM_SECURITY_AUDIT_PROFILE_EXTENDED};
    if (choice == 2) { return 65535U; }
    if (choice == 3) { return s_audit_custom_n; }
    return (uint32_t)poom_security_audit_build_ports(k_prof[choice], s_ses->sb->ports, SA_PORT_CHUNK);
}

static void sa_audit_enter_confirm_(uint8_t choice)
{
    s_audit_profile = choice;
    s_audit_ports_n = sa_audit_ports_for_(choice);
    s_screen = SA_SCR_AUDIT_CONFIRM;
}

/* Discover -> Scan All Ports+Svc -> Assess All -> Network Vuln Summary.
 * B cancels the current phase; later phases are skipped and whatever was
 * gathered so far is shown. Services are forced ON for the run. */
static void sa_run_netaudit_(void)
{
    bool svc_saved = s_services_on;
    s_services_on = true;
    s_audit_cancel = false;
    s_audit_running = true;
    s_audit_flow = false;

    /* Preflight: catch a missing card / file / non-FAT card BEFORE a long
     * scan. Not enough RAM to mount with Wi-Fi up is NOT fatal here: phase 3
     * mounts with the radio off. The card is released before scanning, and
     * the 5.8 KB match buffer is only taken at the CSV pass. */
    {
        esp_err_t st = poom_net_audit_db_status();
        poom_net_audit_release();
        if (st == ESP_ERR_NOT_FOUND)
        {
            sa_set_info_("No EDB on SD card", SA_SCR_HOSTS);
            goto done;
        }
        if (st != ESP_OK && sd_card_last_raw_err() != ESP_ERR_NO_MEM)
        {
            printf("[security_audit] preflight SD mount failed: raw %s\n",
                   esp_err_to_name(sd_card_last_raw_err()));
            sa_set_info_(sd_card_last_raw_err() == ESP_FAIL ? "SD: no FAT volume"
                                                             : "SD mount failed",
                         SA_SCR_HOSTS);
            goto done;
        }
    }

    /* 1/3 Discover */
    sa_run_sweep_();
    if (s_audit_cancel || s_cancel)
    {
        sa_set_info_("NetAudit cancelled", SA_SCR_HOSTS);
        goto done;
    }
    if (s_ses->host_count == 0) { goto done; } /* sweep already said "No hosts found" */

    /* 2/3 Scan All Ports+Svc (Custom reads its count from s_prog_total) */
    if (s_audit_profile == 3) { s_prog_total = s_audit_custom_n; }
    sa_run_scan_all_(s_audit_profile);
    if (s_screen != SA_SCR_NET_SUMMARY) { goto done; } /* engine refused (info shown) */
    if (s_audit_cancel || s_cancel)
    {
        sa_set_info_("Cancelled: partial", SA_SCR_NET_SUMMARY);
        goto done;
    }
    if (sa_assessable_host_count_() == 0) { goto done; } /* summary shows "No open ports" */

    /* 3/3 Assess All (cancel inside keeps partial matches, "(partial)") */
    (void)sa_run_assess_all_();

done:
    /* One line per run so a skipped phase is explainable from the monitor. */
    printf("[security_audit] audit end: hosts %u, open hosts %d, products %u, "
           "cancel %d, assessed %d, matches %u\n",
           (unsigned)s_ses->host_count, sa_open_host_count_(),
           (unsigned)s_ses->nb->net_prod_count, (int)(s_audit_cancel || s_cancel),
           (int)s_net_assessed, (unsigned)s_net_ss_count);
    s_services_on = svc_saved;
    s_audit_pending = false; /* drop a duplicate A queued before the run */
    s_audit_running = false;
    s_audit_cancel = false;
}

/* Single-host scan (profile choice carried in s_sel at entry). */
static void sa_run_single_scan_(uint8_t profile_choice)
{
    s_cancel = false;
    s_ses->result_count = 0;
    s_prog_done = 0;
    s_screen = SA_SCR_SCANNING;

    /* For Custom the parsed count is in s_prog_total (set by the OSK accept). */
    uint32_t saved_total = s_prog_total;
    if (profile_choice == 3) { s_prog_total = saved_total; }

    /* Open ports are stored (cap SA_RESULT_CAP); closed/filtered only counted. */
    sa_profiles_free_();
    s_ses->n_closed = 0;
    s_ses->n_filtered = 0;
    esp_err_t err = sa_scan_host_(s_ses->target_ip, profile_choice, true);
    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        sa_set_info_("SYN not available", SA_SCR_PORT_METHOD);
        return;
    }
    s_ses->open_count = sa_count_open_();

    /* Identify services on the open ports (on by default; toggle on the range
     * screen). Skipped automatically if there are no open ports. */
    sa_detect_open_services_();

    /* Identify this one host (name, TTL, SSDP/mDNS clues) + local clues. */
    if (!s_cancel)
    {
        poom_security_audit_host_t t = s_ses->detail_host;
        if (t.ip == 0U) { t.ip = sa_parse_ip_(s_ses->target_ip); }
        poom_security_audit_ident_t *id = sa_detail_ident_();
        memset(id, 0, sizeof(*id));
        if (t.ip != 0U)
        {
            s_prog_done = 0;
            s_prog_total = 1;
            s_id_phase = true;
            sa_draw_();
            poom_security_audit_upnp_t up;
            size_t nup = 0;
            (void)poom_security_audit_identify(&t, 1, id, &up, 1, &nup, &s_cancel);
            s_id_phase = false;
            /* A UPnP device product becomes one more service to assess. */
            size_t cap = sizeof(s_ses->as->svcs) / sizeof(s_ses->as->svcs[0]);
            if (nup == 1U && s_ses->as->svc_count < cap)
            {
                poom_security_audit_service_t *sv = &s_ses->as->svcs[s_ses->as->svc_count++];
                memset(sv, 0, sizeof(*sv));
                sv->port = up.port;
                (void)snprintf(sv->service, sizeof(sv->service), "upnp");
                (void)snprintf(sv->product, sizeof(sv->product), "%.23s", up.model);
                (void)snprintf(sv->version, sizeof(sv->version), "%.15s", up.version);
                (void)snprintf(sv->banner, sizeof(sv->banner), "%.15s %.23s",
                               up.manufacturer, up.model);
                sv->evidence = (up.version[0] != '\0') ? POOM_SECURITY_AUDIT_EV_VERSION_REPORTED
                                                       : POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
            }
        }
        for (size_t i = 0; i < s_ses->as->svc_count; ++i)
        {
            poom_ns_os_from_text(id, s_ses->as->svcs[i].banner, POOM_NS_OSEV_CONFIRMED);
            poom_ns_os_from_text(id, s_ses->as->svcs[i].product, POOM_NS_OSEV_CONFIRMED);
        }
        uint16_t open[16];
        size_t no = 0;
        for (size_t i = 0; i < s_ses->result_count && no < 16U; ++i)
        {
            if (s_ses->sb->results[i].state == POOM_SECURITY_AUDIT_PORT_OPEN)
            {
                open[no++] = s_ses->sb->results[i].port;
            }
        }
        sa_fold_local_clues_(id, &t, open, no);

        /* Read-only web checks on the target's plain-HTTP ports. */
        const char *names[16];
        for (size_t i = 0; i < no; ++i)
        {
            poom_security_audit_service_t *sv = sa_find_svc_(open[i]);
            names[i] = (sv != NULL && sv->evidence >= POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE) ? sv->service : NULL;
        }
        uint16_t wp[2];
        size_t nw = sa_web_ports_(open, names, no, wp, 2);
        s_ses->detail_conf = 0;
        if (nw > 0U && t.ip != 0U && !s_cancel)
        {
            (void)snprintf(s_scan_ip, sizeof(s_scan_ip), "%.15s", s_ses->target_ip);
            s_prog_done = 0;
            s_prog_total = 1;
            s_chk_phase = true;
            sa_draw_();
            s_ses->detail_conf = sa_run_web_checks_(t.ip, wp, nw, &s_ses->detail_web_port);
            s_chk_phase = false;
        }
    }

    s_sel = 0;
    s_result_top = 0;
    s_screen = SA_SCR_RESULTS;
    if (s_cancel)
    {
        /* keep partial results, note it */
        (void)snprintf(s_info, sizeof(s_info), "Cancelled");
    }
}

/* Scan All Devices: iterate discovered hosts, open-only to bound memory. */
static void sa_run_scan_all_(uint8_t profile_choice)
{
    s_cancel = false;
    s_ses->all_hosts_up = 0;
    s_ses->all_open_total = 0;
    sa_profiles_free_();
    s_ses->nb->net_prod_count = 0;       /* new scan => previous Assess All is stale */
    memset(s_ses->nb->dev_prod, 0, sizeof(s_ses->nb->dev_prod));
    memset(s_ses->nb->exposures, 0, sizeof(s_ses->nb->exposures));
    memset(s_ses->nb->confirmed, 0, sizeof(s_ses->nb->confirmed));
    memset(s_ses->ids, 0, SA_MAX_HOSTS * sizeof(s_ses->ids[0]));
    s_net_assessed = false;
    s_net_partial = false;
    s_net_ss_count = 0;
    s_screen = SA_SCR_SCANNING;
    s_scanall_active = true;         /* bar tracks hosts, not per-host ports */

    uint32_t custom_n = s_prog_total; /* custom port count, if profile==3 */

    for (size_t h = 0; h < s_ses->host_count && !s_cancel; ++h)
    {
        s_ses->all_idx = h;
        s_ses->result_count = 0;
        s_ses->nb->host_open[h].count = 0;
        s_ses->nb->host_open[h].overflow = false;
        memset(s_ses->nb->host_open[h].prod, 0, sizeof(s_ses->nb->host_open[h].prod));

        /* Host-level progress + current IP, redrawn once per host. */
        (void)snprintf(s_scan_ip, sizeof(s_scan_ip), "%.15s", s_ses->hosts[h].ip_str);
        s_prog_done = (uint32_t)h;
        s_prog_total = (uint32_t)s_ses->host_count;
        s_prog_last_draw_ms = sa_now_ms_();
        sa_draw_();

        if (profile_choice == 3) { s_prog_total = custom_n; } /* sa_scan_host_ reads this */
        esp_err_t err = sa_scan_host_(s_ses->hosts[h].ip_str, profile_choice, true);
        if (err == ESP_ERR_NOT_SUPPORTED)
        {
            s_scanall_active = false;
            sa_set_info_("SYN not available", SA_SCR_PORT_METHOD);
            return;
        }

        /* open_only => every recorded result is an open port. Capture them. */
        size_t open = s_ses->result_count;
        uint8_t stored = (uint8_t)((open > SA_HOST_PORTS) ? SA_HOST_PORTS : open);
        for (size_t i = 0; i < stored; ++i)
        {
            s_ses->nb->host_open[h].ports[i] = s_ses->sb->results[i].port;
            s_ses->nb->host_open[h].svc_id[i] = 0;
        }
        s_ses->nb->host_open[h].count = stored;
        s_ses->nb->host_open[h].overflow = (open > SA_HOST_PORTS);

        /* Identify services on this host's open ports (compact id each), unless
         * disabled. One socket at a time; cancellable between ports. */
        if (s_services_on)
        {
            for (uint8_t i = 0; i < stored && !s_cancel; ++i)
            {
                poom_security_audit_service_t svc;
                if (poom_security_audit_detect_service(s_ses->hosts[h].ip_str,
                                                    s_ses->nb->host_open[h].ports[i], 0,
                                                    &svc, &s_cancel) == ESP_OK &&
                    svc.evidence >= POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE)
                {
                    s_ses->nb->host_open[h].svc_id[i] = poom_security_audit_service_id(svc.service);
                    /* Unique product/version for Assess All (0 = none). */
                    s_ses->nb->host_open[h].prod[i] = poom_net_audit_intern(
                        s_ses->nb->net_prod, &s_ses->nb->net_prod_count, SA_NET_TERMS,
                        svc.product, svc.version);
                    /* Self-reported OS clues ("... Ubuntu-3ubuntu0.6", "Microsoft-IIS"). */
                    poom_ns_os_from_text(&s_ses->ids[h], svc.banner, POOM_NS_OSEV_CONFIRMED);
                    poom_ns_os_from_text(&s_ses->ids[h], svc.product, POOM_NS_OSEV_CONFIRMED);
                }
            }
        }

        printf("[security_audit] %.15s: %u open\n", s_ses->hosts[h].ip_str, (unsigned)open);
        if (open > 0)
        {
            s_ses->all_hosts_up++;
            s_ses->all_open_total += (uint32_t)open;
        }
    }

    s_scanall_active = false;

    /* Identify: names + OS clues for every discovered host (~3 s), then fold
     * the local clues. Skipped if the scan was cancelled. */
    if (!s_cancel)
    {
        s_prog_done = 0;
        s_prog_total = 1;
        s_id_phase = true;
        sa_draw_();
        /* UPnP results are transient: interned straight into the product
         * table, then freed. */
        poom_security_audit_upnp_t *up = (poom_security_audit_upnp_t *)calloc(12, sizeof(*up));
        size_t nup = 0;
        (void)poom_security_audit_identify(s_ses->hosts, s_ses->host_count, s_ses->ids,
                                        up, (up != NULL) ? 12U : 0U, &nup, &s_cancel);
        for (size_t i = 0; up != NULL && i < nup; ++i)
        {
            if (up[i].host >= s_ses->host_count) { continue; }
            s_ses->nb->dev_prod[up[i].host] = poom_net_audit_intern(
                s_ses->nb->net_prod, &s_ses->nb->net_prod_count, SA_NET_TERMS,
                up[i].model, up[i].version);
            s_ses->nb->dev_port[up[i].host] = up[i].port;
            printf("[security_audit] upnp %.15s: %.15s %.23s %.15s\n",
                   s_ses->hosts[up[i].host].ip_str, up[i].manufacturer, up[i].model, up[i].version);
        }
        free(up);
        s_id_phase = false;
    }
    for (size_t h = 0; h < s_ses->host_count; ++h)
    {
        sa_fold_local_clues_(&s_ses->ids[h], &s_ses->hosts[h],
                             s_ses->nb->host_open[h].ports, s_ses->nb->host_open[h].count);
        s_ses->hosts[h].ttl = s_ses->ids[h].ttl;
        s_ses->hosts[h].ttl_known = s_ses->ids[h].ttl_known;
    }

    /* Read-only web checks on each host's plain-HTTP ports (Wi-Fi still up). */
    if (!s_cancel)
    {
        s_chk_phase = true;
        for (size_t h = 0; h < s_ses->host_count && !s_cancel; ++h)
        {
            const char *names[SA_HOST_PORTS];
            for (uint8_t i = 0; i < s_ses->nb->host_open[h].count; ++i)
            {
                uint8_t sid = s_ses->nb->host_open[h].svc_id[i];
                names[i] = (sid != 0U) ? poom_security_audit_service_by_id(sid) : NULL;
            }
            uint16_t wp[2];
            size_t nw = sa_web_ports_(s_ses->nb->host_open[h].ports, names,
                                      s_ses->nb->host_open[h].count, wp, 2);
            if (nw == 0U) { continue; }
            (void)snprintf(s_scan_ip, sizeof(s_scan_ip), "%.15s", s_ses->hosts[h].ip_str);
            s_prog_done = (uint32_t)h;
            s_prog_total = (uint32_t)s_ses->host_count;
            sa_draw_();
            s_ses->nb->confirmed[h] = sa_run_web_checks_(s_ses->hosts[h].ip, wp, nw,
                                                         &s_ses->nb->web_port[h]);
        }
        s_chk_phase = false;
        sa_log_heap_("web checks done");
    }
    sa_net_exposures_builtin_();
    sa_log_heap_("scan-all done");
    s_ses->net_results = true; /* re-openable from Devices (even if cancelled) */
    s_sel = 0;
    s_result_top = 0;
    s_screen = SA_SCR_NET_SUMMARY;
}

static void sa_run_resolve_ip_(void)
{
    /* kb_text holds the typed IP. Resolve MAC (best-effort) into the detail
     * host; the swept device list is left intact. */
    (void)snprintf(s_ses->target_ip, sizeof(s_ses->target_ip), "%.15s", s_ses->kb_text);

    s_scanall_pending = false; /* specific-IP is always a single-host target */
    poom_security_audit_host_t *h = &s_ses->detail_host;
    memset(h, 0, sizeof(*h));
    (void)snprintf(h->ip_str, sizeof(h->ip_str), "%.15s", s_ses->target_ip);
    h->ip = sa_parse_ip_(s_ses->target_ip);
    s_detail_hi = -1;
    memset(&s_ses->detail_id, 0, sizeof(s_ses->detail_id));
    uint8_t mac[6];
    if (poom_security_audit_resolve_mac(s_ses->target_ip, mac) == ESP_OK)
    {
        memcpy(h->mac, mac, 6);
        h->mac_known = true;
        h->arp_responded = true;
    }
    s_screen = SA_SCR_HOST_DETAIL;
}

static void sa_run_scan_aps_(void)
{
    /* Show a "scanning" screen first: poom_wifi_scanner_scan() blocks a few
     * seconds, which otherwise looks like a freeze on the previous screen. */
    s_screen = SA_SCR_SCAN_APS;
    s_aps_scanning = true;
    sa_draw_();

    (void)poom_wifi_scanner_clear_ap_records();
    esp_err_t err = poom_wifi_scanner_scan();
    s_aps_scanning = false;
    if (err != ESP_OK)
    {
        sa_set_info_("Scan failed", SA_SCR_WIFI);
        return;
    }
    sa_reset_list_();
}

/* ---- connect handling --------------------------------------------------- */

static void sa_request_connect_(const char *ssid, const char *pass)
{
    (void)snprintf(s_ses->edit_ssid, sizeof(s_ses->edit_ssid), "%.32s", ssid ? ssid : "");
    (void)snprintf(s_ses->edit_pass, sizeof(s_ses->edit_pass), "%.63s", pass ? pass : "");
    s_connect_pending = true;
}

static void sa_update_connecting_(void)
{
    if (poom_wifi_ctrl_sta_has_ip())
    {
        sa_after_connect_();
        return;
    }
    if ((sa_now_ms_() - s_connect_start_ms) > SA_CONNECT_TIMEOUT_MS)
    {
        (void)poom_wifi_ctrl_sta_disconnect();
        s_relink_keep = false;
        s_resume = SA_RESUME_NONE;
        sa_set_info_("Connect timeout", SA_SCR_WIFI);
    }
}

/* ---- drawing ------------------------------------------------------------ */

static void sa_draw_title_(const char *t)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(t);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void sa_draw_footer_(const char *t)
{
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(t);
}

/* Draw a scrolling list; returns nothing. Caller supplies a row-text fn. */
typedef void (*sa_row_text_fn)(int idx, char *out, size_t cap);

static void sa_draw_list_(int count, sa_row_text_fn row_fn)
{
    sa_fix_window_(count);
    char line[26];
    char row[28];
    for (uint32_t r = 0; r < SA_VISIBLE_ROWS; ++r)
    {
        int idx = s_list_top + (int)r;
        if (idx >= count) { break; }
        int y = 14 + (int)r * 10;
        bool sel = (idx == s_sel);
        row_fn(idx, line, sizeof(line));
        (void)snprintf(row, sizeof(row), "%s%s", sel ? ">" : " ", line);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(row);
    }
}

/* row-text helpers (read from module state) */
static void sa_row_wifi_(int idx, char *out, size_t cap)
{
    bool up = poom_wifi_ctrl_sta_has_ip();
    const char *const up_items[] = {"Scan Hosts", "Change Net", "Disconnect"};
    const char *const dn_items[] = {"Saved Profiles", "Scan Networks"};
    if (up) { (void)snprintf(out, cap, "%s", up_items[idx]); }
    else    { (void)snprintf(out, cap, "%s", dn_items[idx]); }
}
static int sa_wifi_item_count_(void)
{
    return poom_wifi_ctrl_sta_has_ip() ? 3 : 2;
}

static void sa_row_profile_(int idx, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%.21s", s_ses->profiles[idx].ssid);
}

static void sa_row_ap_(int idx, char *out, size_t cap)
{
    wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record((unsigned)idx);
    if (ap == NULL) { (void)snprintf(out, cap, "?"); return; }
    (void)snprintf(out, cap, "%.17s %d", (const char *)ap->ssid, (int)ap->rssi);
}

/* Action rows at the top of the Devices list. "View Last Results" is
 * appended (after the fixed actions, so their positions never shift) while
 * Scan All / NetAudit data from this session is still valid. */
static int sa_host_act_count_(void)
{
    return SA_HOST_ACT_COUNT + (s_ses->net_results ? 1 : 0);
}

static void sa_row_host_(int idx, char *out, size_t cap)
{
    if (idx < sa_host_act_count_())
    {
        const char *const acts[] = {"Full NetAudit", "Refresh Devices", "Scan All Ports+Svc",
                                    "Manual Target", "View Last Results"};
        (void)snprintf(out, cap, "%s", acts[idx]);
        return;
    }
    int hi = idx - sa_host_act_count_();
    poom_security_audit_host_t *h = &s_ses->hosts[hi];

    /* IP in full, then as much of the manufacturer as fits on the line. The
     * list cursor takes 1 char, so the row text budget is ~20. */
    int used = snprintf(out, cap, "%.15s", h->ip_str);
    if (used < 0) { return; }
    int rem = 20 - used; /* space left for " <vendor>" */
    if (rem < 3) { return; }

    /* Hostname (mDNS / NetBIOS) when known, else the manufacturer. */
    const char *vend = NULL;
    if (s_ses->ids[hi].name[0] != '\0')
    {
        vend = s_ses->ids[hi].name;
    }
    else if (h->mac_known)
    {
        vend = menu_wifi_detector_oui_is_random(h->mac)
                   ? "rnd"
                   : menu_wifi_detector_oui_vendor(h->mac);
    }
    if (vend != NULL && vend[0] != '\0')
    {
        (void)snprintf(out + used, cap - (size_t)used, " %.*s", rem - 1, vend);
    }
}

static void sa_row_method_(int idx, char *out, size_t cap)
{
    if (idx == 0) { (void)snprintf(out, cap, "TCP Connect"); }
    else
    {
        (void)snprintf(out, cap, "TCP SYN%s",
                       poom_security_audit_syn_available() ? "" : " (n/a)");
    }
}

static void sa_row_profchoice_(int idx, char *out, size_t cap)
{
    const char *const p[] = {"Quick (common)", "Extended (~1000)",
                             "Full (1-65535)", "Custom..."};
    (void)snprintf(out, cap, "%s", p[idx]);
}

/* Results focus on OPEN ports (the useful outcome); closed/filtered are rolled
 * into a count summary so the small screen is not flooded with noise. The list
 * has a '>' cursor moved by Up/Down (s_sel indexes open ports). */
static void sa_draw_results_(void)
{
    char l[26];
    /* Only open ports are stored; closed/filtered are counted by the engine. */
    size_t nopen = sa_count_open_();
    unsigned nclosed = (unsigned)s_ses->n_closed, nfilt = (unsigned)s_ses->n_filtered;
    s_ses->open_count = nopen;

    (void)snprintf(l, sizeof(l), "%.10s o:%u", s_ses->target_ip, (unsigned)nopen);
    sa_draw_title_(l);

    /* summary: closed/filtered counts + method */
    (void)snprintf(l, sizeof(l), "cls:%u flt:%u %s", (unsigned)nclosed,
                   (unsigned)nfilt, (s_method == 1) ? "SYN" : "TCPc");
    poom_arduboy_set_cursor(2, 14);
    (void)poom_arduboy_print(l);

    if (nopen == 0)
    {
        poom_arduboy_set_cursor(2, 32);
        (void)poom_arduboy_print(F("No open ports"));
        sa_draw_footer_("B:BACK");
        poom_arduboy_display();
        return;
    }

    /* cursor + 3-row window over the open ports */
    const int rows = 3;
    if (s_sel < 0) { s_sel = 0; }
    if ((size_t)s_sel >= nopen) { s_sel = (int)nopen - 1; }
    if (s_sel < s_result_top) { s_result_top = s_sel; }
    if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
    if (s_result_top < 0) { s_result_top = 0; }

    int k = 0; /* index among open ports */
    for (size_t i = 0; i < s_ses->result_count; ++i)
    {
        if (s_ses->sb->results[i].state != POOM_SECURITY_AUDIT_PORT_OPEN) { continue; }
        if (k >= s_result_top && k < s_result_top + rows)
        {
            int y = 26 + (k - s_result_top) * 10;
            uint16_t port = s_ses->sb->results[i].port;
            /* Prefer a detected service name; fall back to the port guess. */
            poom_security_audit_service_t *sv = sa_find_svc_(port);
            const char *name = (sv != NULL &&
                                sv->evidence >= POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE)
                                   ? sv->service
                                   : poom_security_audit_service_name(port);
            (void)snprintf(l, sizeof(l), "%s%5u %.12s", (k == s_sel) ? ">" : " ",
                           (unsigned)port, name);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        k++;
    }
    sa_draw_footer_("A:det L:assess B:bk");
    poom_arduboy_display();
}

static void sa_draw_progress_(const char *title, const char *action)
{
    sa_draw_title_(title);
    poom_arduboy_set_cursor(2, 20);
    (void)poom_arduboy_print(action);

    uint32_t total = s_prog_total ? s_prog_total : 1;
    uint32_t done = (s_prog_done > total) ? total : s_prog_done;
    int w = (int)((uint32_t)(ARDUBOY_WIDTH - 8) * done / total);
    poom_arduboy_draw_rect(4, 34, ARDUBOY_WIDTH - 8, 10, 1);
    if (w > 0) { poom_arduboy_fill_rect(4, 34, w, 10, 1); }

    char l[24];
    (void)snprintf(l, sizeof(l), "%u / %u", (unsigned)done, (unsigned)total);
    poom_arduboy_set_cursor(4, 46);
    (void)poom_arduboy_print(l);
    sa_draw_footer_("B:CANCEL");
    poom_arduboy_display();
}

/* ---- scrolling text pages (detail screens) ------------------------------
 * Detail screens are built as a list of logical lines, word-wrapped to the
 * screen width, and shown SA_PAGE_ROWS at a time with breathing room. Up/Down
 * scroll one line; ^ / v in the right column mark more above / below. */

#define SA_PAGE_COLS   (20U)   /* text columns (col 21 holds the ^/v marker) */
#define SA_PAGE_MAX    (24U)   /* wrapped lines kept per page                */
#define SA_PAGE_ROWS   (3)     /* lines visible at once                      */
#define SA_PAGE_Y0     (15)
#define SA_PAGE_PITCH  (13)

static char        s_page[SA_PAGE_MAX][SA_PAGE_COLS + 1U];
static uint8_t     s_page_n = 0;
static int         s_page_top = 0;               /* first visible line      */
static sa_screen_t s_last_drawn = SA_SCR_INFO;   /* resets s_page_top on entry */

static void sa_page_begin_(void) { s_page_n = 0; }

/* Append text, word-wrapped; "" adds a blank spacer line. */
static void sa_page_add_(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        if (s_page_n < SA_PAGE_MAX) { s_page[s_page_n++][0] = '\0'; }
        return;
    }
    const char *rest = text;
    while (rest != NULL && s_page_n < SA_PAGE_MAX)
    {
        rest = poom_security_audit_wrap(rest, SA_PAGE_COLS, s_page[s_page_n], sizeof(s_page[0]));
        s_page_n++;
    }
}

static void sa_page_addf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sa_page_addf_(const char *fmt, ...)
{
    char tmp[112];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sa_page_add_(tmp);
}

static void sa_page_draw_(const char *title, const char *footer)
{
    sa_draw_title_(title);
    int max_top = (int)s_page_n - SA_PAGE_ROWS;
    if (max_top < 0) { max_top = 0; }
    if (s_page_top > max_top) { s_page_top = max_top; }
    if (s_page_top < 0) { s_page_top = 0; }

    for (int r = 0; r < SA_PAGE_ROWS; ++r)
    {
        int i = s_page_top + r;
        if (i >= (int)s_page_n) { break; }
        poom_arduboy_set_cursor(2, SA_PAGE_Y0 + r * SA_PAGE_PITCH);
        (void)poom_arduboy_print(s_page[i]);
    }
    if (s_page_top > 0)
    {
        poom_arduboy_set_cursor(122, SA_PAGE_Y0);
        (void)poom_arduboy_print("^");
    }
    if (s_page_top < max_top)
    {
        poom_arduboy_set_cursor(122, SA_PAGE_Y0 + (SA_PAGE_ROWS - 1) * SA_PAGE_PITCH);
        (void)poom_arduboy_print("v");
    }
    sa_draw_footer_(footer);
    poom_arduboy_display();
}

static bool sa_is_page_screen_(sa_screen_t scr)
{
    return scr == SA_SCR_HOST_DETAIL || scr == SA_SCR_PORT_DETAIL || scr == SA_SCR_SS_DETAIL ||
           scr == SA_SCR_VULN_EXPOSURES || scr == SA_SCR_VULN_GAPS || scr == SA_SCR_DEVICE_INFO;
}

/* ---- "what is this device" ------------------------------------------------- */

/* Short identity for host h: hostname > UPnP model > OS/device type >
 * manufacturer. Empty if nothing is known. */
static void sa_host_label_(size_t h, char *out, size_t cap)
{
    out[0] = '\0';
    const poom_security_audit_ident_t *id = &s_ses->ids[h];
    uint8_t dp = s_ses->nb->dev_prod[h];
    const char *vend = sa_vendor_of_(&s_ses->hosts[h]);
    if (id->name[0] != '\0') { (void)snprintf(out, cap, "%s", id->name); }
    else if (dp != 0U && dp != POOM_NET_AUDIT_INTERN_FULL)
    {
        (void)snprintf(out, cap, "%s", s_ses->nb->net_prod[dp - 1U].product);
    }
    else if (id->os != POOM_NS_OS_UNKNOWN) { (void)snprintf(out, cap, "%s", poom_ns_os_label(id->os)); }
    else if (vend != NULL && vend[0] != '\0') { (void)snprintf(out, cap, "%s", vend); }
}

/* Header lines describing the host whose results are shown. */
static void sa_page_add_device_header_(void)
{
    const poom_security_audit_host_t *ho;
    const poom_security_audit_ident_t *id;
    uint8_t dp = 0;
    if (s_proj_host >= 0 && (size_t)s_proj_host < s_ses->host_count)
    {
        ho = &s_ses->hosts[s_proj_host];
        id = &s_ses->ids[s_proj_host];
        dp = s_ses->nb->dev_prod[s_proj_host];
    }
    else
    {
        ho = &s_ses->detail_host;
        id = sa_detail_ident_();
    }
    const char *vend = sa_vendor_of_(ho);
    if (id->name[0] != '\0') { sa_page_addf_("Name: %s", id->name); }
    if (dp != 0U && dp != POOM_NET_AUDIT_INTERN_FULL)
    {
        const poom_net_audit_prodver_t *pv = &s_ses->nb->net_prod[dp - 1U];
        sa_page_addf_("Model: %s %s", pv->product, pv->version);
    }
    if (id->os != POOM_NS_OS_UNKNOWN)
    {
        sa_page_addf_("OS: %s (%s)", poom_ns_os_label(id->os), poom_ns_osev_label(id->os_ev));
    }
    if (vend != NULL && vend[0] != '\0') { sa_page_addf_("Mfr: %s", vend); }
    else if (ho->mac_known && menu_wifi_detector_oui_is_random(ho->mac)) { sa_page_add_("Mfr: (random MAC)"); }
    sa_page_add_("");
}

static const char *sa_tier_label_(uint8_t tier)
{
    switch (tier)
    {
    case POOM_NET_AUDIT_TIER_STRONG: return "STRONG (version named)";
    case POOM_NET_AUDIT_TIER_MEDIUM: return "MEDIUM (other version)";
    default:                         return "WEAK (product only)";
    }
}

static void sa_draw_(void)
{
    char l[26];

    if (s_screen != s_last_drawn)
    {
        s_page_top = 0; /* every page opens at its top */
        s_last_drawn = s_screen;
    }

    switch (s_screen)
    {
    case SA_SCR_WIFI:
    {
        bool up = poom_wifi_ctrl_sta_has_ip();
        int n = sa_wifi_item_count_();
        sa_draw_title_("SECURITY AUDIT");

        poom_arduboy_set_cursor(2, 14);
        if (up)
        {
            sa_load_ssid_();
            (void)snprintf(l, sizeof(l), "On: %.16s", s_ssid[0] ? s_ssid : "(net)");
            (void)poom_arduboy_print(l);
        }
        else
        {
            (void)poom_arduboy_print(F("Not connected"));
        }

        /* Small menu below the status line: 3 rows visible, scrolls. */
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= n) { s_sel = n - 1; }
        if (s_sel < s_list_top) { s_list_top = s_sel; }
        if (s_sel >= s_list_top + 3) { s_list_top = s_sel - 2; }
        if (s_list_top > n - 3) { s_list_top = (n > 3) ? n - 3 : 0; }
        if (s_list_top < 0) { s_list_top = 0; }
        for (int i = s_list_top; i < n && i < s_list_top + 3; ++i)
        {
            int y = 26 + (i - s_list_top) * 10;
            char row[28];
            sa_row_wifi_(i, l, sizeof(l));
            (void)snprintf(row, sizeof(row), "%s%s", (i == s_sel) ? ">" : " ", l);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(row);
        }
        sa_draw_footer_("A:OK B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_PROFILES:
        sa_draw_title_("SAVED PROFILES");
        (void)sa_profiles_();
        if (s_ses->profile_count == 0)
        {
            poom_arduboy_set_cursor(2, 26);
            (void)poom_arduboy_print(F("(none saved)"));
        }
        else
        {
            sa_draw_list_((int)s_ses->profile_count, sa_row_profile_);
        }
        sa_draw_footer_("A:connect B:back");
        poom_arduboy_display();
        break;

    case SA_SCR_SCAN_APS:
    {
        sa_draw_title_("SCAN NETWORKS");
        if (s_aps_scanning)
        {
            poom_arduboy_set_cursor(2, 28);
            (void)poom_arduboy_print(F("Scanning for"));
            poom_arduboy_set_cursor(2, 38);
            (void)poom_arduboy_print(F("networks..."));
            poom_arduboy_display();
            break;
        }
        poom_wifi_scanner_ap_records_t *recs = poom_wifi_scanner_get_ap_records();
        int n = (recs != NULL) ? (int)recs->count : 0;
        if (n == 0)
        {
            poom_arduboy_set_cursor(2, 26);
            (void)poom_arduboy_print(F("No APs"));
        }
        else
        {
            sa_draw_list_(n, sa_row_ap_);
        }
        sa_draw_footer_("A:pick B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_PASSWORD:
    case SA_SCR_SPECIFIC_IP:
    case SA_SCR_PORT_CUSTOM:
        poom_arduboy_clear();
        poom_ui_keyboard_draw(&s_ses->kb);
        poom_arduboy_display();
        break;

    case SA_SCR_CONNECTING:
        sa_draw_title_("CONNECTING");
        poom_arduboy_set_cursor(2, 24);
        (void)snprintf(l, sizeof(l), "%.20s", s_ses->edit_ssid);
        (void)poom_arduboy_print(l);
        poom_arduboy_set_cursor(2, 36);
        (void)poom_arduboy_print(poom_wifi_ctrl_sta_is_connected()
                                     ? F("Got link, DHCP...") : F("Associating..."));
        sa_draw_footer_("B:CANCEL");
        poom_arduboy_display();
        break;

    case SA_SCR_HOSTS:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "DEVICES (%u)", (unsigned)s_ses->host_count);
        sa_draw_title_(t);
        int total = sa_host_act_count_() + (int)s_ses->host_count;
        sa_draw_list_(total, sa_row_host_);
        sa_draw_footer_("A:sel B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_SWEEPING:
        sa_draw_progress_(s_audit_running ? "NETAUDIT 1/3" : "PING SWEEP", "Discovering...");
        break;

    case SA_SCR_HOST_DETAIL:
    {
        poom_security_audit_host_t *h = &s_ses->detail_host;
        const char *vend = NULL;
        bool rnd = h->mac_known && menu_wifi_detector_oui_is_random(h->mac);
        if (h->mac_known && !rnd) { vend = menu_wifi_detector_oui_vendor(h->mac); }

        sa_page_begin_();
        sa_page_addf_("IP: %.15s", s_ses->target_ip);
        if (h->mac_known)
        {
            sa_page_addf_("MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                          h->mac[0], h->mac[1], h->mac[2], h->mac[3], h->mac[4], h->mac[5]);
        }
        else
        {
            sa_page_add_("MAC: unknown");
        }
        if (rnd) { sa_page_add_("Mfr: (random MAC)"); }
        else     { sa_page_addf_("Mfr: %s", (vend != NULL && vend[0]) ? vend : "unknown"); }

        /* Identity from the last scan's identify pass (name, OS + how sure,
         * TTL). Before any scan only the vendor guess is available. */
        poom_security_audit_ident_t id = *sa_detail_ident_();
        if (id.os == POOM_NS_OS_UNKNOWN) { poom_ns_os_from_vendor(&id, vend); }
        if (id.name[0] != '\0') { sa_page_addf_("Name: %s", id.name); }
        if (id.os != POOM_NS_OS_UNKNOWN)
        {
            sa_page_addf_("OS: %s", poom_ns_os_label(id.os));
            sa_page_addf_("  (%s)", poom_ns_osev_label(id.os_ev));
        }
        else
        {
            sa_page_add_("OS: unknown");
        }
        if (id.ttl_known) { sa_page_addf_("Ping TTL: %u", (unsigned)id.ttl); }

        sa_page_draw_("HOST", "A:Port Scan  B:back");
        break;
    }

    case SA_SCR_PORT_METHOD:
        sa_draw_title_("SCAN METHOD");
        sa_draw_list_(2, sa_row_method_);
        sa_draw_footer_("A:ok B:back");
        poom_arduboy_display();
        break;

    case SA_SCR_PORT_PROFILE:
    {
        sa_draw_title_("PORT RANGE");
        sa_draw_list_(4, sa_row_profchoice_);
        char f[26];
        if (s_audit_flow)
        {
            (void)snprintf(f, sizeof(f), "NetAudit A:next");
        }
        else
        {
            (void)snprintf(f, sizeof(f), "Svc:%s(L) A:start",
                           s_services_on ? "ON" : "OFF");
        }
        sa_draw_footer_(f);
        poom_arduboy_display();
        break;
    }

    case SA_SCR_SCANNING:
        if (s_chk_phase)
        {
            sa_draw_progress_(s_audit_running ? "AUDIT 2/3 CHK" : "WEB CHECKS", s_scan_ip);
        }
        else if (s_id_phase)
        {
            sa_draw_progress_(s_audit_running ? "AUDIT 2/3 ID" : "IDENTIFY", "Names & OS...");
        }
        else if (s_svc_phase)
        {
            sa_draw_progress_("SERVICES", "Identifying...");
        }
        else if (s_scanall_active)
        {
            char t[26];
            /* host counts are <= SA_MAX_HOSTS; bound them for the 26-byte title */
            (void)snprintf(t, sizeof(t), "%s %u/%u", s_audit_running ? "AUDIT 2/3" : "ALL",
                           (unsigned)(uint8_t)(s_prog_done + 1), (unsigned)(uint8_t)s_prog_total);
            sa_draw_progress_(t, s_scan_ip);
        }
        else
        {
            sa_draw_progress_("PORT SCAN", "Scanning host...");
        }
        break;

    case SA_SCR_RESULTS:
        sa_draw_results_();
        break;

    case SA_SCR_NET_SUMMARY:
    {
        sa_draw_title_("NET SUMMARY");
        (void)snprintf(l, sizeof(l), "Up:%u/%u  open:%u",
                       (unsigned)s_ses->all_hosts_up, (unsigned)s_ses->host_count,
                       (unsigned)s_ses->all_open_total);
        poom_arduboy_set_cursor(2, 14);
        (void)poom_arduboy_print(l);

        int nhosts = sa_open_host_count_();
        if (nhosts == 0)
        {
            poom_arduboy_set_cursor(2, 32);
            (void)poom_arduboy_print(F("No open ports found"));
            sa_draw_footer_("B:back");
            poom_arduboy_display();
            break;
        }

        const int rows = 3;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= nhosts) { s_sel = nhosts - 1; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
        if (s_result_top < 0) { s_result_top = 0; }

        for (int r = 0; r < rows; ++r)
        {
            int n = s_result_top + r;
            if (n >= nhosts) { break; }
            int h = sa_nth_open_host_(n);
            if (h < 0) { break; }
            int y = 26 + r * 10;
            (void)snprintf(l, sizeof(l), "%s%.15s (%u)", (n == s_sel) ? ">" : " ",
                           s_ses->hosts[h].ip_str,
                           (unsigned)s_ses->nb->host_open[h].count);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:view L:assess B:bk");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_HOST_OPEN:
    {
        int h = s_open_host;
        int cnt = s_ses->nb->host_open[h].count;
        (void)snprintf(l, sizeof(l), "%.10s o:%u%s", s_ses->hosts[h].ip_str,
                       (unsigned)cnt, s_ses->nb->host_open[h].overflow ? "+" : "");
        sa_draw_title_(l);

        const int rows = 4;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= cnt) { s_sel = (cnt > 0) ? cnt - 1 : 0; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
        if (s_result_top < 0) { s_result_top = 0; }

        for (int r = 0; r < rows; ++r)
        {
            int i = s_result_top + r;
            if (i >= cnt) { break; }
            uint16_t port = s_ses->nb->host_open[h].ports[i];
            uint8_t sid = s_ses->nb->host_open[h].svc_id[i];
            /* detected service (compact id) if we have one, else port guess */
            const char *name = (sid != 0) ? poom_security_audit_service_by_id(sid)
                                          : poom_security_audit_service_name(port);
            int y = 14 + r * 10;
            (void)snprintf(l, sizeof(l), "%s%5u %.12s", (i == s_sel) ? ">" : " ",
                           (unsigned)port, name);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:detail B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_PORT_DETAIL:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "PORT %u", (unsigned)s_detail_port);
        if (s_detecting)
        {
            sa_page_begin_();
            sa_page_addf_("Host: %.15s", s_detail_ip);
            sa_page_add_("");
            sa_page_add_("Detecting service...");
            sa_page_draw_(t, "B:back");
            break;
        }

        const poom_security_audit_service_t *sv = &s_ses->detail_svc;
        bool got = s_ses->detail_svc_valid && sv->port == s_detail_port;
        bool confirmed = got && sv->evidence >= POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;

        sa_page_begin_();
        sa_page_addf_("Host: %.15s", s_detail_ip);
        sa_page_addf_("Port: %u/tcp %s", (unsigned)s_detail_port,
                      poom_security_audit_state_label(s_detail_state));
        if (confirmed)
        {
            sa_page_addf_("Service: %s", sv->service);
        }
        else
        {
            sa_page_addf_("Service: %s?", poom_security_audit_service_name(s_detail_port));
            sa_page_add_("(guess from port no.)");
        }
        if (got && sv->product[0]) { sa_page_addf_("Product: %s", sv->product); }
        if (got && sv->version[0]) { sa_page_addf_("Version: %s", sv->version); }
        if (got)
        {
            sa_page_addf_("Evidence: %s", poom_security_audit_evidence_label(sv->evidence));
        }
        if (got && sv->banner[0])
        {
            sa_page_add_("Banner:");
            sa_page_add_(sv->banner);
        }
        sa_page_draw_(t, got ? "A:redetect  B:back" : "A:detect  B:back");
        break;
    }

    case SA_SCR_VULN_SUMMARY:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "%.11s ASSESS", s_ses->target_ip);
        sa_draw_title_(t);
        const char *const rows3[4] = {"Potential", "No match", "Unassessed", "Exposures"};
        unsigned nexp = 0;
        nexp = sa_popcount_(s_cur_exp) + sa_popcount_(s_cur_conf);
        unsigned cnt3[4] = {s_ses->as->vuln_potential, s_ses->as->vuln_nomatch,
                            s_ses->as->vuln_unassessed, nexp};
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel > 3) { s_sel = 3; }
        for (int i = 0; i < 4; ++i)
        {
            int y = 14 + i * 10;
            (void)snprintf(l, sizeof(l), "%s%-11s (%u)", (i == s_sel) ? ">" : " ",
                           rows3[i], cnt3[i] % 100U);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:open B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_VULN_POTENTIAL:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "%.15s", s_ses->target_ip);
        sa_draw_title_(t);
        /* what the device is, right under the IP */
        if (s_proj_host >= 0)
        {
            char lab[POOM_NS_NAME_CAP + 8];
            sa_host_label_((size_t)s_proj_host, lab, sizeof(lab));
            poom_arduboy_set_cursor(2, 13);
            (void)snprintf(l, sizeof(l), "%.21s", lab[0] ? lab : "(unidentified)");
            (void)poom_arduboy_print(l);
        }
        const char *const opt[3] = {"By exploit", "By service", "Device info"};
        int nopt = (s_proj_host >= 0) ? 3 : 2;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= nopt) { s_sel = nopt - 1; }
        for (int i = 0; i < nopt; ++i)
        {
            int y = 25 + i * 10;
            (void)snprintf(l, sizeof(l), "%s%s", (i == s_sel) ? ">" : " ", opt[i]);
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:open B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_VULN_SERVICES:
    {
        sa_draw_title_("BY SERVICE");
        int n = s_ses->as->ss_n_terms;
        const int rows = 4;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= n) { s_sel = (n > 0) ? n - 1 : 0; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
        if (s_result_top < 0) { s_result_top = 0; }
        for (int r = 0; r < rows; ++r)
        {
            int i = s_result_top + r;
            if (i >= n) { break; }
            int y = 14 + r * 10;
            uint8_t svc = s_ses->as->ss_term_svc[i];
            (void)snprintf(l, sizeof(l), "%s%-12.12s x%d", (i == s_sel) ? ">" : " ",
                           s_ses->as->svcs[svc].product, sa_term_match_count_((uint8_t)i));
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:exploits B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_VULN_GAPS:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "%.15s %s", s_ses->target_ip, s_gaps_mode ? "UNID" : "NO HIT");
        sa_page_begin_();
        sa_page_add_device_header_();
        sa_page_add_(s_gaps_mode ? "Open, nothing identified:" : "Identified, no exploit:");
        int shown = 0;
        for (size_t i = 0; i < s_ses->as->svc_count; ++i)
        {
            const poom_security_audit_service_t *sv = &s_ses->as->svcs[i];
            bool has_product = (sv->product[0] != '\0');
            bool include;
            if (s_gaps_mode) { include = !has_product; }       /* unassessed */
            else                                                /* no-match: product svc with 0 hits */
            {
                include = false;
                if (has_product)
                {
                    for (uint8_t t2 = 0; t2 < s_ses->as->ss_n_terms; ++t2)
                    {
                        if (s_ses->as->ss_term_svc[t2] == i)
                        {
                            include = (sa_term_match_count_(t2) == 0);
                            break;
                        }
                    }
                }
            }
            if (!include) { continue; }
            if (has_product)
            {
                sa_page_addf_("%u %s %s %s", (unsigned)sv->port, sv->service, sv->product, sv->version);
            }
            else
            {
                sa_page_addf_("%u %s", (unsigned)sv->port, sv->service);
            }
            shown++;
        }
        if (shown == 0) { sa_page_add_("(none)"); }
        sa_page_draw_(t, "U/D:scroll  B:back");
        break;
    }

    case SA_SCR_SS_RESULTS:
    {
        if (s_ss_searching)
        {
            sa_draw_progress_("ASSESSING", s_idx_building ? "Building index (once)"
                                                          : "Exploit DB scan");
            break;
        }
        const char *const tiers[4] = {"ALL", "STRONG", "MEDIUM", "WEAK"};
        char t[26];
        (void)snprintf(t, sizeof(t), "%.15s %.6s", s_ses->target_ip, tiers[s_ss_tier_filter]);
        sa_draw_title_(t);
        int cnt = (int)s_ses->as->ss_disp_count;
        const int rows = 4;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= cnt) { s_sel = (cnt > 0) ? cnt - 1 : 0; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
        if (s_result_top < 0) { s_result_top = 0; }
        if (cnt == 0)
        {
            poom_arduboy_set_cursor(2, 28);
            (void)poom_arduboy_print(F("(none in this tier)"));
        }
        for (int r = 0; r < rows; ++r)
        {
            int i = s_result_top + r;
            if (i >= cnt) { break; }
            poom_net_audit_match_t *m = &s_ses->as->ss[s_ses->as->ss_disp[i].idx];
            int y = 14 + r * 10;
            const char *label = (m->cve[0] != '\0') ? m->cve : m->title;
            if (s_ses->as->ss_disp[i].count > 1)
            {
                (void)snprintf(l, sizeof(l), "%s%.14s x%u", (i == s_sel) ? ">" : " ",
                               label, (unsigned)s_ses->as->ss_disp[i].count);
            }
            else
            {
                (void)snprintf(l, sizeof(l), "%s%.18s", (i == s_sel) ? ">" : " ", label);
            }
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:view L/R:tier B:bk");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_SS_DETAIL:
    {
        poom_net_audit_match_t *m = &s_ses->as->ss[s_ses->as->ss_disp[s_ss_sel].idx];
        const poom_security_audit_service_t *sv = &s_ses->as->svcs[s_ses->as->ss_term_svc[m->term_id]];
        char t[26];
        (void)snprintf(t, sizeof(t), "EDB-%.8s", m->edb_id);

        sa_page_begin_();
        sa_page_add_(m->title);
        sa_page_add_("");
        sa_page_addf_("Service: %u %s", (unsigned)sv->port, sv->product);
        if (sv->version[0]) { sa_page_addf_("Version: %s", sv->version); }
        sa_page_addf_("Type: %s", m->type[0] ? m->type : "-");
        sa_page_addf_("Platform: %s", m->platform[0] ? m->platform : "-");
        sa_page_addf_("CVE: %s", m->cve[0] ? m->cve : "none listed");
        if (poom_net_audit_platform_fit(m->platform, s_cur_os_class) == POOM_NET_AUDIT_FIT_YES)
        {
            sa_page_add_("Platform fits host OS");
        }
        sa_page_add_("Match:");
        sa_page_add_(sa_tier_label_(m->tier));
        sa_page_add_("");
        sa_page_add_("POTENTIAL lead only - verify before acting.");

        sa_page_draw_(t, "U/D:scroll  B:back");
        break;
    }

    case SA_SCR_NET_VULN:
    {
        if (s_ss_searching)
        {
            sa_draw_progress_(s_audit_running ? "NETAUDIT 3/3" : "ASSESS ALL",
                              s_idx_building ? "Building index (once)" : "Exploit DB scan");
            break;
        }
        sa_draw_title_(s_net_partial ? "NET VULN (partial)" : "NET VULN SUMMARY");
        const char *const rows3[4] = {"Potential", "No match", "Unassessed", "Exposures"};
        uint8_t cnt4[4] = {s_ses->nb->net_cls_count[0], s_ses->nb->net_cls_count[1],
                           s_ses->nb->net_cls_count[2], s_ses->nb->net_exp_hosts};
        /* 4 categories + "Save report"; 4 rows visible, scrolls */
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel > 4) { s_sel = 4; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + 4) { s_result_top = s_sel - 3; }
        if (s_result_top < 0) { s_result_top = 0; }
        for (int i = s_result_top; i < 5 && i < s_result_top + 4; ++i)
        {
            int y = 14 + (i - s_result_top) * 10;
            if (i < 4)
            {
                (void)snprintf(l, sizeof(l), "%s%-11s %u hst", (i == s_sel) ? ">" : " ",
                               rows3[i], (unsigned)cnt4[i]);
            }
            else
            {
                (void)snprintf(l, sizeof(l), "%sSave report", (i == s_sel) ? ">" : " ");
            }
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_("A:hosts B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_NET_VULN_HOSTS:
    {
        const char *const titles[4] = {"POTENTIAL HOSTS", "NO MATCH HOSTS", "UNASSESSED HOSTS",
                                       "EXPOSED HOSTS"};
        sa_draw_title_(titles[(s_net_cat < 4) ? s_net_cat : 2]);
        int cnt = 0;
        (void)sa_net_cat_host_(-1, &cnt);
        const int rows = 4;
        if (s_sel < 0) { s_sel = 0; }
        if (s_sel >= cnt) { s_sel = (cnt > 0) ? cnt - 1 : 0; }
        if (s_sel < s_result_top) { s_result_top = s_sel; }
        if (s_sel >= s_result_top + rows) { s_result_top = s_sel - rows + 1; }
        if (s_result_top < 0) { s_result_top = 0; }
        if (cnt == 0)
        {
            poom_arduboy_set_cursor(2, 28);
            (void)poom_arduboy_print(F("(none)"));
        }
        for (int r = 0; r < rows; ++r)
        {
            int n = s_result_top + r;
            if (n >= cnt) { break; }
            int h = sa_net_cat_host_(n, NULL);
            if (h < 0) { break; }
            int y = 14 + r * 10;
            /* Potential: services with leads; others: open-port count. */
            unsigned v;
            if (s_net_cat == 0) { v = sa_host_hit_svcs_((size_t)h); }
            else if (s_net_cat == 3)
            {
                v = 0;
                v = sa_popcount_(s_ses->nb->exposures[h]) + sa_popcount_(s_ses->nb->confirmed[h]);
            }
            else { v = (unsigned)s_ses->nb->host_open[h].count; }
            /* ".96 SRS-ZR5  !1": last octet + what the device is (full IP in
             * the next screen's title); plain IP when nothing is known. */
            char lab[POOM_NS_NAME_CAP + 8];
            sa_host_label_((size_t)h, lab, sizeof(lab));
            const char *mark = (s_net_cat == 0 || s_net_cat == 3) ? "!" : "o";
            if (lab[0] != '\0')
            {
                (void)snprintf(l, sizeof(l), "%s.%u %-12.12s %s%u", (n == s_sel) ? ">" : " ",
                               (unsigned)(s_ses->hosts[h].ip & 0xFFU), lab, mark, v % 100U);
            }
            else
            {
                (void)snprintf(l, sizeof(l), "%s%.15s %s%u", (n == s_sel) ? ">" : " ",
                               s_ses->hosts[h].ip_str, mark, v % 100U);
            }
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(l);
        }
        sa_draw_footer_(cnt ? "A:open B:back" : "B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_AUDIT_CONFIRM:
    {
        const char *const rng[4] = {"Quick", "Extended", "Full", "Custom"};
        char d[12];
        sa_draw_title_("FULL NETAUDIT");
        poom_arduboy_set_cursor(2, 13);
        (void)snprintf(l, sizeof(l), "Range: %.8s %up", rng[s_audit_profile & 3U],
                       (unsigned)(uint16_t)((s_audit_ports_n > 65535U) ? 65535U : s_audit_ports_n));
        (void)poom_arduboy_print(l);
        poom_arduboy_set_cursor(2, 23);
        if (s_ses->host_count > 0)
        {
            (void)snprintf(l, sizeof(l), "Hosts: %u (last)", (unsigned)s_ses->host_count);
        }
        else
        {
            (void)snprintf(l, sizeof(l), "Hosts: <=%u (new)", (unsigned)s_ses->subnet.host_count);
        }
        (void)poom_arduboy_print(l);
        poom_arduboy_set_cursor(2, 33);
        (void)poom_arduboy_print(F("Disc>Ports+Svc>EDB"));
        poom_security_audit_fmt_duration(sa_audit_estimate_s_(), d, sizeof(d));
        poom_arduboy_set_cursor(2, 43);
        (void)snprintf(l, sizeof(l), "Est: up to ~%.6s", d);
        (void)poom_arduboy_print(l);
        sa_draw_footer_("A:start B:back");
        poom_arduboy_display();
        break;
    }

    case SA_SCR_VULN_EXPOSURES:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "%.11s EXPOSED", s_ses->target_ip);
        sa_page_begin_();
        sa_page_add_device_header_();
        if (s_cur_exp == 0U && s_cur_conf == 0U) { sa_page_add_("No exposures observed."); }
        if (s_cur_conf != 0U)
        {
            /* Seen directly: the device returned the page (read-only GET). */
            sa_page_addf_("CONFIRMED (port %u):", (unsigned)s_cur_web_port);
            for (uint8_t c = 0; c < poom_net_audit_check_count(); ++c)
            {
                if (((s_cur_conf >> c) & 1U) == 0U) { continue; }
                const poom_net_audit_check_t *ck = poom_net_audit_check(c);
                sa_page_addf_("- %s", ck->title);
                sa_page_addf_("GET %s", ck->path);
                sa_page_add_(ck->detail);
            }
            sa_page_add_("");
        }
        /* Weaknesses first, then the informational ones - no symbol legend. */
        for (int pass = 0; pass < 2; ++pass)
        {
            bool want = (pass == 0);
            bool any = false;
            for (uint8_t b = 0; b < sa_rule_count_(); ++b)
            {
                if (((s_cur_exp >> b) & 1UL) == 0U || sa_rule_detected_(b) != want) { continue; }
                if (!any) { sa_page_add_(want ? "WEAKNESSES FOUND:" : "GOOD TO KNOW:"); any = true; }
                sa_page_addf_("- %s", sa_rule_title_(b));
                sa_page_add_(sa_rule_detail_(b));
            }
            if (any) { sa_page_add_(""); }
        }
        sa_page_draw_(t, "U/D:scroll  B:back");
        break;
    }

    case SA_SCR_DEVICE_INFO:
    {
        char t[26];
        (void)snprintf(t, sizeof(t), "%.15s", s_ses->target_ip);
        sa_page_begin_();
        sa_page_add_device_header_();
        if (s_proj_host >= 0)
        {
            const poom_security_audit_host_t *ho = &s_ses->hosts[s_proj_host];
            if (ho->mac_known)
            {
                sa_page_addf_("MAC: %02X:%02X:%02X:%02X:%02X:%02X", ho->mac[0], ho->mac[1],
                              ho->mac[2], ho->mac[3], ho->mac[4], ho->mac[5]);
            }
            if (s_ses->ids[s_proj_host].ttl_known)
            {
                sa_page_addf_("Ping TTL: %u", (unsigned)s_ses->ids[s_proj_host].ttl);
            }
            sa_page_add_("");
        }
        sa_page_add_("Services:");
        for (size_t i = 0; i < s_ses->as->svc_count; ++i)
        {
            const poom_security_audit_service_t *sv = &s_ses->as->svcs[i];
            sa_page_addf_("%u %s %s %s", (unsigned)sv->port, sv->service, sv->product, sv->version);
        }
        if (s_ses->as->svc_count == 0U) { sa_page_add_("(none open)"); }
        sa_page_draw_(t, "U/D:scroll  B:back");
        break;
    }

    case SA_SCR_SAVE_CONFIRM:
        sa_draw_title_("SAVE NETWORK?");
        poom_arduboy_set_cursor(2, 18);
        (void)snprintf(l, sizeof(l), "%.20s", s_ses->edit_ssid);
        (void)poom_arduboy_print(l);
        poom_arduboy_set_cursor(2, 30);
        (void)poom_arduboy_print(F("Save to profiles?"));
        sa_draw_footer_("A:SAVE   B:SKIP");
        poom_arduboy_display();
        break;

    case SA_SCR_INFO:
    default:
        sa_draw_title_("SECURITY AUDIT");
        poom_arduboy_set_cursor(2, 28);
        (void)poom_arduboy_print(s_info);
        sa_draw_footer_("A/B:ok");
        poom_arduboy_display();
        break;
    }
}

/* ---- OSK entry helpers -------------------------------------------------- */

static void sa_enter_password_(const char *ssid)
{
    (void)snprintf(s_ses->edit_ssid, sizeof(s_ses->edit_ssid), "%.32s", ssid);
    s_ses->edit_pass[0] = '\0';
    s_ses->kb_text[0] = '\0';
    poom_ui_keyboard_init(&s_ses->kb, s_ses->kb_text, sizeof(s_ses->kb_text), s_ses->edit_ssid);
    s_ses->offer_save = true;
    s_screen = SA_SCR_PASSWORD;
}

static void sa_enter_specific_ip_(void)
{
    /* Prefill the local subnet prefix (e.g. "192.168.1.") so the user usually
     * just types the last octet; still fully editable for an off-subnet target.
     * The keyboard puts the cursor at the end of the prefilled text. */
    uint32_t ip = s_ses->subnet.ip;
    if (ip != 0)
    {
        (void)snprintf(s_ses->kb_text, sizeof(s_ses->kb_text), "%u.%u.%u.",
                       (unsigned)((ip >> 24) & 0xFFU),
                       (unsigned)((ip >> 16) & 0xFFU),
                       (unsigned)((ip >> 8) & 0xFFU));
    }
    else
    {
        s_ses->kb_text[0] = '\0';
    }
    poom_ui_keyboard_init(&s_ses->kb, s_ses->kb_text, sizeof(s_ses->kb_text), "IP addr");
    s_screen = SA_SCR_SPECIFIC_IP;
}

static void sa_enter_custom_ports_(void)
{
    s_ses->kb_text[0] = '\0';
    poom_ui_keyboard_init(&s_ses->kb, s_ses->kb_text, sizeof(s_ses->kb_text), "ports");
    s_screen = SA_SCR_PORT_CUSTOM;
}

/* ---- button handling ---------------------------------------------------- */

static bool sa_is_long_press_(uint8_t event)
{
    return (event == BUTTON_LONG_PRESS_START) ||
           (event == BUTTON_LONG_PRESS_HOLD) ||
           (event == BUTTON_LONG_PRESS_UP);
}

static void sa_handle_back_(void)
{
    if (s_audit_running)
    {
        /* NetAudit pipeline: B cancels the current phase (and skips the rest);
         * no navigation until the worker settles on a results screen. */
        s_audit_cancel = true;
        s_cancel = true;
        return;
    }

    switch (s_screen)
    {
    case SA_SCR_WIFI:
        s_exit_requested = true;
        break;
    case SA_SCR_PROFILES:
    case SA_SCR_SCAN_APS:
        s_screen = SA_SCR_WIFI;
        sa_reset_list_();
        break;
    case SA_SCR_PASSWORD:
        s_screen = SA_SCR_SCAN_APS;
        break;
    case SA_SCR_CONNECTING:
        (void)poom_wifi_ctrl_sta_disconnect();
        s_relink_keep = false;
        s_resume = SA_RESUME_NONE;
        s_screen = SA_SCR_WIFI;
        break;
    case SA_SCR_HOSTS:
        s_screen = SA_SCR_WIFI;
        sa_reset_list_();
        break;
    case SA_SCR_SWEEPING:
    case SA_SCR_SCANNING:
        s_cancel = true; /* cooperative cancel; worker returns to a screen */
        break;
    case SA_SCR_HOST_DETAIL:
    case SA_SCR_SPECIFIC_IP:
        s_screen = SA_SCR_HOSTS;
        s_sel = s_host_sel; /* land back on the device you had selected */
        break;
    case SA_SCR_PORT_PROFILE:
        /* PORT_PROFILE is the entry of the scan-config flow (TCP SYN removed, so
         * there is no method screen). Return to where it was entered from. */
        if (s_audit_flow)
        {
            s_audit_flow = false;
            s_screen = SA_SCR_HOSTS;
            s_sel = s_host_sel;
        }
        else if (s_scanall_pending)
        {
            s_scanall_pending = false;
            s_screen = SA_SCR_HOSTS;
            sa_reset_list_();
        }
        else
        {
            s_screen = SA_SCR_HOST_DETAIL;
        }
        break;
    case SA_SCR_PORT_CUSTOM:
        s_screen = SA_SCR_PORT_PROFILE;
        break;
    case SA_SCR_AUDIT_CONFIRM:
        s_screen = SA_SCR_PORT_PROFILE; /* change the range */
        s_sel = s_audit_profile;
        break;
    case SA_SCR_RESULTS:
    case SA_SCR_NET_SUMMARY:
        s_screen = SA_SCR_HOSTS;
        s_sel = s_host_sel; /* return to the device / action you launched from */
        break;
    case SA_SCR_HOST_OPEN:
        s_screen = SA_SCR_NET_SUMMARY;
        s_sel = s_summary_sel; /* return to the host row we opened */
        s_result_top = 0;
        break;
    case SA_SCR_PORT_DETAIL:
        s_screen = s_detail_return; /* keep cursor where it was */
        break;
    case SA_SCR_VULN_SUMMARY:
        if (s_assess_net)
        {
            /* a host projected from Assess All: back to that category's list */
            s_screen = SA_SCR_NET_VULN_HOSTS;
            s_sel = s_net_host_sel;
        }
        else
        {
            s_screen = SA_SCR_RESULTS;
            s_sel = 0;
        }
        s_result_top = 0;
        break;
    case SA_SCR_NET_VULN:
        if (s_ss_searching) { s_cancel = true; } /* cancel -> partial results */
        else
        {
            s_screen = SA_SCR_NET_SUMMARY;
            s_sel = s_summary_sel;
            s_result_top = 0;
        }
        break;
    case SA_SCR_NET_VULN_HOSTS:
        s_screen = SA_SCR_NET_VULN;
        s_sel = s_net_cat;
        s_result_top = 0;
        break;
    case SA_SCR_VULN_POTENTIAL:
        if (s_net_direct)
        {
            s_screen = SA_SCR_NET_VULN_HOSTS; /* back to the host list */
            s_sel = s_net_host_sel;
            s_result_top = 0;
        }
        else
        {
            s_screen = SA_SCR_VULN_SUMMARY;
            s_sel = 0;
        }
        break;
    case SA_SCR_DEVICE_INFO:
        s_screen = SA_SCR_VULN_POTENTIAL;
        s_sel = 2;
        break;
    case SA_SCR_VULN_SERVICES:
        s_screen = SA_SCR_VULN_POTENTIAL;
        s_sel = 1; /* "By service" row */
        break;
    case SA_SCR_VULN_GAPS:
    case SA_SCR_VULN_EXPOSURES:
        if (s_net_direct)
        {
            s_screen = SA_SCR_NET_VULN_HOSTS; /* back to the host list */
            s_sel = s_net_host_sel;
            s_result_top = 0;
        }
        else
        {
            s_sel = (s_screen == SA_SCR_VULN_EXPOSURES) ? 3 : (s_gaps_mode ? 2 : 1);
            s_screen = SA_SCR_VULN_SUMMARY;
        }
        break;
    case SA_SCR_SS_RESULTS:
        if (s_ss_searching) { s_cancel = true; } /* cancel the DB scan */
        else if (s_ss_service_filter >= 0) { s_screen = SA_SCR_VULN_SERVICES; s_sel = s_ss_service_filter; }
        else { s_screen = SA_SCR_VULN_POTENTIAL; s_sel = 0; }
        break;
    case SA_SCR_SS_DETAIL:
        s_screen = SA_SCR_SS_RESULTS;
        s_sel = s_ss_sel; /* back to the match we viewed */
        break;
    case SA_SCR_SAVE_CONFIRM:
        s_ses->offer_save = false;   /* B = skip saving */
        s_screen = SA_SCR_HOSTS;
        sa_reset_list_();
        break;
    case SA_SCR_INFO:
        s_screen = s_info_return;
        break;
    default:
        s_screen = SA_SCR_WIFI;
        break;
    }
}

static void sa_handle_select_(void)
{
    switch (s_screen)
    {
    case SA_SCR_WIFI:
        if (poom_wifi_ctrl_sta_has_ip())
        {
            if (s_sel == 0) /* Scan Hosts */
            {
                if (poom_security_audit_subnet_current(&s_ses->subnet) != ESP_OK)
                {
                    sa_set_info_("No subnet", SA_SCR_WIFI);
                }
                else { s_screen = SA_SCR_HOSTS; sa_reset_list_(); }
            }
            else if (s_sel == 1) /* Change Net */
            {
                s_screen = SA_SCR_PROFILES; sa_reset_list_();
            }
            else /* Disconnect */
            {
                (void)poom_wifi_ctrl_sta_disconnect();
                sa_set_info_("Disconnected", SA_SCR_WIFI);
            }
        }
        else
        {
            if (s_sel == 0) { s_screen = SA_SCR_PROFILES; sa_reset_list_(); }
            else            { s_scan_aps_pending = true; }
        }
        break;

    case SA_SCR_PROFILES:
        if (sa_profiles_() != NULL && s_ses->profile_count > 0)
        {
            s_ses->offer_save = false;
            sa_request_connect_(s_ses->profiles[s_sel].ssid, s_ses->profiles[s_sel].pass);
        }
        break;

    case SA_SCR_SCAN_APS:
    {
        wifi_ap_record_t *ap = poom_wifi_scanner_get_ap_record((unsigned)s_sel);
        if (ap != NULL)
        {
            sa_enter_password_((const char *)ap->ssid);
        }
        break;
    }

    case SA_SCR_HOSTS:
        s_host_sel = s_sel; /* remember the row so B returns here later */
        if (s_sel < SA_HOST_ACT_COUNT && !sa_need_link_(SA_RESUME_HOSTS_ROW)) { break; }
        if (s_sel == SA_HOST_ACT_AUDIT)
        {
            /* Prompt for the range (default Quick), then confirm/estimate. */
            s_audit_flow = true;
            s_scanall_pending = false;
            s_screen = SA_SCR_PORT_PROFILE;
            sa_reset_list_();
        }
        else if (s_sel == SA_HOST_ACT_SWEEP) { s_sweep_pending = true; }
        else if (s_sel == SA_HOST_ACT_ALL)
        {
            if (s_ses->host_count == 0) { sa_set_info_("Sweep first", SA_SCR_HOSTS); }
            else
            {
                s_audit_flow = false;
                s_scanall_pending = true;
                s_screen = SA_SCR_PORT_PROFILE;
                sa_reset_list_();
            }
        }
        else if (s_sel == SA_HOST_ACT_IP) { sa_enter_specific_ip_(); }
        else if (s_sel == SA_HOST_ACT_COUNT && s_ses->net_results)
        {
            /* Re-open the last Scan All / NetAudit without rescanning. */
            s_sel = 0;
            s_result_top = 0;
            s_summary_sel = 0;
            s_screen = s_net_assessed ? SA_SCR_NET_VULN : SA_SCR_NET_SUMMARY;
        }
        else
        {
            int hi = s_sel - sa_host_act_count_();
            if (hi >= 0 && hi < (int)s_ses->host_count)
            {
                s_ses->detail_host = s_ses->hosts[hi];
                s_detail_hi = hi;
                (void)snprintf(s_ses->target_ip, sizeof(s_ses->target_ip),
                               "%.15s", s_ses->hosts[hi].ip_str);
                s_scanall_pending = false;
                s_screen = SA_SCR_HOST_DETAIL;
            }
        }
        break;

    case SA_SCR_HOST_DETAIL:
        if (!sa_need_link_(SA_RESUME_PORTSCAN)) { break; }
        s_audit_flow = false;
        s_screen = SA_SCR_PORT_PROFILE; sa_reset_list_();  /* method screen removed */
        break;

    case SA_SCR_PORT_METHOD:
        if (s_sel == 1 && !poom_security_audit_syn_available())
        {
            sa_set_info_("SYN not available", SA_SCR_PORT_METHOD);
        }
        else
        {
            s_method = (uint8_t)s_sel;
            s_screen = SA_SCR_PORT_PROFILE; sa_reset_list_();
        }
        break;

    case SA_SCR_PORT_PROFILE:
        if (s_sel == 3) { sa_enter_custom_ports_(); }
        else if (s_audit_flow) { sa_audit_enter_confirm_((uint8_t)s_sel); }
        else
        {
            /* profile choice carried via s_sel into the pending scan */
            s_portscan_pending = true;
        }
        break;

    case SA_SCR_NET_SUMMARY:
    {
        int h = sa_nth_open_host_(s_sel);
        if (h >= 0)
        {
            s_summary_sel = s_sel; /* remember which host, to return to it */
            s_open_host = h;
            s_sel = 0;
            s_result_top = 0;
            s_screen = SA_SCR_HOST_OPEN;
        }
        break;
    }

    case SA_SCR_HOST_OPEN:
    {
        int h = s_open_host;
        if (s_sel >= 0 && s_sel < s_ses->nb->host_open[h].count)
        {
            (void)snprintf(s_detail_ip, sizeof(s_detail_ip), "%.15s", s_ses->hosts[h].ip_str);
            s_detail_port = s_ses->nb->host_open[h].ports[s_sel];
            s_detail_state = POOM_SECURITY_AUDIT_PORT_OPEN;
            s_detail_return = SA_SCR_HOST_OPEN;
            /* Seed the detail from the compact service id captured in Scan All
             * (name only; A re-detects for product/version/banner). */
            uint8_t sid = s_ses->nb->host_open[h].svc_id[s_sel];
            if (sid != 0)
            {
                memset(&s_ses->detail_svc, 0, sizeof(s_ses->detail_svc));
                s_ses->detail_svc.port = s_detail_port;
                (void)snprintf(s_ses->detail_svc.service, sizeof(s_ses->detail_svc.service),
                               "%s", poom_security_audit_service_by_id(sid));
                s_ses->detail_svc.evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
                uint8_t pid = s_ses->nb->host_open[h].prod[s_sel];
                if (pid != 0U && pid != POOM_NET_AUDIT_INTERN_FULL)
                {
                    const poom_net_audit_prodver_t *pv = &s_ses->nb->net_prod[pid - 1U];
                    (void)snprintf(s_ses->detail_svc.product, sizeof(s_ses->detail_svc.product),
                                   "%.27s", pv->product);
                    (void)snprintf(s_ses->detail_svc.version, sizeof(s_ses->detail_svc.version),
                                   "%.23s", pv->version);
                    if (pv->version[0] != '\0')
                    {
                        s_ses->detail_svc.evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED;
                    }
                }
                s_ses->detail_svc_valid = true;
            }
            else
            {
                s_ses->detail_svc_valid = false; /* show guess until A detects */
            }
            s_screen = SA_SCR_PORT_DETAIL;
        }
        break;
    }

    case SA_SCR_RESULTS:
    {
        /* Map the cursor (s_sel-th open port) back to its result record. */
        int k = 0;
        for (size_t i = 0; i < s_ses->result_count; ++i)
        {
            if (s_ses->sb->results[i].state != POOM_SECURITY_AUDIT_PORT_OPEN) { continue; }
            if (k == s_sel)
            {
                (void)snprintf(s_detail_ip, sizeof(s_detail_ip), "%.15s", s_ses->target_ip);
                s_detail_port = s_ses->sb->results[i].port;
                s_detail_state = s_ses->sb->results[i].state;
                s_detail_return = SA_SCR_RESULTS;
                /* Prefill from the service already detected after the scan. */
                poom_security_audit_service_t *sv = sa_find_svc_(s_detail_port);
                if (sv != NULL)
                {
                    s_ses->detail_svc = *sv;
                    s_ses->detail_svc_valid = true;
                }
                else
                {
                    s_ses->detail_svc_valid = false;
                }
                s_screen = SA_SCR_PORT_DETAIL;
                break;
            }
            k++;
        }
        break;
    }

    case SA_SCR_PORT_DETAIL:
        if (sa_need_link_(SA_RESUME_DETECT)) { s_detect_pending = true; } /* A = (re)detect in task */
        break;

    case SA_SCR_VULN_SUMMARY:
        if (s_sel == 0)      { s_screen = SA_SCR_VULN_POTENTIAL; s_sel = 0; }
        else if (s_sel == 1) { s_gaps_mode = 0; s_screen = SA_SCR_VULN_GAPS; }
        else if (s_sel == 2) { s_gaps_mode = 1; s_screen = SA_SCR_VULN_GAPS; }
        else                 { s_screen = SA_SCR_VULN_EXPOSURES; }
        break;

    case SA_SCR_VULN_POTENTIAL:
        if (s_sel == 2 && s_proj_host >= 0) { s_screen = SA_SCR_DEVICE_INFO; break; }
        if (s_sel == 0) /* By exploit (all services) */
        {
            s_ss_service_filter = -1;
            s_ss_tier_filter = 0;
            sa_build_ss_disp_();
            s_screen = SA_SCR_SS_RESULTS;
        }
        else /* By service */
        {
            s_screen = SA_SCR_VULN_SERVICES;
            s_sel = 0;
            s_result_top = 0;
        }
        break;

    case SA_SCR_VULN_SERVICES:
        if (s_sel >= 0 && s_sel < s_ses->as->ss_n_terms)
        {
            s_ss_service_filter = s_sel;
            s_ss_tier_filter = 0;
            sa_build_ss_disp_();
            s_screen = SA_SCR_SS_RESULTS;
        }
        break;

    case SA_SCR_SS_RESULTS:
        if (s_ses->as->ss_disp_count > 0)
        {
            s_ss_sel = s_sel;
            s_screen = SA_SCR_SS_DETAIL;
        }
        break;

    case SA_SCR_AUDIT_CONFIRM:
        s_audit_pending = true; /* run the pipeline in the task */
        break;

    case SA_SCR_NET_VULN:
        if (s_sel == 4) { s_report_pending = true; break; } /* Save report (SD I/O in task) */
        if (s_sel >= 0 && s_sel <= 3)
        {
            s_net_cat = (uint8_t)s_sel;
            s_sel = 0;
            s_result_top = 0;
            s_screen = SA_SCR_NET_VULN_HOSTS;
        }
        break;

    case SA_SCR_NET_VULN_HOSTS:
    {
        int h = sa_net_cat_host_(s_sel, NULL);
        if (h >= 0)
        {
            s_net_host_sel = s_sel;
            sa_project_host_((size_t)h);
            /* Go straight to what was picked, for this host (the per-host
             * summary would just repeat the categories). B -> host list. */
            s_net_direct = true;
            switch (s_net_cat)
            {
            case 0: /* Potential -> By exploit / By service / Device info */
                s_screen = SA_SCR_VULN_POTENTIAL;
                s_sel = 0;
                break;
            case 1: s_gaps_mode = 0; s_screen = SA_SCR_VULN_GAPS; break; /* no-match services */
            case 2: s_gaps_mode = 1; s_screen = SA_SCR_VULN_GAPS; break; /* unassessed ports */
            default: s_screen = SA_SCR_VULN_EXPOSURES; break;          /* findings */
            }
        }
        break;
    }

    case SA_SCR_VULN_GAPS:
    case SA_SCR_SS_DETAIL:
        break; /* A does nothing; B returns */

    case SA_SCR_SAVE_CONFIRM:
        if (sa_profiles_() != NULL)
        {
            (void)poom_wifi_profile_store_add_or_update(
                s_ses->profiles, &s_ses->profile_count, POOM_WIFI_PROFILE_MAX,
                s_ses->edit_ssid, s_ses->edit_pass);
        }
        s_ses->offer_save = false;
        sa_set_info_("Saved to profiles", SA_SCR_HOSTS);
        break;

    case SA_SCR_INFO:
        s_screen = s_info_return;
        break;

    default:
        break;
    }
}

static void sa_button_cb_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t ev;
    (void)user;
    if (msg == NULL || msg->len < sizeof(ev)) { return; }
    if (s_relinking || s_ses == NULL) { return; } /* session being rebuilt */
    memcpy(&ev, msg->data, sizeof(ev));

    /* Hold B (long press) -> return to POOM main menu, from any screen. Handled
     * only on the long-press event so it never doubles as a short-press back. */
    if (ev.button == BUTTON_B && sa_is_long_press_(ev.event))
    {
        s_cancel = true;
        s_exit_requested = true;
        sa_wake_();
        return;
    }

    if (ev.event != BUTTON_SINGLE_CLICK) { return; }

    /* During a Full NetAudit only B (cancel) is live; the worker owns the
     * screen until it lands on a results screen. */
    if (s_audit_running)
    {
        if (ev.button == BUTTON_B) { sa_handle_back_(); sa_wake_(); }
        return;
    }

    /* Keyboard screens route navigation into the OSK. */
    if (s_screen == SA_SCR_PASSWORD || s_screen == SA_SCR_SPECIFIC_IP ||
        s_screen == SA_SCR_PORT_CUSTOM)
    {
        if (ev.button == BUTTON_B)
        {
            sa_handle_back_();
            sa_wake_();
            return;
        }
        poom_ui_keyboard_action_t act = poom_ui_keyboard_handle_button(&s_ses->kb, ev.button);
        if (act == POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            if (s_screen == SA_SCR_PASSWORD)
            {
                (void)snprintf(s_ses->edit_pass, sizeof(s_ses->edit_pass), "%.63s", s_ses->kb_text);
                s_connect_pending = true;
            }
            else if (s_screen == SA_SCR_SPECIFIC_IP)
            {
                s_resolve_ip_pending = true;
            }
            else /* custom ports */
            {
                size_t n = poom_security_audit_parse_ports(s_ses->kb_text, s_ses->sb->ports, SA_PORT_CHUNK);
                if (n == 0) { sa_set_info_("Bad port list", SA_SCR_PORT_PROFILE); }
                else if (s_audit_flow)
                {
                    s_audit_custom_n = (uint32_t)n;
                    sa_audit_enter_confirm_(3);
                }
                else { s_prog_total = (uint32_t)n; s_sel = 3; s_portscan_pending = true; }
            }
        }
        sa_wake_();
        return;
    }

    if (ev.button == BUTTON_B)
    {
        sa_handle_back_();
        sa_wake_();
        return;
    }

    /* list navigation */
    if (ev.button == BUTTON_UP || ev.button == BUTTON_DOWN)
    {
        /* Detail pages scroll one line; lists move the cursor. */
        if (sa_is_page_screen_(s_screen))
        {
            s_page_top += (ev.button == BUTTON_UP) ? -1 : 1; /* draw clamps */
        }
        else
        {
            int delta = (ev.button == BUTTON_UP) ? -1 : 1;
            s_sel += delta; /* draw clamps to the active list (incl. results) */
        }
        sa_wake_();
        return;
    }

    if (ev.button == BUTTON_LEFT || ev.button == BUTTON_RIGHT)
    {
        if (s_screen == SA_SCR_SS_RESULTS)
        {
            /* Cycle the tier filter: ALL -> STRONG -> MEDIUM -> WEAK (wraps). */
            int d = (ev.button == BUTTON_RIGHT) ? 1 : -1;
            s_ss_tier_filter = (uint8_t)(((int)s_ss_tier_filter + 4 + d) % 4);
            sa_build_ss_disp_();
            sa_wake_();
            return;
        }
        if (ev.button == BUTTON_LEFT)
        {
            if (s_screen == SA_SCR_PORT_PROFILE && !s_audit_flow)
            {
                s_services_on = !s_services_on; /* services on/off (forced ON in NetAudit) */
                sa_wake_();
            }
            else if (s_screen == SA_SCR_RESULTS)
            {
                s_assess_pending = true; /* L = Assess (exploit lookup) */
                sa_wake_();
            }
            else if (s_screen == SA_SCR_NET_SUMMARY)
            {
                s_summary_sel = s_sel;
                if (s_net_assessed && !s_net_partial)
                {
                    /* reuse the last complete Assess All (until the next Scan All) */
                    s_sel = 0;
                    s_result_top = 0;
                    s_screen = SA_SCR_NET_VULN;
                }
                else
                {
                    s_assess_all_pending = true; /* L = Assess All */
                }
                sa_wake_();
            }
            return;
        }
        /* RIGHT is unbound everywhere else (A confirms; RIGHT never does). */
        return;
    }

    if (ev.button == BUTTON_A)
    {
        sa_handle_select_();
        sa_wake_();
        return;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */


/* printf, not ESP_LOGx: this build sets CONFIG_LOG_DEFAULT_LEVEL_NONE, which
 * compiles every ESP_LOG call out; stdout still reaches the USB console. */
static void sa_log_heap_(const char *when)
{
    printf("[security_audit] %s: free %u, largest block %u\n", when,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void sa_session_free_(void)
{
    if (s_ses == NULL) { return; }
    free(s_ses->sb);
    free(s_ses->as);
    free(s_ses->nb);
    free(s_ses->ids);
    free(s_ses->profiles);
    free(s_ses);
    s_ses = NULL;
}

/* Allocate the core + side blocks; all-or-nothing. */
static bool sa_session_alloc_(void)
{
    s_ses = (sa_session_t *)calloc(1, sizeof(sa_session_t));
    if (s_ses == NULL) { return false; }
    s_ses->sb = (sa_scanbuf_t *)calloc(1, sizeof(sa_scanbuf_t));
    s_ses->as = (sa_assess_t *)calloc(1, sizeof(sa_assess_t));
    s_ses->nb = (sa_netbuf_t *)calloc(1, sizeof(sa_netbuf_t));
    s_ses->ids = (poom_security_audit_ident_t *)calloc(SA_MAX_HOSTS, sizeof(poom_security_audit_ident_t));
    if (s_ses->sb == NULL || s_ses->as == NULL || s_ses->nb == NULL || s_ses->ids == NULL)
    {
        sa_session_free_();
        return false;
    }
    return true;
}

/* Hand the radio back in its boot state (Wi-Fi off). Nothing starts Wi-Fi at
 * boot; each app brings it up itself and e.g. Fox Hunt only tears down a
 * driver it started. Leaving ours resident (even disconnected) starves the
 * next radio app - Bluedroid BLE init in particular needs a large heap. So:
 * disconnect (bounded wait), then stop + deinit the driver. */
static void sa_release_wifi_(void)
{
    if (poom_wifi_ctrl_sta_has_ip() || poom_wifi_ctrl_sta_is_connected())
    {
        (void)poom_wifi_ctrl_sta_disconnect();
        for (uint8_t i = 0; i < 12U; ++i)
        {
            if (!poom_wifi_ctrl_sta_has_ip() && !poom_wifi_ctrl_sta_is_connected()) { break; }
            vTaskDelay(pdMS_TO_TICKS(100U));
        }
    }
    (void)poom_wifi_ctrl_deinit(); /* also unregisters our event cb */
}

static void sa_exit_(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    s_active = false;

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", sa_button_cb_, "menu_security_audit");
        s_buttons_subscribed = false;
    }

    sa_release_wifi_();
    (void)poom_wifi_ctrl_unregister_cb();

    sa_session_free_();
    free(s_net_ss);
    s_net_ss = NULL;
    s_net_ss_count = 0;
    poom_net_audit_release(); /* unmount the SD only if Assess mounted it */
    /* Printed while this task's own stack (SA_TASK_STACK) is still allocated:
     * free is ~6.4 KB lower and the largest block can read split until the
     * task is deleted. Compare sessions using the next "enter" line. */
    sa_log_heap_("exit (task stack still held)");
    if (current == s_task || s_task == NULL)
    {
        /* Runs in the scanner task: how close did it get to its stack size? */
        printf("[security_audit] task stack: %u of %u bytes never used\n",
               (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)SA_TASK_STACK);
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

/* Mid-session rejoin after a DB pass (Wi-Fi is off). Measured on device: a
 * Wi-Fi restart on a heap still holding the session + the results buffer
 * (taken while Wi-Fi was down) can fail for lack of a large block. So, like
 * exit + re-enter: keep only the network credentials, free EVERYTHING, then
 * reallocate fresh and let the normal connect path run. The old results are
 * dropped (a reconnect already invalidated them). Runs in the task. */
/* Light rejoin: the only long-lived block allocated while Wi-Fi was down is
 * the results buffer - the session blocks date from entry, laid out as on a
 * fresh entry - so free just that (and the SD) and keep the device list,
 * names and scan results. Assess results are dropped (re-run L if needed). */
static void sa_relink_light_(void)
{
    s_wifi_suspended = false;
    poom_net_audit_release();
    free(s_net_ss);
    s_net_ss = NULL;
    s_net_ss_count = 0;
    s_net_assessed = false;
    s_net_partial = false;
    s_ses->offer_save = false;
    s_relink_keep = true;
    s_relink_full_tried = false;
    sa_log_heap_("relink (results freed)");
    s_connect_pending = true;
}

/* Full rejoin (fallback if the light one could not start Wi-Fi): free the
 * whole session, reallocate fresh, rediscover. */
static bool sa_relink_(void)
{
    char ssid[POOM_WIFI_PROFILE_SSID_CAP];
    char pass[POOM_WIFI_PROFILE_PASS_CAP];
    (void)snprintf(ssid, sizeof(ssid), "%.32s", s_ses->edit_ssid);
    (void)snprintf(pass, sizeof(pass), "%.63s", s_ses->edit_pass);

    s_relinking = true;
    s_wifi_suspended = false;
    poom_net_audit_release();
    free(s_net_ss);
    s_net_ss = NULL;
    s_net_ss_count = 0;
    s_net_assessed = false;
    s_net_partial = false;
    sa_session_free_();
    sa_log_heap_("relink (all freed)");

    bool ok = sa_session_alloc_();
    s_relinking = false;
    if (!ok)
    {
        printf("[security_audit] relink: session alloc failed\n");
        s_exit_requested = true; /* cannot continue without a session */
        return false;
    }
    (void)snprintf(s_ses->edit_ssid, sizeof(s_ses->edit_ssid), "%s", ssid);
    (void)snprintf(s_ses->edit_pass, sizeof(s_ses->edit_pass), "%s", pass);
    s_ses->offer_save = false;
    s_ses->auto_swept = false; /* rediscover devices once connected */
    s_detail_hi = -1;
    s_host_sel = 0;
    sa_reset_list_();
    s_connect_pending = true;
    return true;
}

static void sa_task_(void *arg)
{
    (void)arg;
    sa_draw_();

    while (s_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SA_REFRESH_MS));

        if (s_exit_requested) { sa_exit_(); break; }

        /* Auto Ping Sweep once, the first time we land on an empty device list
         * (so "Scan All Ports" isn't gated behind a manual Discover step). */
        if (s_screen == SA_SCR_HOSTS && !s_ses->auto_swept &&
            s_ses->host_count == 0 && !s_sweep_pending)
        {
            s_ses->auto_swept = true;
            s_sweep_pending = true;
        }

        if (s_relink_pending)
        {
            s_relink_pending = false;
            sa_relink_light_();
        }

        if (s_scan_aps_pending) { s_scan_aps_pending = false; sa_run_scan_aps_(); }

        if (s_report_pending)
        {
            s_report_pending = false;
            unsigned rn = sa_write_report_();
            char msg[22];
            if (rn != 0U) { (void)snprintf(msg, sizeof(msg), "Saved audit_%03u.txt", rn % 1000U); }
            else          { (void)snprintf(msg, sizeof(msg), "Report not saved"); }
            sa_set_info_(msg, SA_SCR_NET_VULN);
        }

        if (s_connect_pending)
        {
            s_connect_pending = false;
            /* Link-event cb is dropped by a Wi-Fi deinit (CSV pass); without it
             * association never completes. Idempotent. */
            (void)poom_wifi_ctrl_register_cb(sa_wifi_evt_cb_, NULL);
            sa_log_heap_("connect");
            esp_err_t st = poom_wifi_ctrl_sta_connect(
                s_ses->edit_ssid, (s_ses->edit_pass[0] != '\0') ? s_ses->edit_pass : NULL);
            if (st == ESP_ERR_NO_MEM && s_relink_keep && !s_relink_full_tried)
            {
                /* Light rejoin could not start Wi-Fi: fall back to the full
                 * rebuild (frees the whole session) and retry once. */
                sa_log_heap_("connect no RAM -> full relink");
                s_relink_full_tried = true;
                s_relink_keep = false;
                s_resume = SA_RESUME_NONE;
                (void)sa_relink_(); /* sets s_connect_pending, or exit on failure */
                continue;
            }
            if (st != ESP_OK)
            {
                s_relink_keep = false;
                s_resume = SA_RESUME_NONE;
                sa_log_heap_("connect-fail");
                sa_set_info_((st == ESP_ERR_NO_MEM) ? "Wi-Fi: no RAM" : "Connect failed",
                             SA_SCR_WIFI);
            }
            else
            {
                /* Save is offered only after a confirmed connection (see
                 * sa_after_connect_), so a bad password is never persisted. */
                s_connect_start_ms = sa_now_ms_();
                s_screen = SA_SCR_CONNECTING;
            }
        }

        if (s_screen == SA_SCR_CONNECTING) { sa_update_connecting_(); }

        if (s_resolve_ip_pending) { s_resolve_ip_pending = false; sa_run_resolve_ip_(); }

        if (s_detect_pending)
        {
            s_detect_pending = false;
            s_cancel = false;
            s_detecting = true;
            sa_draw_();                 /* show "Detecting..." */
            (void)poom_security_audit_detect_service(s_detail_ip, s_detail_port, 0,
                                                  &s_ses->detail_svc, &s_cancel);
            s_ses->detail_svc.port = s_detail_port;
            s_ses->detail_svc_valid = true;
            s_detecting = false;
        }

        if (s_assess_pending) { s_assess_pending = false; sa_run_assess_(); }

        if (s_assess_all_pending) { s_assess_all_pending = false; (void)sa_run_assess_all_(); }

        if (s_audit_pending) { s_audit_pending = false; sa_run_netaudit_(); }

        if (s_sweep_pending) { s_sweep_pending = false; sa_run_sweep_(); }

        if (s_portscan_pending)
        {
            s_portscan_pending = false;
            uint8_t choice = (uint8_t)s_sel; /* 0..3 from the profile list */
            if (s_scanall_pending)
            {
                sa_run_scan_all_(choice);
                s_scanall_pending = false;
            }
            else
            {
                sa_run_single_scan_(choice);
            }
        }

        sa_draw_();
    }

    vTaskDelete(NULL);
}

void menu_security_audit_show(void)
{
    /* Defensive: never overwrite a live session pointer (that would leak it). */
    sa_session_free_();
    free(s_net_ss);
    s_net_ss = NULL;

    sa_log_heap_("enter");
    if (!sa_session_alloc_())
    {
        printf("[security_audit] session alloc failed (core %u + %u/%u/%u bytes)\n",
               (unsigned)sizeof(sa_session_t), (unsigned)sizeof(sa_scanbuf_t),
               (unsigned)sizeof(sa_assess_t), (unsigned)sizeof(sa_netbuf_t));
        /* Can't open without a session. Don't leave the (already-detached) main
         * menu frozen: hand control back so POOM redraws and stays responsive. */
        const uint8_t token = 1;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    s_active = true;
    s_exit_requested = false;
    s_cancel = false;
    s_connect_pending = false;
    s_sweep_pending = false;
    s_scan_aps_pending = false;
    s_portscan_pending = false;
    s_scanall_pending = false;
    s_resolve_ip_pending = false;
    s_detect_pending = false;
    s_detecting = false;
    s_aps_scanning = false;
    s_svc_phase = false;
    s_assess_pending = false;
    s_ss_searching = false;
    s_ss_sel = 0;
    s_ss_service_filter = -1;
    s_ss_tier_filter = 0;
    s_gaps_mode = 0;
    s_assess_all_pending = false;
    s_wifi_suspended = false;
    s_detail_hi = -1;
    s_relink_pending = false;
    s_chk_phase = false;
    s_report_pending = false;
    s_relink_keep = false;
    s_relink_full_tried = false;
    s_resume = SA_RESUME_NONE;
    s_id_phase = false;
    s_cur_os_class = POOM_NET_AUDIT_OS_UNKNOWN;
    s_audit_flow = false;
    s_audit_pending = false;
    s_audit_running = false;
    s_audit_cancel = false;
    s_audit_profile = 0;
    s_audit_ports_n = 0;
    s_audit_custom_n = 0;
    s_net_assessed = false;
    s_net_partial = false;
    s_assess_net = false;
    s_net_direct = false;
    s_proj_host = -1;
    s_net_cat = 0;
    s_net_host_sel = 0;
    s_net_ss_count = 0;
    s_services_on = true;
    s_host_sel = 0;
    s_summary_sel = 0;
    s_method = 0;
    sa_reset_list_();
    s_result_top = 0;

    /* Bring up the STA driver/netif (idempotent) before any connect, and
     * register for link events -- without this, association never completes and
     * sta_has_ip() never becomes true. Mirrors menu_wifi_profiles.c. */
    esp_err_t wst = poom_wifi_ctrl_init_sta();
    (void)poom_wifi_ctrl_register_cb(sa_wifi_evt_cb_, NULL);
    if (wst != ESP_OK)
    {
        printf("[security_audit] Wi-Fi start failed: %s\n", esp_err_to_name(wst));
        sa_log_heap_("wifi-start-fail");
    }

    /* Saved Wi-Fi profiles (shared store) load on demand: sa_profiles_(). */

    /* If already connected, land on the Wi-Fi screen showing the live network;
     * the user confirms "Scan Hosts" to continue. Otherwise it offers the
     * profile / scan choices. */
    s_screen = SA_SCR_WIFI;
    if (wst != ESP_OK)
    {
        sa_set_info_((wst == ESP_ERR_NO_MEM) ? "Wi-Fi: no RAM" : "Wi-Fi start failed",
                     SA_SCR_WIFI);
    }

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", sa_button_cb_, "menu_security_audit");
        s_buttons_subscribed = true;
    }

    if (s_task == NULL)
    {
        if (xTaskCreate(sa_task_, "menu_security_audit", SA_TASK_STACK, NULL,
                        SA_TASK_PRIO, &s_task) != pdPASS)
        {
            /* Out of memory for the task: clean up fully so nothing leaks and
             * the main menu stays usable, instead of a stuck, growing-OOM app. */
            s_task = NULL;
            s_active = false;
            if (s_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", sa_button_cb_, "menu_security_audit");
                s_buttons_subscribed = false;
            }
            sa_release_wifi_();
            (void)poom_wifi_ctrl_unregister_cb();
            sa_session_free_();
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    sa_wake_();
}
