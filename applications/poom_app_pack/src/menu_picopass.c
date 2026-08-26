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
    PP_RESULT
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

static void pp_draw_header_(void)
{
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("PICOPASS"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
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
    else
    {  // PP_RESULT
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
        poom_arduboy_set_cursor(72, 56);
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
