// SPDX-License-Identifier: GPL-3.0-or-later
// Device-menu entry for the PicoPass reader. Opens a submenu (Read for now),
// modeled on menu_nfc / menu_fw_info.

#include "menu_picopass.h"
#include "poom_nfc_controller.h"  // release the NFC core on exit
#include "poom_picopass.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC   "poom/menu/resume"
#define MENU_PICOPASS_REFRESH_MS (150U)
#define MENU_PICOPASS_POLL_MS    (120U)  // gap between read attempts while polling
#define MENU_PICOPASS_STACK      (8192U)  // read flow + loclass/DES need the stack
#define MENU_PICOPASS_PRIO       (4U)

#define MAIN_LIST_Y0      (16)
#define MAIN_ROW_STEP     (11)
#define MAIN_ROW_HILITE_H (9)

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

// iCLASS config fuses (config block byte 7): the card is unsecure when the
// crypt fuses read as CRYPT0 alone.
#define PP_FUSE_CRYPT0  0x08
#define PP_FUSE_CRYPT1  0x10
#define PP_FUSE_CRYPT10 (PP_FUSE_CRYPT0 | PP_FUSE_CRYPT1)
// SE/SEOS marker: block 6 byte 0 reads 0x30 on an SIO card.
#define PP_SIO_MARKER   0x30

#define PP_RAW_ROWS_PER_PAGE 5

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_picopass_button_msg_t;

typedef enum
{
    PP_MENU,
    PP_READING,
    PP_RESULT,
    PP_INFO,    // decoded card metadata (security, key, SIO)
    PP_RAW,     // paged raw block hex
    PP_SAVING,  // writing the dump to SD
    PP_SAVED    // save outcome
} pp_state_t;

// Submenu options. Add real entries (Emulate, Write, ...) here as they land.
typedef enum
{
    PP_OPT_READ = 0,
    PP_OPT_COUNT
} pp_opt_t;
static const char* const k_opt_labels[PP_OPT_COUNT] = {"Read"};

static bool s_active                  = false;
static bool s_buttons_subscribed      = false;
static volatile bool s_exit_requested = false;
static volatile bool s_cancel_read = false;  // B while reading: back to submenu
static TaskHandle_t s_task         = NULL;
static char s_sbus_user[]          = "menu_picopass";

static pp_state_t s_state          = PP_MENU;
static pp_opt_t s_opt              = PP_OPT_READ;
static int s_raw_page              = 0;  // current page in the PP_RAW hex view
static esp_err_t s_save_status     = ESP_OK;
static char s_save_path[64]        = {0};
static bool s_sd_ok                = false;  // SD usable (probed on Info entry)
static bool s_sd_probe_pending     = false;
static PoomPicopassStatus s_status = PoomPicopassOk;
static PoomPicopassDump s_dump;

static void menu_picopass_button_cb_(const poom_sbus_msg_t* msg,
                                     void* user_ctx);
static void menu_picopass_task_(void* arg);

static const char* status_short(PoomPicopassStatus s)
{
    switch(s)
    {
        case PoomPicopassErrSelect:
            return "Select fail";
        case PoomPicopassErrReadCheck:
            return "RdChk fail";
        case PoomPicopassErrAuth:
            return "Auth fail";
        case PoomPicopassErrRead:
            return "Read fail";
        case PoomPicopassErrInit:
            return "NFC init fail";
        default:
            return "Error";
    }
}

// A card is "not present yet" (keep polling) vs a hard failure worth reporting.
static bool status_is_no_card(PoomPicopassStatus s)
{
    return (s == PoomPicopassErrNoCard) || (s == PoomPicopassErrIdentify);
}

static const char* save_error_text(esp_err_t e)
{
    switch(e)
    {
        case ESP_ERR_NOT_SUPPORTED:
            return "Format needed";
        case ESP_ERR_NOT_FOUND:
            return "No SD card";
        default:
            return "Save failed";
    }
}

static void pp_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("PICOPASS"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

// Rows for the raw hex view: the four always-read blocks followed by the AA1
// application blocks (6..app_limit) we authenticated to read.
static int pp_raw_row_count(void)
{
    return 4 + s_dump.app_block_count;
}

static const uint8_t* pp_raw_row(int row, char* label, size_t label_sz)
{
    switch(row)
    {
        case 0:
            snprintf(label, label_sz, "CSN");
            return s_dump.csn;
        case 1:
            snprintf(label, label_sz, "CFG");
            return s_dump.config;
        case 2:
            snprintf(label, label_sz, "EPR");
            return s_dump.epurse;
        case 3:
            snprintf(label, label_sz, "AIA");
            return s_dump.aia;
        default:
        {
            int blk = row - 4;
            snprintf(label, label_sz, "B%02d",
                     POOM_PICOPASS_PACS_CFG_BLOCK + blk);
            return s_dump.blocks[blk];
        }
    }
}

static void pp_draw_(void)
{
    poom_arduboy_clear();
    pp_draw_header_();

    if(s_state == PP_MENU)
    {
        for(int row = 0; row < (int)PP_OPT_COUNT; row++)
        {
            const int16_t y =
                (int16_t)(MAIN_LIST_Y0 + (int16_t)row * MAIN_ROW_STEP);
            poom_arduboy_set_cursor(4, y);
            (void)poom_arduboy_print(k_opt_labels[row]);
            if(row == (int)s_opt)
            {
                poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH,
                                       MAIN_ROW_HILITE_H, INVERT);
            }
        }
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:SEL"));
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:EXIT"));
    }
    else if(s_state == PP_READING)
    {
        poom_arduboy_set_cursor(0, 24);
        (void)poom_arduboy_print(F("Reading..."));
        poom_arduboy_set_cursor(0, 34);
        (void)poom_arduboy_print(F("Present a card"));
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:CANCEL"));
    }
    else if(s_state == PP_RESULT)
    {
        char l[24];
        if(s_status != PoomPicopassOk && s_status != PoomPicopassErrAuth)
        {
            poom_arduboy_set_cursor(0, 24);
            (void)poom_arduboy_print(status_short(s_status));
        }
        else
        {
            (void)snprintf(l, sizeof(l), "CSN:%02X%02X%02X%02X%02X%02X%02X%02X",
                           s_dump.csn[0], s_dump.csn[1], s_dump.csn[2],
                           s_dump.csn[3], s_dump.csn[4], s_dump.csn[5],
                           s_dump.csn[6], s_dump.csn[7]);
            poom_arduboy_set_cursor(0, 16);
            (void)poom_arduboy_print(l);

            if(s_dump.wiegand_count > 0)
            {
                // One line per matching format (37-bit yields two).
                for(uint8_t i = 0; i < s_dump.wiegand_count; i++)
                {
                    (void)snprintf(
                        l, sizeof(l), "%s F:%lu C:%llu",
                        s_dump.wiegand[i].format,
                        (unsigned long)s_dump.wiegand[i].facility_code,
                        (unsigned long long)s_dump.wiegand[i].card_number);
                    poom_arduboy_set_cursor(0, (int16_t)(28 + i * 11));
                    (void)poom_arduboy_print(l);
                }
            }
            else if(s_dump.pacs_present)
            {
                // Bad parity on a recognized length shows as e.g. "BITS:26!".
                (void)snprintf(l, sizeof(l), "BITS:%u%s", s_dump.bit_length,
                               s_dump.parity_error ? "!" : "");
                poom_arduboy_set_cursor(0, 30);
                (void)poom_arduboy_print(l);
                // Show only the significant bytes (drop leading zeros).
                int nb = (s_dump.bit_length + 7) / 8;
                if(nb < 1)
                    nb = 1;
                if(nb > 8)
                    nb = 8;
                char* q = l;
                for(int i = 8 - nb; i < 8; i++)
                {
                    q += snprintf(q, sizeof(l) - (size_t)(q - l), "%02X",
                                  s_dump.credential[i]);
                }
                poom_arduboy_set_cursor(0, 42);
                (void)poom_arduboy_print(l);
            }
        }
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:AGAIN"));
        poom_arduboy_set_cursor(46, 56);
        (void)poom_arduboy_print(F("v:INFO"));
        poom_arduboy_set_cursor(92, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }
    else if(s_state == PP_INFO)
    {
        char l[24];
        (void)snprintf(l, sizeof(l), "CSN:%02X%02X%02X%02X%02X%02X%02X%02X",
                       s_dump.csn[0], s_dump.csn[1], s_dump.csn[2],
                       s_dump.csn[3], s_dump.csn[4], s_dump.csn[5],
                       s_dump.csn[6], s_dump.csn[7]);
        poom_arduboy_set_cursor(0, 16);
        (void)poom_arduboy_print(l);

        // Credential summary: SIO-only card, an unusable PACS, or "(bits) hex".
        // block 6 == 0x30 means a pure SIO (SE/SEOS); block 10 == 0x30 next to a
        // legacy PACS means an SR card, shown as "+SIO".
        bool sio =
            (s_dump.app_block_count >= 1) && (s_dump.blocks[0][0] == PP_SIO_MARKER);
        bool sr = (s_dump.app_block_count >= 5) &&
                  (s_dump.blocks[4][0] == PP_SIO_MARKER);
        if(sio)
        {
            (void)snprintf(l, sizeof(l), "SIO");
        }
        else if(!s_dump.pacs_present || s_dump.bit_length == 0 ||
                s_dump.bit_length == 255)
        {
            (void)snprintf(l, sizeof(l), "Invalid PACS");
        }
        else
        {
            int nb = (s_dump.bit_length + 7) / 8;
            if(nb < 1)
                nb = 1;
            if(nb > 8)
                nb = 8;
            char* q = l;
            q += snprintf(q, sizeof(l), "(%u) ", (unsigned)s_dump.bit_length);
            for(int i = 8 - nb; i < 8; i++)
            {
                q += snprintf(q, sizeof(l) - (size_t)(q - l), "%02X",
                              s_dump.credential[i]);
            }
            if(sr)
                (void)snprintf(q, sizeof(l) - (size_t)(q - l), " +SIO");
        }
        poom_arduboy_set_cursor(0, 27);
        (void)poom_arduboy_print(l);

        // Security fuse, then which key authenticated.
        bool unsecure =
            (s_dump.config[7] & PP_FUSE_CRYPT10) == PP_FUSE_CRYPT0;
        poom_arduboy_set_cursor(0, 38);
        (void)poom_arduboy_print(unsecure ? F("Unsecure card") : F("Secure"));
        poom_arduboy_set_cursor(0, 46);
        (void)poom_arduboy_print(s_dump.authenticated ? F("Key: Standard")
                                                       : F("Key: Not Standard"));

        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        // Only offer Save when an SD card is usable.
        if(s_sd_ok)
        {
            poom_arduboy_set_cursor(44, 56);
            (void)poom_arduboy_print(F("^:SAVE"));
        }
        poom_arduboy_set_cursor(92, 56);
        (void)poom_arduboy_print(F("v:RAW"));
    }
    else if(s_state == PP_RAW)
    {
        int total = pp_raw_row_count();
        int pages = (total + PP_RAW_ROWS_PER_PAGE - 1) / PP_RAW_ROWS_PER_PAGE;
        if(pages < 1)
            pages = 1;
        if(s_raw_page >= pages)
            s_raw_page = pages - 1;
        int start = s_raw_page * PP_RAW_ROWS_PER_PAGE;
        for(int i = 0; i < PP_RAW_ROWS_PER_PAGE && (start + i) < total; i++)
        {
            char lbl[6];
            const uint8_t* d = pp_raw_row(start + i, lbl, sizeof(lbl));
            char line[24];
            (void)snprintf(line, sizeof(line),
                           "%s:%02X%02X%02X%02X%02X%02X%02X%02X", lbl, d[0],
                           d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            poom_arduboy_set_cursor(0, (int16_t)(14 + i * 8));
            (void)poom_arduboy_print(line);
        }
        char foot[24];
        (void)snprintf(foot, sizeof(foot), "%d/%d", s_raw_page + 1, pages);
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_set_cursor(56, 56);
        (void)poom_arduboy_print(foot);
        poom_arduboy_set_cursor(100, 56);
        (void)poom_arduboy_print(F("^v"));
    }
    else if(s_state == PP_SAVING)
    {
        poom_arduboy_set_cursor(0, 28);
        (void)poom_arduboy_print(F("Saving..."));
    }
    else if(s_state == PP_SAVED)
    {
        if(s_save_status == ESP_OK)
        {
            poom_arduboy_set_cursor(0, 20);
            (void)poom_arduboy_print(F("Saved"));
            // Show the file name (drop the "/picopass/" directory prefix).
            const char* name = strrchr(s_save_path, '/');
            name             = (name != NULL) ? name + 1 : s_save_path;
            poom_arduboy_set_cursor(0, 32);
            (void)poom_arduboy_print(name);
        }
        else
        {
            poom_arduboy_set_cursor(0, 28);
            (void)poom_arduboy_print(save_error_text(s_save_status));
        }
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }
    poom_arduboy_display();
}

static void menu_picopass_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    s_active                  = false;
    s_exit_requested          = false;

    if(s_task != NULL)
    {
        if(s_task != current_task)
        {
            TaskHandle_t task = s_task;
            s_task            = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_task = NULL;
        }
    }
    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_picopass_button_cb_,
                                       s_sbus_user);
        s_buttons_subscribed = false;
    }
    // Release the NFC core so we don't leave the RF field on after exiting.
    poom_nfc_controller_stop();
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static void menu_picopass_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;
    if((msg == NULL) || (msg->len < sizeof(menu_picopass_button_msg_t)))
        return;
    menu_picopass_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));
    if(ev.event != BUTTON_SINGLE_CLICK)
        return;

    if(s_state == PP_MENU)
    {
        if(ev.button == BTN_B)
        {
            s_exit_requested = true;
        }
        else if(ev.button == BTN_UP)
        {
            if((int)s_opt > 0)
                s_opt = (pp_opt_t)((int)s_opt - 1);
        }
        else if(ev.button == BTN_DOWN)
        {
            if(((int)s_opt + 1) < (int)PP_OPT_COUNT)
                s_opt = (pp_opt_t)((int)s_opt + 1);
        }
        else if(ev.button == BTN_A)
        {
            if(s_opt == PP_OPT_READ)
            {
                s_cancel_read = false;
                s_state       = PP_READING;
            }
        }
    }
    else if(s_state == PP_READING)
    {
        if(ev.button == BTN_B)
            s_cancel_read = true;  // stop polling, back to submenu
    }
    else if(s_state == PP_RESULT)
    {
        if(ev.button == BTN_B)
        {
            s_state = PP_MENU;
        }
        else if(ev.button == BTN_A)
        {
            s_cancel_read = false;
            s_state       = PP_READING;
        }
        else if(ev.button == BTN_DOWN &&
                (s_status == PoomPicopassOk || s_status == PoomPicopassErrAuth))
        {
            s_sd_probe_pending = true;  // task checks SD before drawing Info
            s_state            = PP_INFO;
        }
    }
    else if(s_state == PP_INFO)
    {
        if(ev.button == BTN_B)
            s_state = PP_RESULT;
        else if(ev.button == BTN_UP && s_sd_ok)
            s_state = PP_SAVING;  // task performs the write
        else if(ev.button == BTN_DOWN)
        {
            s_raw_page = 0;
            s_state    = PP_RAW;
        }
    }
    else if(s_state == PP_SAVED)
    {
        if(ev.button == BTN_B)
            s_state = PP_INFO;
    }
    else if(s_state == PP_RAW)
    {
        if(ev.button == BTN_B)
        {
            s_state = PP_INFO;
        }
        else if(ev.button == BTN_UP)
        {
            if(s_raw_page > 0)
                s_raw_page--;
        }
        else if(ev.button == BTN_DOWN)
        {
            int total = pp_raw_row_count();
            int pages =
                (total + PP_RAW_ROWS_PER_PAGE - 1) / PP_RAW_ROWS_PER_PAGE;
            if(pages < 1)
                pages = 1;
            if(s_raw_page + 1 < pages)
                s_raw_page++;
        }
    }
}

static void menu_picopass_task_(void* arg)
{
    (void)arg;
    while(s_active)
    {
        if(s_exit_requested)
        {
            menu_picopass_exit_();
            break;
        }

        if(s_state == PP_READING)
        {
            // Retry until a card is found or the user cancels, so the card can
            // be moved into the field.
            pp_draw_();  // show "Reading..." before the (brief) blocking
                         // attempt
            s_status = poom_picopass_read(&s_dump, NULL, false);
            if(s_cancel_read)
            {
                s_cancel_read = false;
                s_state       = PP_MENU;
            }
            else if(s_status == PoomPicopassOk ||
                    s_status == PoomPicopassErrAuth)
            {
                s_state = PP_RESULT;
            }
            else if(status_is_no_card(s_status))
            {
                vTaskDelay(
                    pdMS_TO_TICKS(MENU_PICOPASS_POLL_MS));  // keep polling
            }
            else
            {
                s_state = PP_RESULT;  // hard error (init/select/etc): report it
            }
            continue;
        }

        if(s_state == PP_SAVING)
        {
            pp_draw_();  // show "Saving..." before the (brief) blocking write
            s_save_status =
                poom_picopass_save(&s_dump, s_save_path, sizeof(s_save_path));
            s_state = PP_SAVED;
            continue;
        }

        // Probe the SD once on entering Info (off the read path) so the Save
        // hint only shows when a card is usable.
        if(s_sd_probe_pending)
        {
            s_sd_ok            = poom_picopass_sd_ready();
            s_sd_probe_pending = false;
        }

        pp_draw_();
        vTaskDelay(pdMS_TO_TICKS(MENU_PICOPASS_REFRESH_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void menu_picopass_show(void)
{
    if(s_task != NULL)
        return;

    s_active         = true;
    s_exit_requested = false;
    s_cancel_read    = false;
    s_state          = PP_MENU;
    s_opt            = PP_OPT_READ;

    if(!s_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_picopass_button_cb_,
                                  s_sbus_user))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            s_active            = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token,
                                    sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_picopass_task_, "menu_picopass", MENU_PICOPASS_STACK,
                      NULL, MENU_PICOPASS_PRIO, &s_task);
}
