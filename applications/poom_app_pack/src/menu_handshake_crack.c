// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Handshake cracker UI -- a quick on-device check of a captured handshake
// against the most common WPA passphrases (like the BLEShark Nano's "Auto
// Crack"). Two ways in:
//   1. Browser:  BEAST -> CRACK HS -> lists saved .22000 files -> pick -> crack.
//   2. Direct:   the capture screen's post-capture "A -> CRACK" option opens
//                this straight on the freshly-saved .22000 (menu_handshake_crack_show_file).
//
// The heavy lifting (parse + PBKDF2/PMKID/MIC) lives in poom_handshake_crack.c;
// this file is the screen flow (menu task + button/sbus model).

#include "menu_handshake_crack.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_handshake_crack.h"
#include "poom_hs_crack_journal.h"
#include "poom_sbus.h"
#include "poom_sd_browser.h"
#include "sd_card.h"

/* poom_menu is not in this component's REQUIRES, so define the resume topic
 * locally -- same pattern the sibling menus use. */
#ifndef POOM_MENU_RESUME_TOPIC
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define HS_CRACK_TASK_STACK   (12288)
#define HS_CRACK_TASK_PRIO    (5)
#define HS_CRACK_REFRESH_MS   (200)
#define HS_CRACK_DRAW_MIN_MS  (250)      /* throttle redraws during a run */

#define HS_CRACK_DIR          SD_CARD_PATH "/pcaps/handshakes"
#define HS_WORDLIST_DIR       SD_CARD_PATH "/wordlists"
#define HS_CRACK_MAX_FILES    (128U)
#define HS_CRACK_NAME_MAX     (32U)
#define HS_CRACK_VISIBLE_ROWS (4U)

typedef enum {
    HS_CRACK_SCREEN_BROWSE = 0,
    HS_CRACK_SCREEN_CONFIRM,
    HS_CRACK_SCREEN_RUNNING,
    HS_CRACK_SCREEN_RESULT,
    HS_CRACK_SCREEN_INFO,     /* empty list / error, message in s_info */
    HS_CRACK_SCREEN_ALREADY,  /* this (AP,list) was already tried -- run again? */
    HS_CRACK_SCREEN_SDWARN,   /* SD dictionary attack time warning -- proceed? */
} hs_crack_screen_t;

static bool s_active = false;
static volatile bool s_exit_requested = false;   /* leave the menu entirely   */
static volatile bool s_cancel = false;           /* abort an in-flight run    */
static volatile bool s_start_requested = false;  /* begin cracking s_path     */
static volatile bool s_continue_requested = false; /* resume with the variations */
static bool s_rescan_requested = false;
static bool s_buttons_subscribed = false;
static TaskHandle_t s_task = NULL;

static hs_crack_screen_t s_screen = HS_CRACK_SCREEN_BROWSE;
static bool s_direct_mode = false;               /* opened on a fixed file    */
static poom_hs_crack_scope_t s_scope = POOM_HS_SCOPE_QUICK; /* mode on confirm */
static bool s_can_continue = false;              /* QUICK unfound -> offer more */

/* Confirm-screen mode selector. QUICK/SMART use the built-in wordlist (mapped
 * to the engine scope); SD-LIST streams a wordlist file chosen from the SD. */
typedef enum {
    HS_MODE_QUICK = 0,
    HS_MODE_SMART,
    HS_MODE_SDLIST,
    HS_MODE_COUNT,
} hs_ui_mode_t;
static hs_ui_mode_t s_mode = HS_MODE_QUICK;

static volatile bool s_browse_requested = false;   /* launch the SD list picker */
static volatile bool s_sd_run_requested = false;   /* crack s_path x s_wordlist  */
static char s_wordlist_path[160] = {0};            /* chosen wordlist, abs path  */
static char s_wordlist_label[HS_CRACK_NAME_MAX] = {0};

/* Crack journal: AP identity of the selected handshake, and the pending action
 * when prompting about an already-tried list. */
static uint8_t s_ap_bssid[6] = {0};
static char    s_ap_essid[33] = {0};
static bool    s_ap_valid = false;                 /* peek of s_path succeeded   */
static bool    s_from_journal = false;             /* result recalled, not run   */
static bool    s_pending_is_sd = false;            /* ALREADY screen: which run  */
static poom_hs_crack_scope_t s_pending_scope = POOM_HS_SCOPE_QUICK;
static char    s_pending_list[40] = {0};           /* list_id being confirmed    */
static bool    s_file_cracked[HS_CRACK_MAX_FILES]; /* browser "already cracked"  */

static char s_files[HS_CRACK_MAX_FILES][HS_CRACK_NAME_MAX];
static uint32_t s_file_count = 0;
static uint32_t s_sel = 0;
static uint32_t s_scroll = 0;

static char s_path[160] = {0};   /* full path of the file to crack */
static char s_label[HS_CRACK_NAME_MAX] = {0};
static char s_info[40] = {0};

static poom_hs_crack_result_t s_result;
static volatile uint32_t s_tried = 0;
static uint32_t s_total = 0;
static int64_t s_last_draw_us = 0;
static int64_t s_crack_start_us = 0;   /* set when hashing begins (total>0) */

static void menu_handshake_crack_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_handshake_crack_draw_(void);
static void menu_handshake_crack_run_wordlist_(void);
static void menu_handshake_crack_launch_wordlist_browser_(void);
static void menu_handshake_crack_resume_(void);
static void menu_handshake_crack_task_(void *arg);

/* ---- helpers ------------------------------------------------------------ */

static void menu_handshake_crack_request_render_(void)
{
    if (s_task != NULL)
    {
        (void)xTaskNotifyGive(s_task);
    }
}

static bool hs_has_suffix_22000_(const char *name)
{
    size_t n = strlen(name);
    return (n > 6U) && (strcmp(&name[n - 6U], ".22000") == 0);
}

/* Capture index from a "handshake_<N>.22000" name (the digits just before the
 * extension), used to order newest-first. Returns 0 if there are none. */
static uint32_t hs_file_index_(const char *name)
{
    const char *dot = strrchr(name, '.');
    size_t end = (dot != NULL) ? (size_t)(dot - name) : strlen(name);
    size_t i = end;
    while ((i > 0U) && (name[i - 1U] >= '0') && (name[i - 1U] <= '9'))
    {
        i--;
    }
    if (i == end)
    {
        return 0U;
    }
    return (uint32_t)strtoul(&name[i], NULL, 10);
}

/* qsort comparator: highest capture index first (newest at the top). */
static int hs_cmp_newest_(const void *a, const void *b)
{
    uint32_t ia = hs_file_index_((const char *)a);
    uint32_t ib = hs_file_index_((const char *)b);
    if (ia < ib)
    {
        return 1;
    }
    if (ia > ib)
    {
        return -1;
    }
    return strcmp((const char *)b, (const char *)a);
}

/* (Re)build the list of .22000 files under the handshakes directory. */
static void hs_scan_dir_(void)
{
    s_file_count = 0;
    s_sel = 0;
    s_scroll = 0;

    if (sd_card_is_not_mounted())
    {
        sd_card_begin();
        if (sd_card_mount() != ESP_OK)
        {
            (void)snprintf(s_info, sizeof(s_info), "No SD card");
            return;
        }
    }

    DIR *dir = opendir(HS_CRACK_DIR);
    if (dir == NULL)
    {
        (void)snprintf(s_info, sizeof(s_info), "No handshakes dir");
        return;
    }

    struct dirent *entry;
    while (((entry = readdir(dir)) != NULL) && (s_file_count < HS_CRACK_MAX_FILES))
    {
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
        {
            continue;
        }
        if (!hs_has_suffix_22000_(entry->d_name))
        {
            continue;
        }
        /* Skip 0-byte hash files (old empty captures from before the
         * discard-if-empty logic) so the list shows only crackable files. */
        {
            char full[320]; /* dir + '/' + dirent name (up to 255) + NUL */
            struct stat st;
            (void)snprintf(full, sizeof(full), "%s/%s", HS_CRACK_DIR, entry->d_name);
            if ((stat(full, &st) == 0) && (st.st_size <= 0))
            {
                continue;
            }
        }
        (void)snprintf(s_files[s_file_count], HS_CRACK_NAME_MAX, "%.31s", entry->d_name);
        s_file_count++;
    }
    (void)closedir(dir);

    /* Newest capture first, so the most recent handshakes are at the top. */
    if (s_file_count > 1U)
    {
        qsort(s_files, s_file_count, HS_CRACK_NAME_MAX, hs_cmp_newest_);
    }

    /* Flag handshakes already cracked (peek the AP, check the journal) so the
     * browser can mark them. One tiny parse + lookup per file; fine for the
     * typical handful, and bounded by the 128-file cap. */
    for (uint32_t i = 0U; i < s_file_count; i++)
    {
        s_file_cracked[i] = false;
        char full[320];
        uint8_t bss[6];
        (void)snprintf(full, sizeof(full), "%s/%s", HS_CRACK_DIR, s_files[i]);
        if (poom_hs_crack_peek(full, bss, NULL, 0U) == ESP_OK)
        {
            char pw[64];
            s_file_cracked[i] = poom_hs_journal_get_cracked(bss, pw, sizeof(pw));
        }
    }

    if (s_file_count == 0U)
    {
        (void)snprintf(s_info, sizeof(s_info), "No .22000 files");
    }
}

static void hs_select_file_(uint32_t idx)
{
    (void)snprintf(s_label, sizeof(s_label), "%.31s", s_files[idx]);
    (void)snprintf(s_path, sizeof(s_path), "%s/%s", HS_CRACK_DIR, s_files[idx]);
    s_ap_valid = (poom_hs_crack_peek(s_path, s_ap_bssid, s_ap_essid,
                                     sizeof(s_ap_essid)) == ESP_OK);
}

/* The journal key for a given run: QUICK / SMART (built-in; CONTINUE completes
 * the SMART set) or SD:<wordlist basename>. */
static void hs_list_id_(poom_hs_crack_scope_t scope, bool is_sd, char *out, size_t sz)
{
    if (is_sd)
    {
        const char *base = strrchr(s_wordlist_path, '/');
        base = (base != NULL) ? (base + 1) : s_wordlist_path;
        (void)snprintf(out, sz, "SD:%.*s", (int)(sz - 4U), base);
    }
    else
    {
        (void)snprintf(out, sz, "%s", (scope == POOM_HS_SCOPE_QUICK) ? "QUICK" : "SMART");
    }
}

/* Present a password recalled from the journal as a (found) result, no run. */
static void hs_show_saved_(const char *pw)
{
    memset(&s_result, 0, sizeof(s_result));
    s_result.found = true;
    (void)snprintf(s_result.password, sizeof(s_result.password), "%s", pw);
    (void)snprintf(s_result.essid, sizeof(s_result.essid), "%s", s_ap_essid);
    s_can_continue = false;
    s_from_journal = true;
    s_screen = HS_CRACK_SCREEN_RESULT;
}

/* Decide what happens when the user asks to crack: show a stored password,
 * warn that this list was already tried, or request the run. Safe to call from
 * a button/browser callback (it only sets flags + screen; the task runs). */
static void hs_decide_(poom_hs_crack_scope_t scope, bool is_sd)
{
    char pw[sizeof(s_result.password)];
    if (s_ap_valid && poom_hs_journal_get_cracked(s_ap_bssid, pw, sizeof(pw)))
    {
        hs_show_saved_(pw);
        return;
    }
    hs_list_id_(scope, is_sd, s_pending_list, sizeof(s_pending_list));
    if (s_ap_valid &&
        (poom_hs_journal_lookup(s_ap_bssid, s_pending_list) == HS_JOURNAL_MISS))
    {
        s_pending_is_sd = is_sd;
        s_pending_scope = scope;
        s_screen = HS_CRACK_SCREEN_ALREADY;
        return;
    }
    if (is_sd) { s_sd_run_requested = true; }
    else       { s_scope = scope; s_start_requested = true; }
}

/* ---- the run ------------------------------------------------------------ */

/* Progress callback: invoked from the crack task between passphrases. Keep the
 * display in step without redrawing on every try (the OLED write is slow). */
static void hs_progress_cb_(uint32_t tried, uint32_t total, void *user)
{
    (void)user;
    s_tried = tried;
    s_total = total;

    int64_t now = esp_timer_get_time();

    /* Mark when hashing actually begins (total becomes known / nonzero) so the
     * running screen can show a live PSK/s rate. total==0 is the engine's
     * "still counting the wordlist" sentinel, during which no hashing happens. */
    if ((total != 0U) && (s_crack_start_us == 0))
    {
        s_crack_start_us = now;
    }

    if ((tried == 1U) || ((now - s_last_draw_us) >= (HS_CRACK_DRAW_MIN_MS * 1000)))
    {
        s_last_draw_us = now;
        menu_handshake_crack_draw_();
    }
}

static void menu_handshake_crack_run_(poom_hs_crack_scope_t scope)
{
    s_screen = HS_CRACK_SCREEN_RUNNING;
    s_tried = 0;
    s_total = poom_hs_crack_wordlist_count();
    s_last_draw_us = 0;
    s_crack_start_us = 0;
    s_from_journal = false;
    menu_handshake_crack_draw_();

    (void)poom_hs_crack_run_file(s_path, scope, &s_cancel, hs_progress_cb_, NULL, &s_result);

    /* Journal the outcome of a completed run (not a cancel or parse error). */
    if (s_ap_valid && !s_result.error && !s_result.cancelled)
    {
        char lid[40];
        hs_list_id_(scope, false, lid, sizeof(lid));
        poom_hs_journal_record(s_ap_bssid, s_ap_essid, lid,
                               s_result.found ? HS_JOURNAL_CRACK : HS_JOURNAL_MISS,
                               s_result.found ? s_result.password : NULL);
    }

    s_can_continue = false;
    if (s_result.error)
    {
        (void)snprintf(s_info, sizeof(s_info), "%.39s", s_result.error_msg);
        s_screen = HS_CRACK_SCREEN_INFO;
    }
    else
    {
        /* After a QUICK pass with no hit, the variations/ESSID remain untried. */
        if ((scope == POOM_HS_SCOPE_QUICK) && !s_result.found && !s_result.cancelled)
        {
            s_can_continue = true;
        }
        s_screen = HS_CRACK_SCREEN_RESULT;
    }
}

static void menu_handshake_crack_run_wordlist_(void)
{
    s_screen = HS_CRACK_SCREEN_RUNNING;
    s_tried = 0;
    s_total = 0;               /* real total arrives via the progress callback */
    s_last_draw_us = 0;
    s_crack_start_us = 0;
    s_from_journal = false;
    menu_handshake_crack_draw_();

    (void)poom_hs_crack_run_file_wordlist(s_path, s_wordlist_path, &s_cancel,
                                          hs_progress_cb_, NULL, &s_result);

    if (s_ap_valid && !s_result.error && !s_result.cancelled)
    {
        char lid[40];
        hs_list_id_(POOM_HS_SCOPE_QUICK, true, lid, sizeof(lid));
        poom_hs_journal_record(s_ap_bssid, s_ap_essid, lid,
                               s_result.found ? HS_JOURNAL_CRACK : HS_JOURNAL_MISS,
                               s_result.found ? s_result.password : NULL);
    }

    s_can_continue = false;   /* an SD list has no "rules" follow-up pass */
    if (s_result.error)
    {
        (void)snprintf(s_info, sizeof(s_info), "%.39s", s_result.error_msg);
        s_screen = HS_CRACK_SCREEN_INFO;
    }
    else
    {
        s_screen = HS_CRACK_SCREEN_RESULT;
    }
}

/* ---- SD wordlist picker (hands off to poom_sd_browser, mirrors menu_lua) -- */

static bool hs_wordlist_filter_(const char *name, bool is_directory, void *user_ctx)
{
    (void)user_ctx;
    if (is_directory)
    {
        return true;
    }
    if ((name == NULL) || (name[0] == '\0'))
    {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return false;
    }
    return (strcasecmp(dot, ".txt") == 0) || (strcasecmp(dot, ".lst") == 0) ||
           (strcasecmp(dot, ".dic") == 0) || (strcasecmp(dot, ".dict") == 0);
}

/* Re-enter this menu at the confirm screen after the SD browser hands control
 * back, rebuilding the task + button subscription the browse teardown dropped.
 * Selection state (s_path, s_mode, s_wordlist_*) is preserved. */
static void menu_handshake_crack_resume_(void)
{
    if (s_task != NULL)
    {
        return;
    }
    s_active = true;
    s_exit_requested = false;
    s_cancel = false;
    s_start_requested = false;
    s_continue_requested = false;
    s_rescan_requested = false;
    s_browse_requested = false;
    s_screen = HS_CRACK_SCREEN_CONFIRM;

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_handshake_crack_button_cb_, "menu_hs_crack");
        s_buttons_subscribed = true;
    }
    (void)xTaskCreate(menu_handshake_crack_task_,
                      "menu_hs_crack",
                      HS_CRACK_TASK_STACK,
                      NULL,
                      HS_CRACK_TASK_PRIO,
                      &s_task);
}

static void hs_wordlist_exit_cb_(void *user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    menu_handshake_crack_resume_();   /* backed out without choosing a list */
}

static void hs_wordlist_selected_cb_(const char *abs_path, void *user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);

    if ((abs_path != NULL) && (abs_path[0] != '\0'))
    {
        (void)snprintf(s_wordlist_path, sizeof(s_wordlist_path), "%.159s", abs_path);
        const char *base = strrchr(abs_path, '/');
        base = (base != NULL) ? (base + 1) : abs_path;
        (void)snprintf(s_wordlist_label, sizeof(s_wordlist_label), "%.31s", base);
        menu_handshake_crack_resume_();
        /* saved password / already-tried prompt / request the run */
        hs_decide_(POOM_HS_SCOPE_QUICK, true);
        menu_handshake_crack_request_render_();
    }
    else
    {
        menu_handshake_crack_resume_();
    }
}

static void menu_handshake_crack_launch_wordlist_browser_(void)
{
    /* Prefer /sdcard/wordlists, but fall back to the SD root if that folder
     * doesn't exist yet, so the picker always opens somewhere navigable. */
    struct stat st;
    const char *start_dir =
        ((stat(HS_WORDLIST_DIR, &st) == 0) && S_ISDIR(st.st_mode)) ? HS_WORDLIST_DIR
                                                                   : SD_CARD_PATH;
    poom_sd_browser_config_t cfg = {
        .start_dir = start_dir,
        .header = "WORDLIST",
        .filter = hs_wordlist_filter_,
        .filter_ctx = NULL,
        .on_file_selected = hs_wordlist_selected_cb_,
        .on_file_selected_ctx = NULL,
    };
    (void)poom_sd_browser_set_exit_callback(hs_wordlist_exit_cb_, NULL);
    (void)poom_sd_browser_start_ex(&cfg);
}

/* ---- render ------------------------------------------------------------- */

static void hs_draw_header_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void hs_draw_browse_(void)
{
    char l[48];

    hs_draw_header_("CRACK HS");

    if (s_file_count == 0U)
    {
        poom_arduboy_set_cursor(2, 24);
        (void)poom_arduboy_print((s_info[0] != '\0') ? s_info : "No .22000 files");
        poom_arduboy_set_cursor(2, 54);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    uint32_t first = s_scroll;
    for (uint32_t i = 0U; i < HS_CRACK_VISIBLE_ROWS; i++)
    {
        uint32_t idx = first + i;
        if (idx >= s_file_count)
        {
            break;
        }
        int16_t y = (int16_t)(14 + (i * 10U));
        bool is_sel = (idx == s_sel);
        poom_arduboy_set_cursor(3, y);
        (void)snprintf(l, sizeof(l), "%s%.20s",
                       s_file_cracked[idx] ? "*" : "", s_files[idx]);
        (void)poom_arduboy_print(l);
        /* highlight the selected row: print white text, then invert the whole
         * row so it reads as black text on a white bar. */
        if (is_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    poom_arduboy_set_cursor(2, 56);
    (void)snprintf(l, sizeof(l), "%lu/%lu A:CRACK",
                   (unsigned long)(s_sel + 1U), (unsigned long)s_file_count);
    (void)poom_arduboy_print(l);
    poom_arduboy_display();
}

static void hs_draw_confirm_(void)
{
    char l[48];

    hs_draw_header_("CRACK HS");

    poom_arduboy_set_cursor(2, 16);
    (void)poom_arduboy_print(F("File:"));
    poom_arduboy_set_cursor(2, 26);
    (void)snprintf(l, sizeof(l), "%.20s", s_label);
    (void)poom_arduboy_print(l);

    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print((s_mode == HS_MODE_QUICK) ? F("Mode: QUICK")
                           : (s_mode == HS_MODE_SMART) ? F("Mode: SMART")
                                                       : F("Mode: SD LIST"));
    poom_arduboy_set_cursor(2, 45);
    if (s_mode == HS_MODE_QUICK)
    {
        uint32_t qc = poom_hs_crack_quick_count();
        (void)snprintf(l, sizeof(l), "%lu PSKs (~%lus)",
                       (unsigned long)qc, (unsigned long)((qc + 8U) / 9U));
        (void)poom_arduboy_print(l);
    }
    else if (s_mode == HS_MODE_SMART)
    {
        (void)poom_arduboy_print(F("+rules+ESSID (~2m)"));
    }
    else if (s_wordlist_label[0] != '\0')
    {
        (void)snprintf(l, sizeof(l), "%.20s", s_wordlist_label);
        (void)poom_arduboy_print(l);
    }
    else
    {
        (void)poom_arduboy_print(F("A: pick a .txt list"));
    }

    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("A:GO  <>:MODE  B:BK"));
    poom_arduboy_display();
}

static void hs_draw_running_(void)
{
    char l[48];

    hs_draw_header_("CRACKING");

    poom_arduboy_set_cursor(2, 16);
    if (s_result.essid[0] != '\0')
    {
        (void)snprintf(l, sizeof(l), "AP: %.16s", s_result.essid);
    }
    else
    {
        (void)snprintf(l, sizeof(l), "%.20s", s_label);
    }
    (void)poom_arduboy_print(l);

    uint32_t tried = s_tried;
    uint32_t total = s_total;

    if (total == 0U)
    {
        /* Pre-pass: the engine is still counting WPA-valid lines in the SD
         * wordlist (no hashing yet). Show it is working and the running count
         * so a large list never looks frozen. */
        poom_arduboy_set_cursor(2, 28);
        (void)poom_arduboy_print(F("Counting words..."));
        poom_arduboy_set_cursor(2, 40);
        (void)snprintf(l, sizeof(l), "%lu found", (unsigned long)tried);
        (void)poom_arduboy_print(l);
    }
    else
    {
        /* Hashing: words checked / total + percent, then live PSK/s rate, so
         * progress is readable even when the bar barely moves on a huge list. */
        uint32_t pct = (uint32_t)((uint64_t)tried * 100U / total);
        poom_arduboy_set_cursor(2, 24);
        (void)snprintf(l, sizeof(l), "%lu/%lu  %lu%%",
                       (unsigned long)tried, (unsigned long)total,
                       (unsigned long)pct);
        (void)poom_arduboy_print(l);

        poom_arduboy_set_cursor(2, 34);
        if (s_crack_start_us != 0)
        {
            uint32_t el_ms =
                (uint32_t)((esp_timer_get_time() - s_crack_start_us) / 1000);
            if (el_ms >= 500U)
            {
                /* integer PSK/s to one decimal: rate_x10 = tried*10 / seconds */
                uint32_t rate_x10 =
                    (uint32_t)((uint64_t)tried * 10000U / el_ms);
                (void)snprintf(l, sizeof(l), "%lu.%lu PSK/s",
                               (unsigned long)(rate_x10 / 10U),
                               (unsigned long)(rate_x10 % 10U));
            }
            else
            {
                (void)snprintf(l, sizeof(l), "-- PSK/s");
            }
            (void)poom_arduboy_print(l);
        }

        /* progress bar (64-bit math: tried can exceed 34M on big lists) */
        int16_t w = (int16_t)(uint32_t)((uint64_t)124U * tried / total);
        poom_arduboy_draw_rect(2, 44, 124, 8, WHITE);
        if (w > 0)
        {
            poom_arduboy_fill_rect(2, 44, w, 8, WHITE);
        }
    }

    poom_arduboy_set_cursor(2, 54);
    (void)poom_arduboy_print(F("B:CANCEL"));
    poom_arduboy_display();
}

static void hs_draw_result_(void)
{
    char l[48];

    hs_draw_header_(s_result.found ? "CRACKED!" : "NOT FOUND");

    if (s_result.found)
    {
        poom_arduboy_set_cursor(2, 15);
        (void)poom_arduboy_print(F("Password:"));
        poom_arduboy_set_cursor(2, 25);
        (void)snprintf(l, sizeof(l), "%.20s", s_result.password);
        (void)poom_arduboy_print(l);
    }
    else
    {
        poom_arduboy_set_cursor(2, 15);
        if (s_result.cancelled)
        {
            (void)poom_arduboy_print(F("Cancelled"));
        }
        else
        {
            (void)snprintf(l, sizeof(l), "Not in top %lu",
                           (unsigned long)s_result.total);
            (void)poom_arduboy_print(l);
        }
        poom_arduboy_set_cursor(2, 25);
        (void)snprintf(l, sizeof(l), "AP: %.16s", s_result.essid);
        (void)poom_arduboy_print(l);
    }

    if (s_from_journal)
    {
        /* Result recalled from the journal, not a fresh run. */
        poom_arduboy_set_cursor(2, 37);
        (void)snprintf(l, sizeof(l), "AP: %.16s", s_result.essid);
        (void)poom_arduboy_print(l);
        poom_arduboy_set_cursor(2, 47);
        (void)poom_arduboy_print(F("(saved - already done)"));
    }
    else
    {
        /* tried count + elapsed + rate (passphrases/sec) */
        uint32_t secs10 = s_result.elapsed_ms / 100U;   /* tenths of a second */
        poom_arduboy_set_cursor(2, 37);
        (void)snprintf(l, sizeof(l), "Tried %lu in %lu.%lus",
                       (unsigned long)s_result.tried,
                       (unsigned long)(secs10 / 10U), (unsigned long)(secs10 % 10U));
        (void)poom_arduboy_print(l);

        poom_arduboy_set_cursor(2, 47);
        if (s_result.elapsed_ms > 0U)
        {
            uint32_t rate10 = (uint32_t)(((uint64_t)s_result.tried * 10000ULL) / s_result.elapsed_ms);
            (void)snprintf(l, sizeof(l), "%lu.%lu PSK/s  HS:%u",
                           (unsigned long)(rate10 / 10U), (unsigned long)(rate10 % 10U),
                           (unsigned)s_result.target_count);
        }
        else
        {
            (void)snprintf(l, sizeof(l), "HS:%u", (unsigned)s_result.target_count);
        }
        (void)poom_arduboy_print(l);
    }

    poom_arduboy_set_cursor(2, 56);
    if (s_can_continue)
    {
        (void)poom_arduboy_print(F("A:MORE(rules) B:EXIT"));
    }
    else
    {
        (void)poom_arduboy_print(s_direct_mode ? F("A/B:BACK") : F("A:LIST   B:EXIT"));
    }
    poom_arduboy_display();
}

static void hs_draw_info_(void)
{
    hs_draw_header_("CRACK HS");
    poom_arduboy_set_cursor(2, 26);
    (void)poom_arduboy_print((s_info[0] != '\0') ? s_info : "Error");
    poom_arduboy_set_cursor(2, 54);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void hs_draw_already_(void)
{
    char l[48];
    hs_draw_header_("ALREADY TRIED");
    poom_arduboy_set_cursor(2, 16);
    (void)poom_arduboy_print(F("Tried this list on"));
    poom_arduboy_set_cursor(2, 26);
    (void)poom_arduboy_print(F("this AP before (miss):"));
    poom_arduboy_set_cursor(2, 38);
    (void)snprintf(l, sizeof(l), "%.20s", s_pending_list);
    (void)poom_arduboy_print(l);
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("A:RUN AGAIN  B:BACK"));
    poom_arduboy_display();
}

static void hs_draw_sdwarn_(void)
{
    hs_draw_header_("SD DICTIONARY");
    poom_arduboy_set_cursor(2, 16);
    (void)poom_arduboy_print(F("A large dictionary"));
    poom_arduboy_set_cursor(2, 26);
    (void)poom_arduboy_print(F("attack may take a"));
    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print(F("long time on-device."));
    poom_arduboy_set_cursor(2, 46);
    (void)poom_arduboy_print(F("Proceed?"));
    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("A:YES   B:BACK"));
    poom_arduboy_display();
}

static void menu_handshake_crack_draw_(void)
{
    if (!s_active)
    {
        return;
    }
    switch (s_screen)
    {
    case HS_CRACK_SCREEN_BROWSE:  hs_draw_browse_();  break;
    case HS_CRACK_SCREEN_CONFIRM: hs_draw_confirm_(); break;
    case HS_CRACK_SCREEN_RUNNING: hs_draw_running_(); break;
    case HS_CRACK_SCREEN_RESULT:  hs_draw_result_();  break;
    case HS_CRACK_SCREEN_ALREADY: hs_draw_already_(); break;
    case HS_CRACK_SCREEN_SDWARN:  hs_draw_sdwarn_();  break;
    case HS_CRACK_SCREEN_INFO:
    default:                      hs_draw_info_();    break;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

static void menu_handshake_crack_exit_(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();

    s_active = false;

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_handshake_crack_button_cb_, "menu_hs_crack");
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

static void menu_handshake_crack_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
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
    case HS_CRACK_SCREEN_BROWSE:
        if (ev.button == BUTTON_B)
        {
            s_exit_requested = true;
        }
        else if (ev.button == BUTTON_UP)
        {
            if (s_file_count > 0U)
            {
                s_sel = (s_sel == 0U) ? (s_file_count - 1U) : (s_sel - 1U);
                if (s_sel < s_scroll)
                {
                    s_scroll = s_sel;
                }
                else if (s_sel >= (s_scroll + HS_CRACK_VISIBLE_ROWS))
                {
                    s_scroll = s_sel - (HS_CRACK_VISIBLE_ROWS - 1U);
                }
            }
        }
        else if (ev.button == BUTTON_DOWN)
        {
            if (s_file_count > 0U)
            {
                s_sel = (s_sel + 1U) % s_file_count;
                if (s_sel < s_scroll)
                {
                    s_scroll = s_sel;
                }
                else if (s_sel >= (s_scroll + HS_CRACK_VISIBLE_ROWS))
                {
                    s_scroll = s_sel - (HS_CRACK_VISIBLE_ROWS - 1U);
                }
            }
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_file_count > 0U)
            {
                hs_select_file_(s_sel);
                s_screen = HS_CRACK_SCREEN_CONFIRM;
            }
        }
        break;

    case HS_CRACK_SCREEN_CONFIRM:
        if (ev.button == BUTTON_A)
        {
            if (s_mode == HS_MODE_SDLIST)
            {
                /* The list identity isn't known until the file is picked, so
                 * here we can only shortcut an already-cracked AP; otherwise
                 * open the picker and let hs_decide_ run after selection. */
                char pw[sizeof(s_result.password)];
                if (s_ap_valid && poom_hs_journal_get_cracked(s_ap_bssid, pw, sizeof(pw)))
                {
                    hs_show_saved_(pw);
                }
                else
                {
                    /* Warn about dictionary-attack runtime before the picker. */
                    s_screen = HS_CRACK_SCREEN_SDWARN;
                }
            }
            else
            {
                hs_decide_((s_mode == HS_MODE_QUICK) ? POOM_HS_SCOPE_QUICK
                                                     : POOM_HS_SCOPE_FULL, false);
            }
        }
        else if (ev.button == BUTTON_RIGHT)
        {
            s_mode = (hs_ui_mode_t)((s_mode + 1U) % (unsigned)HS_MODE_COUNT);
        }
        else if (ev.button == BUTTON_LEFT)
        {
            s_mode = (hs_ui_mode_t)((s_mode + (unsigned)HS_MODE_COUNT - 1U) %
                                    (unsigned)HS_MODE_COUNT);
        }
        else if (ev.button == BUTTON_B)
        {
            if (s_direct_mode)
            {
                s_exit_requested = true;
            }
            else
            {
                s_screen = HS_CRACK_SCREEN_BROWSE;
            }
        }
        break;

    case HS_CRACK_SCREEN_ALREADY:
        if (ev.button == BUTTON_A)
        {
            /* Run anyway. The task runs without re-checking, so no re-prompt. */
            if (s_pending_is_sd)
            {
                s_sd_run_requested = true;
            }
            else
            {
                s_scope = s_pending_scope;
                s_start_requested = true;
            }
        }
        else if (ev.button == BUTTON_B)
        {
            s_screen = HS_CRACK_SCREEN_CONFIRM;
        }
        break;

    case HS_CRACK_SCREEN_SDWARN:
        if (ev.button == BUTTON_A)
        {
            s_browse_requested = true; /* acknowledged -> open wordlist picker */
        }
        else if (ev.button == BUTTON_B)
        {
            s_screen = HS_CRACK_SCREEN_CONFIRM;
        }
        break;

    case HS_CRACK_SCREEN_RUNNING:
        if (ev.button == BUTTON_B)
        {
            s_cancel = true; /* engine polls this between passphrases */
        }
        break;

    case HS_CRACK_SCREEN_RESULT:
        if (ev.button == BUTTON_B)
        {
            s_exit_requested = true;
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_can_continue)
            {
                /* Resume: run the rule/ESSID passes the QUICK scan skipped. */
                s_continue_requested = true;
            }
            else if (s_direct_mode)
            {
                s_exit_requested = true;
            }
            else
            {
                s_rescan_requested = true;
                s_screen = HS_CRACK_SCREEN_BROWSE;
            }
        }
        break;

    case HS_CRACK_SCREEN_INFO:
    default:
        if ((ev.button == BUTTON_B) || (ev.button == BUTTON_A))
        {
            if (s_direct_mode)
            {
                s_exit_requested = true;
            }
            else
            {
                s_rescan_requested = true;
                s_screen = HS_CRACK_SCREEN_BROWSE;
            }
        }
        break;
    }

    menu_handshake_crack_request_render_();
}

static void menu_handshake_crack_task_(void *arg)
{
    (void)arg;

    menu_handshake_crack_draw_();

    while (s_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HS_CRACK_REFRESH_MS));

        if (s_exit_requested && (s_screen != HS_CRACK_SCREEN_RUNNING))
        {
            menu_handshake_crack_exit_();
            break;
        }

        if (s_rescan_requested)
        {
            s_rescan_requested = false;
            s_info[0] = '\0';
            hs_scan_dir_();
        }

        if (s_browse_requested && (s_screen != HS_CRACK_SCREEN_RUNNING))
        {
            /* Hand the screen + buttons to the SD browser (mirrors menu_lua):
             * drop our subscription, end this task, and let the browser's
             * callbacks re-enter via menu_handshake_crack_resume_(). */
            s_browse_requested = false;
            if (s_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button",
                                               menu_handshake_crack_button_cb_, "menu_hs_crack");
                s_buttons_subscribed = false;
            }
            s_active = false;
            s_task = NULL;
            menu_handshake_crack_launch_wordlist_browser_();
            vTaskDelete(NULL);
        }

        if (s_start_requested)
        {
            s_start_requested = false;
            s_cancel = false;
            menu_handshake_crack_run_(s_scope);
            if (s_exit_requested)
            {
                menu_handshake_crack_exit_();
                break;
            }
        }

        if (s_continue_requested)
        {
            s_continue_requested = false;
            s_cancel = false;
            menu_handshake_crack_run_(POOM_HS_SCOPE_CONTINUE);
            if (s_exit_requested)
            {
                menu_handshake_crack_exit_();
                break;
            }
        }

        if (s_sd_run_requested)
        {
            s_sd_run_requested = false;
            s_cancel = false;
            menu_handshake_crack_run_wordlist_();
            if (s_exit_requested)
            {
                menu_handshake_crack_exit_();
                break;
            }
        }

        menu_handshake_crack_draw_();
    }

    vTaskDelete(NULL);
}

static void menu_handshake_crack_common_start_(void)
{
    s_active = true;
    s_exit_requested = false;
    s_cancel = false;
    s_start_requested = false;
    s_continue_requested = false;
    s_rescan_requested = false;
    s_can_continue = false;
    s_scope = POOM_HS_SCOPE_QUICK;
    s_mode = HS_MODE_QUICK;
    s_browse_requested = false;
    s_sd_run_requested = false;
    s_wordlist_path[0] = '\0';
    s_wordlist_label[0] = '\0';
    s_ap_valid = false;
    s_from_journal = false;
    s_pending_is_sd = false;
    s_pending_list[0] = '\0';
    memset(s_ap_bssid, 0, sizeof(s_ap_bssid));
    s_ap_essid[0] = '\0';
    memset(&s_result, 0, sizeof(s_result));
    s_tried = 0;
    s_total = poom_hs_crack_wordlist_count();

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_handshake_crack_button_cb_, "menu_hs_crack");
        s_buttons_subscribed = true;
    }

    if (s_task == NULL)
    {
        (void)xTaskCreate(menu_handshake_crack_task_,
                          "menu_hs_crack",
                          HS_CRACK_TASK_STACK,
                          NULL,
                          HS_CRACK_TASK_PRIO,
                          &s_task);
    }

    menu_handshake_crack_request_render_();
}

void menu_handshake_crack_show(void)
{
    s_direct_mode = false;
    s_screen = HS_CRACK_SCREEN_BROWSE;
    s_info[0] = '\0';
    hs_scan_dir_();
    menu_handshake_crack_common_start_();
}

void menu_handshake_crack_show_file(const char *path)
{
    if ((path == NULL) || (path[0] == '\0'))
    {
        menu_handshake_crack_show();
        return;
    }

    s_direct_mode = true;
    (void)snprintf(s_path, sizeof(s_path), "%.159s", path);

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    (void)snprintf(s_label, sizeof(s_label), "%.31s", base);

    s_screen = HS_CRACK_SCREEN_CONFIRM;
    s_info[0] = '\0';
    menu_handshake_crack_common_start_();

    /* Direct mode skips the browser's hs_select_file_, so peek the AP here for
     * the journal (already-cracked / already-tried) checks. */
    s_ap_valid = (poom_hs_crack_peek(s_path, s_ap_bssid, s_ap_essid,
                                     sizeof(s_ap_essid)) == ESP_OK);
}
