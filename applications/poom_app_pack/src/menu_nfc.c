// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_nfc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#include "poom_nfc_dump.h"
#include "poom_nfc_card_ident.h"
#include "poom_nfc_controller.h"
#include "poom_nfc_emulator.h"
#include "poom_nfc_emv.h"
#include "poom_nfc_iso14443_4.h"
#include "poom_nfc_iso7816.h"
#include "poom_nfc_mifare_classic.h"
#include "poom_nfc_store.h"
#include "poom_nfc_tlv.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_NFC_REFRESH_MS (180U)
#define MENU_NFC_UI_POLL_MS (250U)
#define MENU_NFC_STACK (4096U)
#define MENU_NFC_PRIO (4U)

#define MENU_NFC_SCAN_TIMEOUT_MS (2500U)
#define MENU_NFC_SCAN_MAX_FOUND (12U)
#define MENU_NFC_INFO_HOLD_MS (1800U)
#define MENU_NFC_EMV_RETRY_DELAY_MS (80U)
#define MENU_NFC_EMV_PPSE_TRIES (3U)
#define MENU_NFC_EMV_DIRECT_AID_TRIES (2U)
#define MENU_NFC_EMV_APP_MAX (6U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define LIST_Y0 (16)
#define ROW_STEP (12)
#define ROW_HILITE_H (11)
#define VISIBLE_ROWS (3)

#define MAIN_LIST_Y0 (14)
#define MAIN_ROW_STEP (10)
#define MAIN_ROW_HILITE_H (9)

#define MENU_NFC_INFO_Y0 (15)
#define MENU_NFC_INFO_STEP (9)
#define MENU_NFC_SCAN_INFO_MAX_LINES (40U)
#define MENU_NFC_SCAN_SUMMARY_LINES (3U)
#define MENU_NFC_SCAN_INFO_VISIBLE_LINES (4U)
#define MENU_NFC_BUSY_BAR_X (18)
#define MENU_NFC_BUSY_BAR_Y (43)
#define MENU_NFC_BUSY_BAR_W (92)
#define MENU_NFC_BUSY_BAR_H (6)

/* ISO/IEC 14443-4 / NFC-A: SAK bit 5 indicates ISO-DEP capability. */
#define MENU_NFC_NFCA_SAK_ISODEP_MASK (0x20U)

#define MENU_NFC_TRACE(fmt, ...)                                                 \
    do                                                                           \
    {                                                                            \
        if(poom_reader_is_verbose())                                             \
        {                                                                        \
            printf("  [zen-read] " fmt "\r\n", ##__VA_ARGS__);                   \
        }                                                                        \
    } while(0)

#ifndef BTN_A
#define BTN_A (0U)
#endif

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
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
} menu_nfc_button_msg_t;

typedef enum
{
    MENU_NFC_OPT_SCAN = 0,
    MENU_NFC_OPT_EMULATE,
    MENU_NFC_OPT_RESTORE,
    MENU_NFC_OPT_STORAGE,
    MENU_NFC_OPT_COUNT,
} menu_nfc_opt_t;

typedef enum
{
    MENU_NFC_STATE_MAIN = 0,
    MENU_NFC_STATE_SCAN_SCANNING,
    MENU_NFC_STATE_SCAN_RESULT,
    MENU_NFC_STATE_SCAN_INFO,
    MENU_NFC_STATE_SCAN_ACTIONS,
    MENU_NFC_STATE_EMV_APP_SELECT,
    MENU_NFC_STATE_EMULATE_LIST,
    MENU_NFC_STATE_EMULATE_SOURCE,
    MENU_NFC_STATE_EMULATE_SD_LIST,
    MENU_NFC_STATE_EMULATE_RUNNING,
    MENU_NFC_STATE_RESTORE_SD_LIST,
    MENU_NFC_STATE_RESTORE_MODE,
    MENU_NFC_STATE_STORAGE_LIST,
    MENU_NFC_STATE_STORAGE_CONFIRM_DEL,
    MENU_NFC_STATE_STORAGE_CONFIRM_CLEAR,
    MENU_NFC_STATE_INFO,
} menu_nfc_state_t;

static const char *const k_opt_labels[MENU_NFC_OPT_COUNT] = {
    "SCAN",
    "EMULATE",
    "RESTORE MFC",
    "STORAGE",
};

static bool s_menu_nfc_active = false;
static bool s_menu_nfc_buttons_subscribed = false;
static bool s_menu_nfc_exit_requested = false;
static bool s_menu_nfc_scan_requested = false;
static bool s_menu_nfc_restore_requested = false;
static TaskHandle_t s_menu_nfc_ui_task = NULL;
static char s_menu_nfc_sbus_user[] = "menu_nfc";
static bool s_menu_nfc_input_dirty = false;

static menu_nfc_state_t s_state = MENU_NFC_STATE_MAIN;
static menu_nfc_opt_t s_opt = MENU_NFC_OPT_SCAN;

static bool s_yes_selected = true;
static char s_info_line0[22] = "";
static char s_info_line1[22] = "";
static menu_nfc_state_t s_info_return_state = MENU_NFC_STATE_MAIN;
static TickType_t s_info_until_tick = 0;

static uint8_t s_saved_total = 0U;

static poom_nfc_store_t s_store_cache;
static int s_store_selected = 0;
static int s_store_scroll = 0;

static poom_nfc_dump_t s_scan_dump;
static bool s_scan_dump_valid = false;
static bool s_scan_has_ndef = false;
static bool s_scan_ndef_known = false;

typedef enum
{
    MENU_NFC_SCAN_KIND_GENERIC = 0,
    MENU_NFC_SCAN_KIND_T2T,
    MENU_NFC_SCAN_KIND_MIFARE_BASIC,
    MENU_NFC_SCAN_KIND_MIFARE_READ,
    MENU_NFC_SCAN_KIND_ISODEP,
    MENU_NFC_SCAN_KIND_EMV,
} menu_nfc_scan_kind_t;

#define MENU_NFC_SCAN_ACTION_MAX (6U)

typedef enum
{
    MENU_NFC_SCAN_ACT_CARD_INFO = 0,
    MENU_NFC_SCAN_ACT_DETAILS,
    MENU_NFC_SCAN_ACT_READ_CARD,
    MENU_NFC_SCAN_ACT_READ_AGAIN,
    MENU_NFC_SCAN_ACT_CHECK_PAYMENT,
    MENU_NFC_SCAN_ACT_SAVE_DUMP,
    MENU_NFC_SCAN_ACT_SAVE_SUMMARY,
    MENU_NFC_SCAN_ACT_SAVE_ID,
    MENU_NFC_SCAN_ACT_EMULATE,
    MENU_NFC_SCAN_ACT_ADVANCED,
    MENU_NFC_SCAN_ACT_SCAN_AGAIN,
} menu_nfc_scan_action_t;

typedef struct
{
    uint8_t aid[POOM_NFC_EMV_AID_MAX_LEN];
    size_t aid_len;
    char label[22];
    bool has_priority;
    uint8_t priority;
} menu_nfc_emv_candidate_t;

typedef struct
{
    menu_nfc_scan_kind_t kind;
    char summary_lines[MENU_NFC_SCAN_SUMMARY_LINES][22];
    char info_lines[MENU_NFC_SCAN_INFO_MAX_LINES][22];
    poom_nfc_profile_t profile;
    uint8_t info_count;
    uint8_t info_scroll;
    uint8_t action_count;
    uint8_t action_sel;
    uint8_t action_scroll;
    bool profile_valid;
    bool emv_data_valid;
    poom_nfc_emv_card_t* emv_data;
    menu_nfc_emv_candidate_t* emv_apps;
    uint8_t emv_app_count;
    uint8_t emv_app_sel;
    uint8_t emv_app_scroll;
    menu_nfc_scan_action_t actions[MENU_NFC_SCAN_ACTION_MAX];
} menu_nfc_scan_meta_t;

static menu_nfc_scan_meta_t* s_scan_meta = NULL;

typedef enum
{
    MENU_NFC_EMU_SRC_EMBEDDED = 0,
    MENU_NFC_EMU_SRC_SD,
    MENU_NFC_EMU_SRC_T4T_DEFAULT,
    MENU_NFC_EMU_SRC_COUNT,
} menu_nfc_emu_source_t;

static menu_nfc_emu_source_t s_emu_source_sel = MENU_NFC_EMU_SRC_EMBEDDED;
static bool s_emu_source_sd = false;
static poom_nfc_card_id_t s_emu_active_id;
static bool s_emu_active_id_valid = false;
static menu_nfc_state_t s_emu_running_return_state = MENU_NFC_STATE_EMULATE_LIST;

#define MENU_NFC_SD_MAX_FILES (16)
#define MENU_NFC_SD_NAME_MAX (64)
typedef enum
{
    MENU_NFC_SD_ITEM_NFC_DUMP = 0,
    MENU_NFC_SD_ITEM_MFC_DUMP,
} menu_nfc_sd_item_kind_t;

typedef struct
{
    char name[MENU_NFC_SD_NAME_MAX];
    char rel_path[96];
    menu_nfc_sd_item_kind_t kind;
} menu_nfc_sd_item_t;

static menu_nfc_sd_item_t s_sd_dump_files[MENU_NFC_SD_MAX_FILES];
static int s_sd_dump_count = 0;
static int s_sd_dump_selected = 0;
static int s_sd_dump_scroll = 0;
static char s_restore_rel_path[96];
static bool s_restore_write_trailers = false;

static const char *menu_nfc_scan_card_short_(const poom_nfc_dump_t *dump);
static const char *menu_nfc_mfr_abbr_(const poom_nfc_dump_t *dump);
static void menu_nfc_format_uid_compact_(
    const poom_nfc_card_id_t *id, char *out, size_t out_len);
static bool menu_nfc_scan_meta_acquire_(void);
static void menu_nfc_scan_meta_release_(void);
static bool menu_nfc_emu_supported_(const poom_nfc_card_id_t *id);
static void menu_nfc_prepare_generic_scan_view_(void);
static void menu_nfc_scan_save_to_sd_(void);
static void menu_nfc_scan_save_embedded_(void);
static menu_nfc_state_t menu_nfc_scan_primary_state_(void);
static bool menu_nfc_scan_use_combined_info_(void);
static uint8_t menu_nfc_scan_summary_count_(void);
static uint8_t menu_nfc_scan_total_info_lines_(void);
static const char* menu_nfc_scan_info_line_at_(uint8_t idx);
static void menu_nfc_emu_start_id_(
    const poom_nfc_card_id_t *id, bool from_sd, menu_nfc_state_t fail_return_state);
static void menu_nfc_run_restore_(void);
static void menu_nfc_set_info_return_(
    const char *l0, const char *l1, menu_nfc_state_t return_state);
static void menu_nfc_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_nfc_ui_task_(void *arg);

/**
 * @brief Internal helper for `menu_nfc_request_redraw`.
 *
 * @return void
 */
static void menu_nfc_request_redraw_(void)
{
    s_menu_nfc_input_dirty = true;
    if (s_menu_nfc_ui_task != NULL)
    {
        (void)xTaskNotifyGive(s_menu_nfc_ui_task);
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_draw_frame_(const char *title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(46, 2);
    (void)poom_arduboy_print(title ? title : "NFC");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Internal helper for `menu_nfc_type_char`.
 *
 * @param[in] type Parameter passed to the helper.
 * @return char
 */
static char menu_nfc_type_char_(uint8_t type)
{
    switch (type)
    {
        case 0U:
        case 10U:
            return 'A';
        case 1U:
        case 11U:
            return 'B';
        case 2U:
        case 12U:
            return 'F';
        case 3U:
        case 13U:
            return 'V';
        case 4U:
            return 'T';
        default:   return '?';
    }
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] id Parameter passed to the helper.
 * @param[in] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_format_uid_hex_(const poom_nfc_card_id_t *id, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }

    out[0] = '\0';
    if (id == NULL)
    {
        return;
    }

    size_t w = 0U;
    for (uint8_t i = 0U; (i < id->uid_len) && (w + 2U < out_len); i++)
    {
        w += (size_t)snprintf(&out[w], out_len - w, "%02X", id->uid[i]);
    }
}

/**
 * @brief Clears the scan-specific summary/info state.
 *
 * @return void
 */
static void menu_nfc_scan_meta_reset_(void)
{
    if(s_scan_meta == NULL)
    {
        return;
    }

    if(s_scan_meta->emv_data != NULL)
    {
        poom_nfc_emv_card_release(s_scan_meta->emv_data);
        free(s_scan_meta->emv_data);
    }
    free(s_scan_meta->emv_apps);
    (void)memset(s_scan_meta, 0, sizeof(*s_scan_meta));
    s_scan_meta->kind = MENU_NFC_SCAN_KIND_GENERIC;
}

/**
 * @brief Allocates the scan-specific UI state on demand.
 *
 * @return true when the state is ready for use.
 */
static bool menu_nfc_scan_meta_acquire_(void)
{
    if(s_scan_meta == NULL)
    {
        s_scan_meta = (menu_nfc_scan_meta_t*)calloc(1U, sizeof(*s_scan_meta));
        if(s_scan_meta == NULL)
        {
            return false;
        }
    }

    menu_nfc_scan_meta_reset_();
    return true;
}

/**
 * @brief Releases the scan-specific UI state.
 *
 * @return void
 */
static void menu_nfc_scan_meta_release_(void)
{
    if(s_scan_meta != NULL)
    {
        if(s_scan_meta->emv_data != NULL)
        {
            poom_nfc_emv_card_release(s_scan_meta->emv_data);
            free(s_scan_meta->emv_data);
        }
        free(s_scan_meta->emv_apps);
        free(s_scan_meta);
        s_scan_meta = NULL;
    }
}

/**
 * @brief Resets the dynamic action list for the current scan context.
 *
 * @return void
 */
static void menu_nfc_scan_actions_reset_(void)
{
    if(s_scan_meta == NULL)
    {
        return;
    }

    (void)memset(s_scan_meta->actions, 0, sizeof(s_scan_meta->actions));
    s_scan_meta->action_count = 0U;
    s_scan_meta->action_sel = 0U;
    s_scan_meta->action_scroll = 0U;
}

/**
 * @brief Appends one action to the current scan action list.
 *
 * @param[in] action Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_scan_action_add_(menu_nfc_scan_action_t action)
{
    if((s_scan_meta == NULL) || (s_scan_meta->action_count >= MENU_NFC_SCAN_ACTION_MAX))
    {
        return;
    }

    s_scan_meta->actions[s_scan_meta->action_count++] = action;
}

/**
 * @brief Returns the currently selected scan action.
 *
 * @return menu_nfc_scan_action_t
 */
static menu_nfc_scan_action_t menu_nfc_scan_action_selected_(void)
{
    if((s_scan_meta == NULL) || (s_scan_meta->action_count == 0U) ||
       (s_scan_meta->action_sel >= s_scan_meta->action_count))
    {
        return MENU_NFC_SCAN_ACT_CARD_INFO;
    }

    return s_scan_meta->actions[s_scan_meta->action_sel];
}

/**
 * @brief Adjusts the action selection/scroll window like the other list menus.
 *
 * @return void
 */
static void menu_nfc_scan_actions_adjust_scroll_(void)
{
    if(s_scan_meta == NULL)
    {
        return;
    }

    if(s_scan_meta->action_count == 0U)
    {
        s_scan_meta->action_sel = 0U;
        s_scan_meta->action_scroll = 0U;
        return;
    }

    if(s_scan_meta->action_sel >= s_scan_meta->action_count)
    {
        s_scan_meta->action_sel = (uint8_t)(s_scan_meta->action_count - 1U);
    }

    if(s_scan_meta->action_sel < s_scan_meta->action_scroll)
    {
        s_scan_meta->action_scroll = s_scan_meta->action_sel;
    }
    if(s_scan_meta->action_sel >= (uint8_t)(s_scan_meta->action_scroll + VISIBLE_ROWS))
    {
        s_scan_meta->action_scroll = (uint8_t)(s_scan_meta->action_sel - VISIBLE_ROWS + 1U);
    }

    {
        uint8_t max_scroll = 0U;
        if(s_scan_meta->action_count > VISIBLE_ROWS)
        {
            max_scroll = (uint8_t)(s_scan_meta->action_count - VISIBLE_ROWS);
        }
        if(s_scan_meta->action_scroll > max_scroll)
        {
            s_scan_meta->action_scroll = max_scroll;
        }
    }
}

/**
 * @brief Formats arbitrary bytes as compact hex for one UI line.
 *
 * @param[in] data Parameter passed to the helper.
 * @param[in] data_len Parameter passed to the helper.
 * @param[out] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_format_hex_compact_(const uint8_t* data, size_t data_len, char* out, size_t out_len)
{
    size_t w = 0U;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }

    out[0] = '\0';
    if((data == NULL) || (data_len == 0U))
    {
        return;
    }

    for(size_t i = 0U; i < data_len; i++)
    {
        const int n = snprintf(&out[w], out_len - w, "%02X", data[i]);
        if((n <= 0) || ((size_t)n >= (out_len - w)))
        {
            out[out_len - 1U] = '\0';
            return;
        }
        w += (size_t)n;
    }
}

/**
 * @brief Copies one string into a fixed 21-char UI line.
 *
 * @param[in] src Parameter passed to the helper.
 * @param[out] out Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_line_copy_(const char* src, char out[22])
{
    if(out == NULL)
    {
        return;
    }

    (void)snprintf(out, 22U, "%.21s", (src != NULL) ? src : "");
}

/**
 * @brief Appends one scan info line when space remains.
 *
 * @param[in] text Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_scan_info_add_(const char* text)
{
    if((s_scan_meta == NULL) || (s_scan_meta->info_count >= MENU_NFC_SCAN_INFO_MAX_LINES))
    {
        return;
    }

    menu_nfc_line_copy_(text, s_scan_meta->info_lines[s_scan_meta->info_count]);
    s_scan_meta->info_count++;
}

/**
 * @brief Internal helper for `menu_nfc_scan_type_from_dump`.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return nfc_card_type_t
 */
static nfc_card_type_t menu_nfc_scan_type_from_dump_(const poom_nfc_dump_t* dump)
{
    if((dump == NULL) || !poom_nfc_card_id_is_valid(&dump->id))
    {
        return NFC_CARD_UNKNOWN;
    }

    if(((dump->id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U) ||
       ((dump->id.flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U))
    {
        return NFC_CARD_UNKNOWN;
    }

    return nfc_ident_detect_nfca(
        (uint16_t)(((uint16_t)dump->id.atqa[1] << 8) | (uint16_t)dump->id.atqa[0]),
        dump->id.sak);
}

/**
 * @brief Reports whether the scanned NFC-A card advertises ISO-DEP support.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return true when SAK marks ISO-DEP support.
 */
static bool menu_nfc_scan_is_isodep_candidate_(const poom_nfc_dump_t* dump)
{
    if((dump == NULL) || !poom_nfc_card_id_is_valid(&dump->id))
    {
        return false;
    }

    if(((dump->id.flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U) ||
       ((dump->id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U))
    {
        return false;
    }

    return (dump->id.sak & MENU_NFC_NFCA_SAK_ISODEP_MASK) != 0U;
}

/**
 * @brief Decides whether a direct EMV AID probe is appropriate for a card type.
 *
 * This is intentionally conservative: if the NFC-A heuristics already point to
 * a specific family such as MIFARE, we prefer not to relabel it as EMV unless
 * PPSE discovery succeeds first.
 *
 * @param[in] card_type Parameter passed to the helper.
 * @return true when direct AID probing is allowed.
 */
static bool menu_nfc_allow_direct_emv_aid_fallback_(nfc_card_type_t card_type)
{
    switch(card_type)
    {
        case NFC_CARD_UNKNOWN:
        case NFC_CARD_OTHER:
        case NFC_CARD_JCOP:
            return true;

        default:
            return false;
    }
}

/**
 * @brief Builds a small ID-only dump from the last ISO-DEP activation profile.
 *
 * @param[in] profile Parameter passed to the helper.
 * @param[out] out_dump Parameter passed to the helper.
 * @return true when the profile contains enough NFC-A identity data.
 */
static bool menu_nfc_fill_dump_from_profile_(
    const poom_nfc_profile_t* profile, poom_nfc_dump_t* out_dump)
{
    if((profile == NULL) || (out_dump == NULL) || !poom_nfc_profile_has_uid(profile))
    {
        return false;
    }

    (void)memset(out_dump, 0, sizeof(*out_dump));
    out_dump->page_size = POOM_NFC_DUMP_PAGE_SIZE;
    out_dump->read_mode = POOM_NFC_DUMP_READ_ID_ONLY;
    out_dump->read_ok = true;
    out_dump->id.type = 0U;
    out_dump->id.uid_len = profile->uid_len;
    (void)memcpy(out_dump->id.uid, profile->uid, profile->uid_len);

    if(profile->atqa_set)
    {
        out_dump->id.flags |= POOM_NFC_CARD_FLAG_ATQA_SET;
        out_dump->id.atqa[0] = profile->atqa[0];
        out_dump->id.atqa[1] = profile->atqa[1];
    }
    if(profile->sak_set)
    {
        out_dump->id.flags |= POOM_NFC_CARD_FLAG_SAK_SET;
        out_dump->id.sak = profile->sak;
    }

    return poom_nfc_card_id_is_valid(&out_dump->id);
}

/**
 * @brief Formats EMV AID text with a compact middle ellipsis when needed.
 *
 * @param[in] aid Parameter passed to the helper.
 * @param[in] aid_len Parameter passed to the helper.
 * @param[out] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emv_format_aid_(const uint8_t* aid, size_t aid_len, char* out, size_t out_len)
{
    char full[65];
    size_t w = 0U;

    if(out == NULL || out_len == 0U)
    {
        return;
    }

    out[0] = '\0';
    if(aid == NULL || aid_len == 0U)
    {
        return;
    }

    for(size_t i = 0U; (i < aid_len) && ((w + 2U) < sizeof(full)); i++)
    {
        w += (size_t)snprintf(&full[w], sizeof(full) - w, "%02X", aid[i]);
    }
    full[sizeof(full) - 1U] = '\0';

    if(strlen(full) < out_len)
    {
        (void)snprintf(out, out_len, "%s", full);
        return;
    }

    if(out_len <= 9U)
    {
        (void)snprintf(out, out_len, "%.8s", full);
        return;
    }

    (void)snprintf(out, out_len, "%.4s...%.4s", full, &full[strlen(full) - 4U]);
}

static void menu_nfc_emv_format_masked_pan_(const poom_nfc_emv_card_t* card,
                                            char* out,
                                            size_t out_len)
{
    if(out == NULL || out_len == 0U)
    {
        return;
    }
    out[0] = '\0';
    if(card == NULL || card->pan_len < 4U)
    {
        return;
    }

    (void)snprintf(out, out_len, "PAN:********%s", &card->pan[card->pan_len - 4U]);
}

/**
 * @brief Copies printable EMV bytes into a compact UI string.
 *
 * @param[in] src Parameter passed to the helper.
 * @param[in] src_len Parameter passed to the helper.
 * @param[out] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emv_copy_ascii_(const uint8_t* src, size_t src_len, char* out, size_t out_len)
{
    size_t n;

    if(out == NULL || out_len == 0U)
    {
        return;
    }

    out[0] = '\0';
    if(src == NULL || src_len == 0U)
    {
        return;
    }

    n = (src_len < (out_len - 1U)) ? src_len : (out_len - 1U);
    for(size_t i = 0U; i < n; i++)
    {
        char c = (char)src[i];
        out[i] = ((c >= 0x20) && (c <= 0x7E)) ? c : '?';
    }
    out[n] = '\0';
}

/**
 * @brief Formats EMV language preference bytes (`5F2D`) into "es pt en".
 *
 * @param[in] src Parameter passed to the helper.
 * @param[in] src_len Parameter passed to the helper.
 * @param[out] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emv_format_langs_(const uint8_t* src, size_t src_len, char* out, size_t out_len)
{
    size_t w = 0U;

    if(out == NULL || out_len == 0U)
    {
        return;
    }

    out[0] = '\0';
    if(src == NULL || src_len < 2U)
    {
        return;
    }

    for(size_t i = 0U; (i + 1U) < src_len; i += 2U)
    {
        const char* sep = (w == 0U) ? "" : " ";
        int n = snprintf(&out[w], out_len - w, "%s%c%c", sep, (char)src[i], (char)src[i + 1U]);
        if(n <= 0 || (size_t)n >= (out_len - w))
        {
            out[out_len - 1U] = '\0';
            return;
        }
        w += (size_t)n;
    }
}

/**
 * @brief Returns a short EMV network name inferred from the AID RID.
 *
 * @param[in] aid Parameter passed to the helper.
 * @param[in] aid_len Parameter passed to the helper.
 * @return const char * Static brand label or NULL when unknown.
 */
static const char* menu_nfc_emv_brand_from_aid_(const uint8_t* aid, size_t aid_len)
{
    if((aid == NULL) || (aid_len < 5U))
    {
        return NULL;
    }

    if((aid[0] == 0xA0U) && (aid[1] == 0x00U) && (aid[2] == 0x00U) && (aid[3] == 0x00U))
    {
        switch(aid[4])
        {
            case 0x03U: return "VISA";
            case 0x04U: return "MASTERCARD";
            case 0x25U: return "AMEX";
            case 0x65U: return "JCB";
            case 0x24U: return "DINERS";
            default:    break;
        }
    }

    return NULL;
}

typedef menu_nfc_emv_candidate_t menu_nfc_emv_first_app_t;

typedef struct
{
    menu_nfc_emv_candidate_t* apps;
    size_t capacity;
    size_t count;
} menu_nfc_emv_collect_ctx_t;

typedef struct
{
    uint8_t label[POOM_NFC_EMV_LABEL_MAX_LEN];
    size_t label_len;
    uint8_t name[POOM_NFC_EMV_NAME_MAX_LEN];
    size_t name_len;
    uint8_t langs[16];
    size_t langs_len;
    bool has_priority;
    uint8_t priority;
} menu_nfc_emv_select_meta_t;

typedef struct
{
    const uint8_t* aid;
    size_t aid_len;
} menu_nfc_emv_probe_aid_t;

enum
{
    MENU_NFC_EMV_TAG_LABEL = 0x50U,
    MENU_NFC_EMV_TAG_PRIORITY = 0x87U,
    MENU_NFC_EMV_TAG_PREFERRED_NAME = 0x9F12U,
    MENU_NFC_EMV_TAG_LANGUAGE_PREF = 0x5F2DU,
};

/* Common EMV application AIDs used as a fast fallback when PPSE is absent or flaky. */
static const uint8_t k_menu_nfc_emv_aid_visa_credit_debit[] = {
    0xA0U, 0x00U, 0x00U, 0x00U, 0x03U, 0x10U, 0x10U,
};

static const uint8_t k_menu_nfc_emv_aid_mastercard_credit_debit[] = {
    0xA0U, 0x00U, 0x00U, 0x00U, 0x04U, 0x10U, 0x10U,
};

static const uint8_t k_menu_nfc_emv_aid_maestro[] = {
    0xA0U, 0x00U, 0x00U, 0x00U, 0x04U, 0x30U, 0x60U,
};

static const menu_nfc_emv_probe_aid_t k_menu_nfc_emv_probe_aids[] = {
    {k_menu_nfc_emv_aid_visa_credit_debit, sizeof(k_menu_nfc_emv_aid_visa_credit_debit)},
    {k_menu_nfc_emv_aid_mastercard_credit_debit, sizeof(k_menu_nfc_emv_aid_mastercard_credit_debit)},
    {k_menu_nfc_emv_aid_maestro, sizeof(k_menu_nfc_emv_aid_maestro)},
};

/**
 * @brief Captures the first EMV app announced by PPSE.
 *
 * @param[in] app Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_emv_collect_app_cb_(const poom_nfc_emv_app_t* app, void* user_ctx)
{
    menu_nfc_emv_collect_ctx_t* ctx = (menu_nfc_emv_collect_ctx_t*)user_ctx;

    if(app == NULL || ctx == NULL || ctx->apps == NULL || app->aid_len == 0U ||
       app->aid_len > POOM_NFC_EMV_AID_MAX_LEN)
    {
        return false;
    }

    for(size_t i = 0U; i < ctx->count; i++)
    {
        if(ctx->apps[i].aid_len == app->aid_len &&
           memcmp(ctx->apps[i].aid, app->aid, app->aid_len) == 0)
        {
            return true;
        }
    }
    if(ctx->count >= ctx->capacity)
    {
        return true;
    }

    menu_nfc_emv_candidate_t* candidate = &ctx->apps[ctx->count++];
    candidate->aid_len =
        (app->aid_len <= sizeof(candidate->aid)) ? app->aid_len : sizeof(candidate->aid);
    (void)memcpy(candidate->aid, app->aid, candidate->aid_len);
    menu_nfc_emv_copy_ascii_(app->label, app->label_len, candidate->label, sizeof(candidate->label));
    candidate->has_priority = app->has_priority;
    candidate->priority = app->priority;
    return true;
}

/**
 * @brief Recursively collects a few EMV FCI tags used by the Zen summary.
 *
 * @param[in] buf Parameter passed to the helper.
 * @param[in] buf_len Parameter passed to the helper.
 * @param[in,out] meta Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emv_collect_meta_(const uint8_t* buf, size_t buf_len, menu_nfc_emv_select_meta_t* meta)
{
    size_t off = 0U;
    poom_tlv_view_t tlv;

    if(buf == NULL || meta == NULL)
    {
        return;
    }

    while(poom_tlv_next(buf, buf_len, &off, &tlv))
    {
        if((tlv.tag == MENU_NFC_EMV_TAG_LABEL) && (meta->label_len == 0U))
        {
            meta->label_len =
                (tlv.value_len < sizeof(meta->label)) ? tlv.value_len : sizeof(meta->label);
            (void)memcpy(meta->label, tlv.value, meta->label_len);
        }
        else if((tlv.tag == MENU_NFC_EMV_TAG_PREFERRED_NAME) && (meta->name_len == 0U))
        {
            meta->name_len =
                (tlv.value_len < sizeof(meta->name)) ? tlv.value_len : sizeof(meta->name);
            (void)memcpy(meta->name, tlv.value, meta->name_len);
        }
        else if((tlv.tag == MENU_NFC_EMV_TAG_LANGUAGE_PREF) && (meta->langs_len == 0U))
        {
            meta->langs_len =
                (tlv.value_len < sizeof(meta->langs)) ? tlv.value_len : sizeof(meta->langs);
            (void)memcpy(meta->langs, tlv.value, meta->langs_len);
        }
        else if((tlv.tag == MENU_NFC_EMV_TAG_PRIORITY) && !meta->has_priority && (tlv.value_len > 0U))
        {
            meta->has_priority = true;
            meta->priority = tlv.value[0];
        }

        if(tlv.constructed && tlv.value_len > 0U)
        {
            menu_nfc_emv_collect_meta_(tlv.value, tlv.value_len, meta);
        }
    }
}

/**
 * @brief Try a short list of common EMV AIDs when PPSE discovery is unavailable.
 *
 * @param[out] out_first Parameter passed to the helper.
 * @param[out] out_meta Parameter passed to the helper.
 * @param[out] out_aid_select_ok Parameter passed to the helper.
 * @return true when one direct AID select succeeds with `90 00`.
 */
static bool menu_nfc_emv_try_direct_aids_(
    menu_nfc_emv_first_app_t* out_first,
    menu_nfc_emv_select_meta_t* out_meta,
    bool* out_aid_select_ok)
{
    uint8_t rapdu[260];
    size_t rapdu_len = 0U;
    poom_iso7816_rapdu_view_t view;
    char aid_text[22];

    if((out_first == NULL) || (out_meta == NULL) || (out_aid_select_ok == NULL))
    {
        return false;
    }

    *out_aid_select_ok = false;
    (void)memset(out_first, 0, sizeof(*out_first));
    (void)memset(out_meta, 0, sizeof(*out_meta));

    for(uint8_t pass = 0U; pass < MENU_NFC_EMV_DIRECT_AID_TRIES; pass++)
    {
        for(size_t i = 0U; i < (sizeof(k_menu_nfc_emv_probe_aids) / sizeof(k_menu_nfc_emv_probe_aids[0])); i++)
        {
            const menu_nfc_emv_probe_aid_t* probe = &k_menu_nfc_emv_probe_aids[i];
            menu_nfc_emv_format_aid_(probe->aid, probe->aid_len, aid_text, sizeof(aid_text));
            MENU_NFC_TRACE(
                "emv aid try=%u/%u aid=%s",
                (unsigned)(pass + 1U),
                (unsigned)MENU_NFC_EMV_DIRECT_AID_TRIES,
                aid_text);

            if(!poom_nfc_emv_select_aid(probe->aid, probe->aid_len, rapdu, sizeof(rapdu), &rapdu_len))
            {
                MENU_NFC_TRACE("emv aid link fail aid=%s", aid_text);
                continue;
            }
            if(!poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view) ||
               !poom_iso7816_status_is_ok(view.sw1, view.sw2))
            {
                if(poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view))
                {
                    MENU_NFC_TRACE(
                        "emv aid reject aid=%s sw=%02X%02X",
                        aid_text,
                        (unsigned)view.sw1,
                        (unsigned)view.sw2);
                }
                else
                {
                    MENU_NFC_TRACE("emv aid parse fail aid=%s", aid_text);
                }
                continue;
            }

            out_first->aid_len =
                (probe->aid_len <= sizeof(out_first->aid)) ? probe->aid_len : sizeof(out_first->aid);
            (void)memcpy(out_first->aid, probe->aid, out_first->aid_len);
            menu_nfc_emv_collect_meta_(view.data, view.data_len, out_meta);
            *out_aid_select_ok = true;
            MENU_NFC_TRACE("emv aid accept aid=%s", aid_text);
            return true;
        }

        if((pass + 1U) < MENU_NFC_EMV_DIRECT_AID_TRIES)
        {
            vTaskDelay(pdMS_TO_TICKS(MENU_NFC_EMV_RETRY_DELAY_MS));
        }
    }

    return false;
}

/**
 * @brief Shows a short busy screen for longer read/save stages.
 *
 * @param[in] line0 Parameter passed to the helper.
 * @param[in] line1 Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_draw_busy_(const char* line0, const char* line1)
{
    menu_nfc_draw_frame_("NFC");
    poom_arduboy_set_cursor(4, 22);
    (void)poom_arduboy_print(line0 ? line0 : "");
    poom_arduboy_set_cursor(4, 34);
    (void)poom_arduboy_print(line1 ? line1 : "");
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Shows a short busy screen with a small staged progress bar.
 *
 * @param[in] line0 Parameter passed to the helper.
 * @param[in] line1 Parameter passed to the helper.
 * @param[in] step Parameter passed to the helper.
 * @param[in] total Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_draw_busy_progress_(
    const char* line0, const char* line1, uint16_t step, uint16_t total)
{
    uint8_t fill_w = 0U;

    menu_nfc_draw_frame_("NFC");
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(line0 ? line0 : "");
    poom_arduboy_set_cursor(4, 31);
    (void)poom_arduboy_print(line1 ? line1 : "");

    poom_arduboy_draw_rect(MENU_NFC_BUSY_BAR_X, MENU_NFC_BUSY_BAR_Y, MENU_NFC_BUSY_BAR_W, MENU_NFC_BUSY_BAR_H, WHITE);
    if((total > 0U) && (step > 0U))
    {
        if(step > total)
        {
            step = total;
        }
        fill_w = (uint8_t)(((uint16_t)(MENU_NFC_BUSY_BAR_W - 2) * (uint16_t)step) / (uint16_t)total);
        if(fill_w > 0U)
        {
            poom_arduboy_fill_rect(
                MENU_NFC_BUSY_BAR_X + 1,
                MENU_NFC_BUSY_BAR_Y + 1,
                fill_w,
                MENU_NFC_BUSY_BAR_H - 2,
                WHITE);
        }
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Refreshes the internal state used by this menu module.
 *
 * @return void
 */
static void menu_nfc_refresh_saved_count_(void)
{
    poom_nfc_store_t store;
    if (poom_nfc_store_load(&store) == ESP_OK)
    {
        s_saved_total = store.count;
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_main_(void)
{
    menu_nfc_draw_frame_("NFC");

    for (int row = 0; row < (int)MENU_NFC_OPT_COUNT; row++)
    {
        const int16_t y = (int16_t)(MAIN_LIST_Y0 + (int16_t)row * MAIN_ROW_STEP);
        const char *label = k_opt_labels[row];

        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(label);

        if (row == (int)s_opt)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MAIN_ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_scanning_(void)
{
    menu_nfc_draw_frame_("NFC");

    poom_arduboy_set_cursor(18, 30);
    (void)poom_arduboy_print(F("Scanning..."));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the footer used by scan result/info views.
 *
 * @param[in] show_up Parameter passed to the helper.
 * @param[in] show_down Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_draw_scan_footer_(bool show_up, bool show_down)
{
    const bool save_direct =
        (s_scan_meta != NULL) && (s_scan_meta->kind == MENU_NFC_SCAN_KIND_EMV);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(save_direct ? F("A:SAVE") : F("A:OPT"));

    if(show_up)
    {
        poom_arduboy_fill_triangle(51, 55, 47, 60, 55, 60, WHITE);
    }
    if(show_down)
    {
        poom_arduboy_fill_triangle(59, 55, 67, 55, 63, 60, WHITE);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_nfc_scan_mode_str_(const poom_nfc_dump_t *dump)
{
    if((dump != NULL) && (dump->read_mode == POOM_NFC_DUMP_READ_FULL))
    {
        return "DUMP";
    }
    return "ID";
}

/**
 * @brief Returns the UI label used for one scan action.
 *
 * @param[in] action Parameter passed to the helper.
 * @return const char *
 */
static const char* menu_nfc_scan_action_label_(menu_nfc_scan_action_t action)
{
    switch(action)
    {
        case MENU_NFC_SCAN_ACT_CARD_INFO:     return "Card info";
        case MENU_NFC_SCAN_ACT_DETAILS:       return "Details";
        case MENU_NFC_SCAN_ACT_READ_CARD:     return "Unlock";
        case MENU_NFC_SCAN_ACT_READ_AGAIN:    return "Unlock again";
        case MENU_NFC_SCAN_ACT_CHECK_PAYMENT: return "Check payment";
        case MENU_NFC_SCAN_ACT_SAVE_DUMP:     return "Save dump";
        case MENU_NFC_SCAN_ACT_SAVE_SUMMARY:  return "Save summary";
        case MENU_NFC_SCAN_ACT_SAVE_ID:       return "Save ID";
        case MENU_NFC_SCAN_ACT_EMULATE:       return "Emulate";
        case MENU_NFC_SCAN_ACT_ADVANCED:      return "Advanced";
        case MENU_NFC_SCAN_ACT_SCAN_AGAIN:    return "Scan again";
        default:                              return "";
    }
}

/**
 * @brief Adds common identity lines for the current scan to the info view.
 *
 * @param[in] type_line Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_scan_info_add_identity_(const char* type_line)
{
    char hex[POOM_NFC_CARD_UID_MAX * 2U + 1U];
    char line[22];

    if(type_line != NULL)
    {
        menu_nfc_scan_info_add_(type_line);
    }

    menu_nfc_format_uid_hex_(&s_scan_dump.id, hex, sizeof(hex));
    if(hex[0] != '\0')
    {
        (void)snprintf(line, sizeof(line), "UID:%.17s", hex);
        menu_nfc_scan_info_add_(line);
    }

    if((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U)
    {
        (void)snprintf(line,
                       sizeof(line),
                       "ATQA:%02X %02X",
                       (unsigned)s_scan_dump.id.atqa[0],
                       (unsigned)s_scan_dump.id.atqa[1]);
        menu_nfc_scan_info_add_(line);
    }

    if((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
    {
        (void)snprintf(line, sizeof(line), "SAK :%02X", (unsigned)s_scan_dump.id.sak);
        menu_nfc_scan_info_add_(line);
    }

    if((s_scan_meta != NULL) && s_scan_meta->profile_valid && (s_scan_meta->profile.ats_len > 0U))
    {
        char ats_hex[17];
        menu_nfc_format_hex_compact_(
            s_scan_meta->profile.ats, s_scan_meta->profile.ats_len, ats_hex, sizeof(ats_hex));
        (void)snprintf(line, sizeof(line), "ATS:%s", ats_hex);
        menu_nfc_scan_info_add_(line);
    }
}

/**
 * @brief Rebuilds the action list for the current scan context.
 *
 * @return void
 */
static void menu_nfc_scan_actions_rebuild_(void)
{
    const bool can_emulate = s_scan_dump_valid && menu_nfc_emu_supported_(&s_scan_dump.id);
    const bool has_dump = s_scan_dump_valid && (s_scan_dump.read_mode == POOM_NFC_DUMP_READ_FULL) && s_scan_dump.read_ok;
    const bool has_extra_info =
        (s_scan_meta != NULL) && (s_scan_meta->info_count > MENU_NFC_SCAN_SUMMARY_LINES);

    menu_nfc_scan_actions_reset_();
    if(s_scan_meta == NULL)
    {
        return;
    }

    switch(s_scan_meta->kind)
    {
        case MENU_NFC_SCAN_KIND_T2T:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_CARD_INFO);
            if(has_dump)
            {
                menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_DUMP);
            }
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_ID);
            if(can_emulate)
            {
                menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_EMULATE);
            }
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SCAN_AGAIN);
            break;

        case MENU_NFC_SCAN_KIND_MIFARE_BASIC:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_READ_CARD);
            if(has_extra_info)
            {
                menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_CARD_INFO);
            }
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_ID);
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SCAN_AGAIN);
            break;

        case MENU_NFC_SCAN_KIND_MIFARE_READ:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_DUMP);
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_READ_AGAIN);
            break;

        case MENU_NFC_SCAN_KIND_ISODEP:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_CHECK_PAYMENT);
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_ID);
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SCAN_AGAIN);
            break;

        case MENU_NFC_SCAN_KIND_EMV:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_SUMMARY);
            break;

        case MENU_NFC_SCAN_KIND_GENERIC:
        default:
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_CARD_INFO);
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SAVE_ID);
            if(can_emulate)
            {
                menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_EMULATE);
            }
            menu_nfc_scan_action_add_(MENU_NFC_SCAN_ACT_SCAN_AGAIN);
            break;
    }
}

/**
 * @brief Prepares the Type 2 / NTAG summary and detail view.
 *
 * @return void
 */
static void menu_nfc_prepare_t2t_scan_view_(void)
{
    char uid_compact[22];
    char line[22];
    uint16_t user_bytes = 0U;
    uint16_t total_bytes = 0U;
    const bool has_user_bytes = poom_nfc_dump_get_t2t_user_bytes(&s_scan_dump, &user_bytes);
    const bool has_total_bytes = poom_nfc_dump_get_t2t_total_bytes(&s_scan_dump, &total_bytes);

    if(s_scan_meta == NULL)
    {
        return;
    }

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_T2T;
    menu_nfc_line_copy_(menu_nfc_scan_card_short_(&s_scan_dump), s_scan_meta->summary_lines[0]);
    menu_nfc_format_uid_compact_(&s_scan_dump.id, uid_compact, sizeof(uid_compact));
    (void)snprintf(s_scan_meta->summary_lines[1],
                   sizeof(s_scan_meta->summary_lines[1]),
                   "UID:%.17s",
                   uid_compact);

    if(has_user_bytes && has_total_bytes)
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "N:%s %u/%u",
                       s_scan_ndef_known ? (s_scan_has_ndef ? "Y" : "N") : "U",
                       (unsigned)user_bytes,
                       (unsigned)total_bytes);
    }
    else
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "Read:%s N:%s",
                       menu_nfc_scan_mode_str_(&s_scan_dump),
                       s_scan_ndef_known ? (s_scan_has_ndef ? "Y" : "N") : "U");
    }

    menu_nfc_scan_info_add_identity_(s_scan_meta->summary_lines[0]);
    (void)snprintf(line, sizeof(line), "Mfr:%s", menu_nfc_mfr_abbr_(&s_scan_dump));
    menu_nfc_scan_info_add_(line);
    (void)snprintf(line, sizeof(line), "Read:%s", menu_nfc_scan_mode_str_(&s_scan_dump));
    menu_nfc_scan_info_add_(line);
    (void)snprintf(line, sizeof(line), "NDEF:%s", s_scan_ndef_known ? (s_scan_has_ndef ? "YES" : "NO") : "UNK");
    menu_nfc_scan_info_add_(line);
    if(has_user_bytes && has_total_bytes)
    {
        (void)snprintf(line, sizeof(line), "Mem:%u/%u", (unsigned)user_bytes, (unsigned)total_bytes);
        menu_nfc_scan_info_add_(line);
    }

    menu_nfc_scan_actions_rebuild_();
}

/**
 * @brief Prepares the initial MIFARE summary without reading sectors.
 *
 * @param[in] card_type Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_prepare_mifare_basic_scan_view_(nfc_card_type_t card_type)
{
    char uid_compact[22];
    char line[22];

    if(s_scan_meta == NULL)
    {
        return;
    }

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_MIFARE_BASIC;
    switch(card_type)
    {
        case NFC_CARD_MIFARE_MINI:
            menu_nfc_line_copy_("MIFARE MINI", s_scan_meta->summary_lines[0]);
            break;

        case NFC_CARD_MIFARE_CLASSIC_1K:
            menu_nfc_line_copy_("MIFARE CLASSIC 1K", s_scan_meta->summary_lines[0]);
            break;

        case NFC_CARD_MIFARE_CLASSIC_4K:
            menu_nfc_line_copy_("MIFARE CLASSIC 4K", s_scan_meta->summary_lines[0]);
            break;

        case NFC_CARD_MIFARE_PLUS:
            menu_nfc_line_copy_("MIFARE PLUS", s_scan_meta->summary_lines[0]);
            break;

        default:
            menu_nfc_line_copy_("MIFARE CARD", s_scan_meta->summary_lines[0]);
            break;
    }

    menu_nfc_format_uid_compact_(&s_scan_dump.id, uid_compact, sizeof(uid_compact));
    (void)snprintf(s_scan_meta->summary_lines[1],
                   sizeof(s_scan_meta->summary_lines[1]),
                   "UID:%.17s",
                   uid_compact);
    if(((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U) &&
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "ATQA:%02X%02X S:%02X ID",
                       (unsigned)s_scan_dump.id.atqa[0],
                       (unsigned)s_scan_dump.id.atqa[1],
                       (unsigned)s_scan_dump.id.sak);
    }
    else
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "Mode:%s",
                       menu_nfc_scan_mode_str_(&s_scan_dump));
    }

    for(uint8_t i = 0U; i < MENU_NFC_SCAN_SUMMARY_LINES; i++)
    {
        if(s_scan_meta->summary_lines[i][0] != '\0')
        {
            menu_nfc_scan_info_add_(s_scan_meta->summary_lines[i]);
        }
    }
    if(((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U) ||
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U))
    {
        (void)snprintf(line, sizeof(line), "Type:%s", menu_nfc_scan_card_short_(&s_scan_dump));
        menu_nfc_scan_info_add_(line);
    }
    menu_nfc_scan_actions_rebuild_();
}

/**
 * @brief Prepares the initial ISO-DEP summary without probing EMV.
 *
 * @return void
 */
static void menu_nfc_prepare_isodep_scan_view_(void)
{
    char uid_compact[22];
    char line[22];

    if(s_scan_meta == NULL)
    {
        return;
    }

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_ISODEP;
    menu_nfc_line_copy_("ISO-DEP CARD", s_scan_meta->summary_lines[0]);
    menu_nfc_format_uid_compact_(&s_scan_dump.id, uid_compact, sizeof(uid_compact));
    (void)snprintf(s_scan_meta->summary_lines[1],
                   sizeof(s_scan_meta->summary_lines[1]),
                   "UID:%.17s",
                   uid_compact);
    if((s_scan_meta != NULL) && s_scan_meta->profile_valid && (s_scan_meta->profile.ats_len > 0U) &&
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "ATS:Y SAK:%02X",
                       (unsigned)s_scan_dump.id.sak);
    }
    else if((s_scan_meta != NULL) && s_scan_meta->profile_valid && (s_scan_meta->profile.ats_len > 0U))
    {
        menu_nfc_line_copy_("ATS:YES", s_scan_meta->summary_lines[2]);
    }
    else if((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "SAK:%02X",
                       (unsigned)s_scan_dump.id.sak);
    }
    else
    {
        menu_nfc_line_copy_("Read:ID", s_scan_meta->summary_lines[2]);
    }

    if(((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U) &&
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U))
    {
        (void)snprintf(line,
                       sizeof(line),
                       "ATQA:%02X %02X",
                       (unsigned)s_scan_dump.id.atqa[0],
                       (unsigned)s_scan_dump.id.atqa[1]);
        menu_nfc_scan_info_add_(line);
    }
    menu_nfc_scan_actions_rebuild_();
}

/**
 * @brief Prepares the initial scan view using only basic identification.
 *
 * @param[in] card_type Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_prepare_basic_scan_view_(nfc_card_type_t card_type)
{
    if(poom_mifare_classic_is_supported_card(card_type))
    {
        menu_nfc_prepare_mifare_basic_scan_view_(card_type);
    }
    else if(card_type == NFC_CARD_ULTRALIGHT_OR_NTAG)
    {
        menu_nfc_prepare_t2t_scan_view_();
    }
    else if(menu_nfc_scan_is_isodep_candidate_(&s_scan_dump))
    {
        menu_nfc_prepare_isodep_scan_view_();
    }
    else
    {
        menu_nfc_prepare_generic_scan_view_();
    }
}

/**
 * @brief Internal helper for `menu_nfc_prepare_generic_scan_view`.
 *
 * @return void
 */
static void menu_nfc_prepare_generic_scan_view_(void)
{
    char uid_compact[22];
    char line[22];
    uint16_t user_bytes = 0U;
    uint16_t total_bytes = 0U;
    const bool has_user_bytes = poom_nfc_dump_get_t2t_user_bytes(&s_scan_dump, &user_bytes);
    const bool has_total_bytes = poom_nfc_dump_get_t2t_total_bytes(&s_scan_dump, &total_bytes);

    if(s_scan_meta == NULL)
    {
        return;
    }

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_GENERIC;
    menu_nfc_format_uid_compact_(&s_scan_dump.id, uid_compact, sizeof(uid_compact));
    (void)snprintf(s_scan_meta->summary_lines[0],
                   sizeof(s_scan_meta->summary_lines[0]),
                   "Type:%s M:%s",
                   menu_nfc_scan_card_short_(&s_scan_dump),
                   menu_nfc_mfr_abbr_(&s_scan_dump));
    (void)snprintf(s_scan_meta->summary_lines[1],
                   sizeof(s_scan_meta->summary_lines[1]),
                   "UID:%.17s",
                   uid_compact);
    if(has_user_bytes && has_total_bytes)
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "R:%s N:%s %u/%u",
                       menu_nfc_scan_mode_str_(&s_scan_dump),
                       s_scan_ndef_known ? (s_scan_has_ndef ? "Y" : "N") : "U",
                       (unsigned)user_bytes,
                       (unsigned)total_bytes);
    }
    else
    {
        (void)snprintf(s_scan_meta->summary_lines[2],
                       sizeof(s_scan_meta->summary_lines[2]),
                       "Read:%s NDEF:%s",
                       menu_nfc_scan_mode_str_(&s_scan_dump),
                       s_scan_ndef_known ? (s_scan_has_ndef ? "YES" : "NO") : "UNK");
    }

    menu_nfc_scan_info_add_identity_(s_scan_meta->summary_lines[0]);
    (void)snprintf(line, sizeof(line), "Read:%s", menu_nfc_scan_mode_str_(&s_scan_dump));
    menu_nfc_scan_info_add_(line);
    menu_nfc_scan_info_add_(s_scan_meta->summary_lines[2]);
    menu_nfc_scan_actions_rebuild_();
}

/**
 * @brief Internal helper for `menu_nfc_prepare_mifare_scan_view`.
 *
 * @param[in] card_type Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_prepare_mifare_scan_view_(nfc_card_type_t card_type)
{
    uint8_t sectors;
    uint8_t found_a = 0U;
    uint8_t found_b = 0U;
    uint8_t unlocked_sectors = 0U;
    char uid_hex[POOM_NFC_CARD_UID_MAX * 2U + 1U];
    char line[22];

    menu_nfc_draw_busy_progress_("READING MIFARE", "Linking...", 1U, 2U);
    MENU_NFC_TRACE("mifare start type=%s", nfc_ident_card_type_to_str(card_type));

    if(!poom_nfc_controller_connect())
    {
        MENU_NFC_TRACE("mifare connect failed");
        poom_nfc_controller_stop();
        return false;
    }

    if(!poom_mifare_classic_bind_card(s_scan_dump.id.uid, s_scan_dump.id.uid_len, card_type))
    {
        MENU_NFC_TRACE("mifare bind failed");
        poom_nfc_controller_stop();
        return false;
    }

    menu_nfc_draw_busy_progress_("READING MIFARE", "Unlocking...", 2U, 2U);
    (void)poom_mifare_classic_discover_default_keys(true);
    sectors = poom_mifare_classic_get_sector_count();
    for(uint8_t s = 0U; s < sectors; s++)
    {
        uint8_t key[6];
        bool has_key = false;
        if(poom_mifare_classic_get_sector_key(s, POOM_MIFARE_KEY_A, key))
        {
            found_a++;
            has_key = true;
        }
        if(poom_mifare_classic_get_sector_key(s, POOM_MIFARE_KEY_B, key))
        {
            found_b++;
            has_key = true;
        }
        if(has_key)
        {
            unlocked_sectors++;
        }
    }
    MENU_NFC_TRACE(
        "mifare keys keyA=%u/%u keyB=%u/%u unlocked=%u/%u",
        (unsigned)found_a,
        (unsigned)sectors,
        (unsigned)found_b,
        (unsigned)sectors,
        (unsigned)unlocked_sectors,
        (unsigned)sectors);
    poom_nfc_controller_stop();

    if(s_scan_meta == NULL)
    {
        return false;
    }

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_MIFARE_READ;
    menu_nfc_line_copy_(
        (unlocked_sectors == sectors) ? "MIFARE READY" : "MIFARE PARTIAL", s_scan_meta->summary_lines[0]);
    (void)snprintf(
        s_scan_meta->summary_lines[1],
        sizeof(s_scan_meta->summary_lines[1]),
        "Unlock:%u/%u sec",
        (unsigned)unlocked_sectors,
        (unsigned)sectors);
    (void)snprintf(
        s_scan_meta->summary_lines[2],
        sizeof(s_scan_meta->summary_lines[2]),
        "Keys:%uA %uB",
        (unsigned)found_a,
        (unsigned)found_b);

    menu_nfc_format_uid_hex_(&s_scan_dump.id, uid_hex, sizeof(uid_hex));
    (void)snprintf(line, sizeof(line), "UID :%.16s", uid_hex);
    menu_nfc_scan_info_add_(line);
    (void)snprintf(line, sizeof(line), "Type:%s", menu_nfc_scan_card_short_(&s_scan_dump));
    menu_nfc_scan_info_add_(line);
    if(((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U) &&
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
    {
        (void)snprintf(line,
                       sizeof(line),
                       "ATQA:%02X %02X",
                       (unsigned)s_scan_dump.id.atqa[0],
                       (unsigned)s_scan_dump.id.atqa[1]);
        menu_nfc_scan_info_add_(line);
        (void)snprintf(line, sizeof(line), "SAK :%02X", (unsigned)s_scan_dump.id.sak);
        menu_nfc_scan_info_add_(line);
    }
    menu_nfc_scan_actions_rebuild_();
    return true;
}

/**
 * @brief Internal helper for `menu_nfc_prepare_emv_scan_view`.
 *
 * @return bool
 */
static bool menu_nfc_prepare_emv_scan_view_connected_(
    bool allow_direct_aid_fallback,
    const menu_nfc_emv_candidate_t* selected_app)
{
    size_t app_count = 0U;
    bool ppse_ok = false;
    bool aid_select_ok = false;
    menu_nfc_emv_first_app_t first = {0};
    menu_nfc_emv_candidate_t candidates[MENU_NFC_EMV_APP_MAX] = {0};
    menu_nfc_emv_collect_ctx_t collect_ctx = {
        .apps = candidates,
        .capacity = MENU_NFC_EMV_APP_MAX,
        .count = 0U,
    };
    menu_nfc_emv_select_meta_t meta = {0};
    const char* aid_brand = NULL;
    char aid_text[22];
    char name_text[22];
    char label_text[22];
    char title_text[22];
    char line[22];
    char aid_log[22];

    if(s_scan_meta == NULL)
    {
        return false;
    }

    MENU_NFC_TRACE("emv start direct_aid=%s", allow_direct_aid_fallback ? "yes" : "no");
    if(selected_app != NULL)
    {
        first = *selected_app;
        ppse_ok = true;
    }
    else
    {
        uint8_t rapdu[260];
        size_t rapdu_len = 0U;
        poom_iso7816_rapdu_view_t view;

        menu_nfc_draw_busy_progress_("READING EMV", "Selecting PPSE...", 2U, 4U);
        for(uint8_t pass = 0U; pass < MENU_NFC_EMV_PPSE_TRIES; pass++)
        {
            app_count = 0U;
            collect_ctx.count = 0U;
            (void)memset(candidates, 0, sizeof(candidates));
            rapdu_len = 0U;

            MENU_NFC_TRACE("emv ppse try=%u/%u", (unsigned)(pass + 1U), (unsigned)MENU_NFC_EMV_PPSE_TRIES);
            ppse_ok = poom_nfc_emv_select_ppse(rapdu, sizeof(rapdu), &rapdu_len) &&
                      poom_nfc_emv_parse_ppse_apps(
                          rapdu, rapdu_len, menu_nfc_emv_collect_app_cb_, &collect_ctx, &app_count) &&
                      (app_count > 0U) &&
                      (collect_ctx.count > 0U);
            if(ppse_ok)
            {
                menu_nfc_emv_format_aid_(
                    candidates[0].aid, candidates[0].aid_len, aid_log, sizeof(aid_log));
                MENU_NFC_TRACE(
                    "emv ppse ok apps=%u aid=%s label=%s",
                    (unsigned)app_count,
                    aid_log,
                    (candidates[0].label[0] != '\0') ? candidates[0].label : "-");
                break;
            }
            if(poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view))
            {
                MENU_NFC_TRACE(
                    "emv ppse miss sw=%02X%02X apps=%u", (unsigned)view.sw1, (unsigned)view.sw2, (unsigned)app_count);
            }
            else
            {
                MENU_NFC_TRACE("emv ppse miss parse/link");
            }
            if((pass + 1U) < MENU_NFC_EMV_PPSE_TRIES)
            {
                vTaskDelay(pdMS_TO_TICKS(MENU_NFC_EMV_RETRY_DELAY_MS));
            }
        }

        if(ppse_ok && collect_ctx.count > 1U)
        {
            free(s_scan_meta->emv_apps);
            s_scan_meta->emv_apps = (menu_nfc_emv_candidate_t*)calloc(
                collect_ctx.count, sizeof(*s_scan_meta->emv_apps));
            if(s_scan_meta->emv_apps == NULL)
            {
                return false;
            }
            (void)memcpy(s_scan_meta->emv_apps,
                         candidates,
                         collect_ctx.count * sizeof(*s_scan_meta->emv_apps));
            s_scan_meta->emv_app_count = (uint8_t)collect_ctx.count;
            s_scan_meta->emv_app_sel = 0U;
            s_scan_meta->emv_app_scroll = 0U;
            s_state = MENU_NFC_STATE_EMV_APP_SELECT;
            menu_nfc_request_redraw_();
            return true;
        }
        if(ppse_ok)
        {
            first = candidates[0];
        }
    }

    if(ppse_ok)
    {
        uint8_t rapdu[260];
        size_t rapdu_len = 0U;
        poom_iso7816_rapdu_view_t view;

        menu_nfc_draw_busy_progress_("READING EMV", "Selecting app...", 3U, 4U);
        aid_select_ok = poom_nfc_emv_select_aid(first.aid, first.aid_len, rapdu, sizeof(rapdu), &rapdu_len);
        if(aid_select_ok &&
           poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view) &&
           poom_iso7816_status_is_ok(view.sw1, view.sw2))
        {
            menu_nfc_emv_collect_meta_(view.data, view.data_len, &meta);
            menu_nfc_emv_format_aid_(first.aid, first.aid_len, aid_log, sizeof(aid_log));
            MENU_NFC_TRACE("emv select ok aid=%s", aid_log);
        }
        else
        {
            if(poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view))
            {
                MENU_NFC_TRACE("emv select fail sw=%02X%02X", (unsigned)view.sw1, (unsigned)view.sw2);
            }
            else
            {
                MENU_NFC_TRACE("emv select fail parse/link");
            }
        }
    }
    else if(allow_direct_aid_fallback)
    {
        menu_nfc_draw_busy_progress_("READING EMV", "Trying app AIDs...", 3U, 4U);
        if(!menu_nfc_emv_try_direct_aids_(&first, &meta, &aid_select_ok))
        {
            MENU_NFC_TRACE("emv direct aid fallback miss");
            return false;
        }
        menu_nfc_emv_format_aid_(first.aid, first.aid_len, aid_log, sizeof(aid_log));
        MENU_NFC_TRACE("emv direct aid ok aid=%s", aid_log);
    }
    else
    {
        MENU_NFC_TRACE("emv no ppse and no direct aid fallback");
        return false;
    }

    s_scan_meta->emv_data_valid = false;
    if(aid_select_ok)
    {
        menu_nfc_draw_busy_progress_("READING EMV", "Reading records...", 4U, 4U);
        s_scan_meta->emv_data =
            (poom_nfc_emv_card_t*)calloc(1U, sizeof(*s_scan_meta->emv_data));
        if(s_scan_meta->emv_data != NULL)
        {
            s_scan_meta->emv_data_valid = poom_nfc_emv_read_application(
                first.aid, first.aid_len, s_scan_meta->emv_data);
            if(!s_scan_meta->emv_data_valid)
            {
                poom_nfc_emv_card_release(s_scan_meta->emv_data);
                free(s_scan_meta->emv_data);
                s_scan_meta->emv_data = NULL;
            }
        }
        MENU_NFC_TRACE(
            "emv records=%s pan=%u tx=%u",
            (s_scan_meta->emv_data_valid && s_scan_meta->emv_data->read_completed) ?
                "ok" : "partial",
            (unsigned)((s_scan_meta->emv_data != NULL) ? s_scan_meta->emv_data->pan_len : 0U),
            (unsigned)((s_scan_meta->emv_data != NULL) ?
                           s_scan_meta->emv_data->transaction_count :
                           0U));
    }

    name_text[0] = '\0';
    label_text[0] = '\0';
    title_text[0] = '\0';

    s_scan_meta->kind = MENU_NFC_SCAN_KIND_EMV;
    aid_brand = menu_nfc_emv_brand_from_aid_(first.aid, first.aid_len);
    if(s_scan_meta->emv_data_valid && s_scan_meta->emv_data->application_name[0] != '\0')
    {
        menu_nfc_line_copy_(s_scan_meta->emv_data->application_name, name_text);
    }
    else if(meta.name_len > 0U)
    {
        menu_nfc_emv_copy_ascii_(meta.name, meta.name_len, name_text, sizeof(name_text));
    }
    if(s_scan_meta->emv_data_valid && s_scan_meta->emv_data->application_label[0] != '\0')
    {
        menu_nfc_line_copy_(s_scan_meta->emv_data->application_label, label_text);
    }
    else if(meta.label_len > 0U)
    {
        menu_nfc_emv_copy_ascii_(meta.label, meta.label_len, label_text, sizeof(label_text));
    }
    else if(first.label[0] != '\0')
    {
        menu_nfc_line_copy_(first.label, label_text);
    }

    if(aid_brand != NULL)
    {
        (void)snprintf(title_text, sizeof(title_text), "EMV %s", aid_brand);
        menu_nfc_line_copy_(title_text, s_scan_meta->summary_lines[0]);
    }
    else
    {
        menu_nfc_line_copy_("EMV APP", s_scan_meta->summary_lines[0]);
    }

    if(name_text[0] != '\0')
    {
        menu_nfc_line_copy_(name_text, s_scan_meta->summary_lines[1]);
    }
    else if(label_text[0] != '\0')
    {
        menu_nfc_line_copy_(label_text, s_scan_meta->summary_lines[1]);
    }
    else if(aid_brand != NULL)
    {
        menu_nfc_line_copy_(aid_brand, s_scan_meta->summary_lines[1]);
    }
    else
    {
        menu_nfc_line_copy_("EMV Payment Card", s_scan_meta->summary_lines[1]);
    }
    MENU_NFC_TRACE(
        "emv result title=%s line1=%s brand=%s",
        s_scan_meta->summary_lines[0],
        s_scan_meta->summary_lines[1],
        (aid_brand != NULL) ? aid_brand : "-");
    menu_nfc_emv_format_aid_(first.aid, first.aid_len, aid_text, sizeof(aid_text));
    if((label_text[0] != '\0') && (strcmp(label_text, s_scan_meta->summary_lines[1]) != 0))
    {
        menu_nfc_line_copy_(label_text, s_scan_meta->summary_lines[2]);
    }
    else
    {
        menu_nfc_line_copy_(aid_text, s_scan_meta->summary_lines[2]);
    }

    if((name_text[0] != '\0') &&
       (strcmp(name_text, s_scan_meta->summary_lines[1]) != 0) &&
       (strcmp(name_text, s_scan_meta->summary_lines[2]) != 0))
    {
        (void)snprintf(line, sizeof(line), "Name:%.16s", name_text);
        menu_nfc_scan_info_add_(line);
    }
    if((label_text[0] != '\0') &&
       (strcmp(label_text, s_scan_meta->summary_lines[1]) != 0) &&
       (strcmp(label_text, s_scan_meta->summary_lines[2]) != 0))
    {
        (void)snprintf(line, sizeof(line), "Label:%.15s", label_text);
        menu_nfc_scan_info_add_(line);
    }
    if((aid_brand != NULL) &&
       (strcmp(aid_brand, s_scan_meta->summary_lines[1]) != 0) &&
       (strstr(s_scan_meta->summary_lines[0], aid_brand) == NULL))
    {
        (void)snprintf(line, sizeof(line), "Brand:%s", aid_brand);
        menu_nfc_scan_info_add_(line);
    }
    if(strcmp(aid_text, s_scan_meta->summary_lines[2]) != 0)
    {
        (void)snprintf(line, sizeof(line), "AID :%.16s", aid_text);
        menu_nfc_scan_info_add_(line);
    }
    if(meta.has_priority || first.has_priority)
    {
        const uint8_t priority = meta.has_priority ? meta.priority : first.priority;
        (void)snprintf(line, sizeof(line), "Prio:%u", (unsigned)priority);
        menu_nfc_scan_info_add_(line);
    }
    if(meta.langs_len > 0U)
    {
        char langs[16];
        menu_nfc_emv_format_langs_(meta.langs, meta.langs_len, langs, sizeof(langs));
        (void)snprintf(line, sizeof(line), "Lang:%s", langs);
        menu_nfc_scan_info_add_(line);
    }
    if(s_scan_meta->emv_data_valid)
    {
        const poom_nfc_emv_card_t* card = s_scan_meta->emv_data;
        (void)snprintf(line, sizeof(line), "Scheme:%s", poom_nfc_emv_scheme_str(card->scheme));
        menu_nfc_scan_info_add_(line);
        if(card->scheme == POOM_NFC_EMV_SCHEME_VISA)
        {
            menu_nfc_scan_info_add_("Kernel:K3 expected");
        }
        else if(card->scheme == POOM_NFC_EMV_SCHEME_MASTERCARD)
        {
            menu_nfc_scan_info_add_("Kernel:K2 expected");
        }
        else
        {
            menu_nfc_scan_info_add_("Kernel:not inferred");
        }
        if(card->has_aip)
        {
            (void)snprintf(line, sizeof(line), "AIP:%02X %02X", card->aip[0], card->aip[1]);
            menu_nfc_scan_info_add_(line);
            (void)snprintf(line, sizeof(line), " AIP:");
            poom_nfc_emv_format_aip(card, &line[5], sizeof(line) - 5U);
            if(line[5] != '\0')
            {
                menu_nfc_scan_info_add_(line);
            }
        }
        if(card->gpo_succeeded)
        {
            menu_nfc_scan_info_add_(card->afl_present ? "Data:AFL records" :
                                                        "Data:GPO inline");
        }
        if(card->pan_len >= 4U)
        {
            menu_nfc_emv_format_masked_pan_(card, line, sizeof(line));
            menu_nfc_scan_info_add_(line);
        }
        if(card->cardholder_name[0] != '\0')
        {
            (void)snprintf(line, sizeof(line), "Holder:%.14s", card->cardholder_name);
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_expiration_date)
        {
            (void)snprintf(line,
                           sizeof(line),
                           "Exp:%02X/%02X",
                           card->expiration_date[0],
                           card->expiration_date[1]);
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_pan_sequence_number)
        {
            (void)snprintf(line, sizeof(line), "PAN seq:%u", (unsigned)card->pan_sequence_number);
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_service_code)
        {
            (void)snprintf(line, sizeof(line), "Service:%03u", (unsigned)card->service_code);
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_cryptogram_information_data)
        {
            (void)snprintf(line,
                           sizeof(line),
                           "CID:%02X %s",
                           card->cryptogram_information_data,
                           poom_nfc_emv_cryptogram_type_str(card->cryptogram_information_data));
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_offline_spending_amount)
        {
            (void)snprintf(line,
                           sizeof(line),
                           "Visa AOSA:%llu",
                           (unsigned long long)card->offline_spending_amount);
            menu_nfc_scan_info_add_(line);
        }
        if(card->issuer_application_data_len > 0U)
        {
            char hex[17];
            const size_t shown = (card->issuer_application_data_len < 8U) ?
                                     card->issuer_application_data_len : 8U;
            menu_nfc_format_hex_compact_(card->issuer_application_data, shown, hex, sizeof(hex));
            (void)snprintf(line, sizeof(line), "IAD:%s", hex);
            menu_nfc_scan_info_add_(line);
        }
        if(card->application_cryptogram_len > 0U)
        {
            char hex[17];
            menu_nfc_format_hex_compact_(card->application_cryptogram,
                                         card->application_cryptogram_len,
                                         hex,
                                         sizeof(hex));
            (void)snprintf(line, sizeof(line), "AC:%s", hex);
            menu_nfc_scan_info_add_(line);
        }
        if(card->card_transaction_qualifiers_len > 0U)
        {
            char hex[9];
            menu_nfc_format_hex_compact_(card->card_transaction_qualifiers,
                                         card->card_transaction_qualifiers_len,
                                         hex,
                                         sizeof(hex));
            (void)snprintf(line, sizeof(line), "CTQ:%s", hex);
            menu_nfc_scan_info_add_(line);
        }
        if(card->form_factor_indicator_len > 0U)
        {
            char hex[17];
            const size_t shown = (card->form_factor_indicator_len < 8U) ?
                                     card->form_factor_indicator_len : 8U;
            menu_nfc_format_hex_compact_(card->form_factor_indicator, shown, hex, sizeof(hex));
            (void)snprintf(line, sizeof(line), "FFI:%s", hex);
            menu_nfc_scan_info_add_(line);
        }
        if(card->details != NULL)
        {
            const poom_nfc_emv_details_t* details = card->details;
            if(meta.langs_len == 0U && details->language_preferences_len > 0U)
            {
                char langs[12];
                menu_nfc_emv_copy_ascii_(details->language_preferences,
                                         details->language_preferences_len,
                                         langs,
                                         sizeof(langs));
                (void)snprintf(line, sizeof(line), "Lang:%s", langs);
                menu_nfc_scan_info_add_(line);
            }
            if(details->has_application_version_number)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "App version:%04X",
                               details->application_version_number);
                menu_nfc_scan_info_add_(line);
            }
            if(details->has_application_usage_control)
            {
                (void)snprintf(line, sizeof(line), "AUC:");
                poom_nfc_emv_format_auc(card, &line[4], sizeof(line) - 4U);
                if(line[4] != '\0')
                {
                    menu_nfc_scan_info_add_(line);
                }
            }
            if(details->cvm_list_len > 0U)
            {
                (void)snprintf(line, sizeof(line), "CVM:");
                poom_nfc_emv_format_cvm(card, &line[4], sizeof(line) - 4U);
                if(line[4] != '\0')
                {
                    menu_nfc_scan_info_add_(line);
                }
            }
            if(details->cdol1_len > 0U)
            {
                (void)snprintf(line, sizeof(line), "CDOL1:");
                poom_nfc_emv_format_dol(
                    details->cdol1, details->cdol1_len, &line[6], sizeof(line) - 6U);
                menu_nfc_scan_info_add_(line);
            }
            if(details->cdol2_len > 0U)
            {
                (void)snprintf(line, sizeof(line), "CDOL2:");
                poom_nfc_emv_format_dol(
                    details->cdol2, details->cdol2_len, &line[6], sizeof(line) - 6U);
                menu_nfc_scan_info_add_(line);
            }
            if(details->has_issuer_action_code_default ||
               details->has_issuer_action_code_denial ||
               details->has_issuer_action_code_online)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "IAC:%s%s%s",
                               details->has_issuer_action_code_default ? "D" : "-",
                               details->has_issuer_action_code_denial ? "N" : "-",
                               details->has_issuer_action_code_online ? "O" : "-");
                menu_nfc_scan_info_add_(line);
            }
            if(details->has_ca_public_key_index)
            {
                (void)snprintf(line, sizeof(line), "CA key:%02X", details->ca_public_key_index);
                menu_nfc_scan_info_add_(line);
            }
            if(details->issuer_public_key_certificate_len > 0U ||
               details->icc_public_key_certificate_len > 0U)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "Cert:I%u C%u",
                               (unsigned)details->issuer_public_key_certificate_len,
                               (unsigned)details->icc_public_key_certificate_len);
                menu_nfc_scan_info_add_(line);
            }
            if(details->mastercard_application_capabilities_len > 0U)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "MC ACI:%uB",
                               (unsigned)details->mastercard_application_capabilities_len);
                menu_nfc_scan_info_add_(line);
            }
            if(details->mastercard_9f6c_len > 0U)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "MC 9F6C:%uB",
                               (unsigned)details->mastercard_9f6c_len);
                menu_nfc_scan_info_add_(line);
            }
            if(details->mastercard_third_party_data_len > 0U)
            {
                (void)snprintf(line,
                               sizeof(line),
                               "MC TPD:%uB",
                               (unsigned)details->mastercard_third_party_data_len);
                menu_nfc_scan_info_add_(line);
            }
        }
        if(card->has_application_transaction_counter)
        {
            (void)snprintf(
                line, sizeof(line), "ATC:%u", (unsigned)card->application_transaction_counter);
            menu_nfc_scan_info_add_(line);
        }
        if(card->has_pin_try_counter)
        {
            (void)snprintf(line, sizeof(line), "PIN tries:%u", (unsigned)card->pin_try_counter);
            menu_nfc_scan_info_add_(line);
        }
        if(card->transaction_count > 0U)
        {
            (void)snprintf(line, sizeof(line), "Tx log:%u", (unsigned)card->transaction_count);
            menu_nfc_scan_info_add_(line);
        }
        if(card->capture != NULL)
        {
            (void)snprintf(line,
                           sizeof(line),
                           "APDU:%u%s",
                           (unsigned)card->capture->exchange_count,
                           card->capture->truncated ? "+" : "");
            menu_nfc_scan_info_add_(line);
        }
        if(card->optional_data_unsupported)
        {
            menu_nfc_scan_info_add_("Optional:unsupported");
        }
    }
    menu_nfc_scan_info_add_(ppse_ok ? "PPSE:OK" : "PPSE:not supported");
    if(s_scan_meta->emv_data_valid)
    {
        (void)snprintf(line,
                       sizeof(line),
                       "EMV:%s",
                       poom_nfc_emv_read_status_str(s_scan_meta->emv_data->read_status));
        menu_nfc_scan_info_add_(line);
    }
    else
    {
        menu_nfc_scan_info_add_(aid_select_ok ? "EMV:partial" : "APP :PPSE only");
    }
    menu_nfc_scan_info_add_("Type:ISO-DEP");
    menu_nfc_scan_actions_rebuild_();
    return true;
}

/**
 * @brief Internal helper for `menu_nfc_prepare_emv_scan_view`.
 *
 * @return bool
 */
static bool menu_nfc_prepare_emv_scan_view_(void)
{
    bool ok;
    nfc_card_type_t card_type;

    menu_nfc_draw_busy_progress_("READING CARD", "Linking...", 1U, 3U);
    if(!poom_nfc_controller_connect())
    {
        MENU_NFC_TRACE("emv wrapper connect failed");
        poom_nfc_controller_stop();
        return false;
    }

    card_type = menu_nfc_scan_type_from_dump_(&s_scan_dump);
    MENU_NFC_TRACE("emv wrapper card_type=%s", nfc_ident_card_type_to_str(card_type));
    ok = menu_nfc_prepare_emv_scan_view_connected_(
        menu_nfc_allow_direct_emv_aid_fallback_(card_type), NULL);
    poom_nfc_controller_stop();
    MENU_NFC_TRACE("emv wrapper result=%s", ok ? "ok" : "fail");
    return ok;
}

static bool menu_nfc_prepare_selected_emv_app_(void)
{
    menu_nfc_emv_candidate_t selected;
    bool ok;

    if(s_scan_meta == NULL || s_scan_meta->emv_apps == NULL ||
       s_scan_meta->emv_app_sel >= s_scan_meta->emv_app_count)
    {
        return false;
    }
    selected = s_scan_meta->emv_apps[s_scan_meta->emv_app_sel];

    menu_nfc_draw_busy_progress_("READING CARD", "Linking...", 1U, 4U);
    if(!poom_nfc_controller_connect())
    {
        poom_nfc_controller_stop();
        return false;
    }

    ok = menu_nfc_prepare_emv_scan_view_connected_(false, &selected);
    poom_nfc_controller_stop();
    if(ok)
    {
        free(s_scan_meta->emv_apps);
        s_scan_meta->emv_apps = NULL;
        s_scan_meta->emv_app_count = 0U;
        s_scan_meta->emv_app_sel = 0U;
        s_scan_meta->emv_app_scroll = 0U;
    }
    return ok;
}

/**
 * @brief Opens the current scan info view from the action menu.
 *
 * @return void
 */
static void menu_nfc_scan_open_info_(void)
{
    if(s_scan_meta == NULL)
    {
        return;
    }

    s_scan_meta->info_scroll = 0U;
    s_state = MENU_NFC_STATE_SCAN_INFO;
    menu_nfc_request_redraw_();
}

/**
 * @brief Returns the primary scan view for the current scan kind.
 *
 * @return menu_nfc_state_t
 */
static menu_nfc_state_t menu_nfc_scan_primary_state_(void)
{
    if(s_scan_meta != NULL)
    {
        if((s_scan_meta->kind == MENU_NFC_SCAN_KIND_MIFARE_READ) ||
           (s_scan_meta->kind == MENU_NFC_SCAN_KIND_ISODEP) ||
           (s_scan_meta->kind == MENU_NFC_SCAN_KIND_EMV))
        {
            return MENU_NFC_STATE_SCAN_INFO;
        }
    }

    return MENU_NFC_STATE_SCAN_RESULT;
}

/**
 * @brief Returns true when the detail view should include summary + info lines.
 *
 * @return bool
 */
static bool menu_nfc_scan_use_combined_info_(void)
{
    return menu_nfc_scan_primary_state_() == MENU_NFC_STATE_SCAN_INFO;
}

/**
 * @brief Counts non-empty summary lines for the current scan.
 *
 * @return uint8_t
 */
static uint8_t menu_nfc_scan_summary_count_(void)
{
    uint8_t count = 0U;

    if(s_scan_meta == NULL)
    {
        return 0U;
    }

    for(uint8_t i = 0U; i < MENU_NFC_SCAN_SUMMARY_LINES; i++)
    {
        if(s_scan_meta->summary_lines[i][0] != '\0')
        {
            count++;
        }
    }

    return count;
}

/**
 * @brief Returns how many lines are visible in the current info view.
 *
 * @return uint8_t
 */
static uint8_t menu_nfc_scan_total_info_lines_(void)
{
    uint8_t total = 0U;

    if(s_scan_meta == NULL)
    {
        return 0U;
    }

    total = s_scan_meta->info_count;
    if(menu_nfc_scan_use_combined_info_())
    {
        total = (uint8_t)(total + menu_nfc_scan_summary_count_());
    }

    return total;
}

/**
 * @brief Returns one line from the combined scan info view.
 *
 * @param[in] idx Parameter passed to the helper.
 * @return const char*
 */
static const char* menu_nfc_scan_info_line_at_(uint8_t idx)
{
    uint8_t summary_count = 0U;

    if(s_scan_meta == NULL)
    {
        return "";
    }

    if(menu_nfc_scan_use_combined_info_())
    {
        for(uint8_t i = 0U; i < MENU_NFC_SCAN_SUMMARY_LINES; i++)
        {
            if(s_scan_meta->summary_lines[i][0] == '\0')
            {
                continue;
            }
            if(summary_count == idx)
            {
                return s_scan_meta->summary_lines[i];
            }
            summary_count++;
        }
        idx = (uint8_t)(idx - summary_count);
    }

    if(idx < s_scan_meta->info_count)
    {
        return s_scan_meta->info_lines[idx];
    }

    return "";
}

/**
 * @brief Executes one dynamic action for the current scan context.
 *
 * @param[in] action Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_scan_run_action_(menu_nfc_scan_action_t action)
{
    const nfc_card_type_t card_type = menu_nfc_scan_type_from_dump_(&s_scan_dump);

    switch(action)
    {
        case MENU_NFC_SCAN_ACT_CARD_INFO:
        case MENU_NFC_SCAN_ACT_DETAILS:
            menu_nfc_scan_open_info_();
            break;

        case MENU_NFC_SCAN_ACT_READ_CARD:
        case MENU_NFC_SCAN_ACT_READ_AGAIN:
            if(!poom_mifare_classic_is_supported_card(card_type) ||
               !menu_nfc_prepare_mifare_scan_view_(card_type))
            {
                menu_nfc_set_info_return_("Read failed", "Hold card near", MENU_NFC_STATE_SCAN_ACTIONS);
                return;
            }
            s_scan_meta->info_scroll = 0U;
            s_state = menu_nfc_scan_primary_state_();
            menu_nfc_request_redraw_();
            break;

        case MENU_NFC_SCAN_ACT_CHECK_PAYMENT:
            if(!menu_nfc_prepare_emv_scan_view_())
            {
                menu_nfc_set_info_return_("Check failed", "Hold card near", MENU_NFC_STATE_SCAN_ACTIONS);
                return;
            }
            if(s_state != MENU_NFC_STATE_EMV_APP_SELECT)
            {
                s_state = menu_nfc_scan_primary_state_();
            }
            menu_nfc_request_redraw_();
            break;

        case MENU_NFC_SCAN_ACT_SAVE_DUMP:
        case MENU_NFC_SCAN_ACT_SAVE_SUMMARY:
            menu_nfc_scan_save_to_sd_();
            break;

        case MENU_NFC_SCAN_ACT_SAVE_ID:
            menu_nfc_scan_save_embedded_();
            break;

        case MENU_NFC_SCAN_ACT_EMULATE:
            menu_nfc_emu_start_id_(&s_scan_dump.id, false, MENU_NFC_STATE_SCAN_ACTIONS);
            break;

        case MENU_NFC_SCAN_ACT_ADVANCED:
            menu_nfc_set_info_return_("Advanced", "Pending", MENU_NFC_STATE_SCAN_ACTIONS);
            break;

        case MENU_NFC_SCAN_ACT_SCAN_AGAIN:
            s_menu_nfc_scan_requested = true;
            s_state = MENU_NFC_STATE_SCAN_SCANNING;
            menu_nfc_request_redraw_();
            break;

        default:
            break;
    }
}

/**
 * @brief Internal helper for menu_nfc_scan_card_short.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_nfc_scan_card_short_(const poom_nfc_dump_t *dump)
{
    if((dump == NULL) || !poom_nfc_card_id_is_valid(&dump->id))
    {
        return "UNKNOWN";
    }

    if((dump->id.type == 0U) || (dump->id.type == 10U))
    {
        if(((dump->id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U) &&
           ((dump->id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
        {
            const uint16_t atqa = ((uint16_t)dump->id.atqa[1] << 8) | (uint16_t)dump->id.atqa[0];
            const nfc_card_type_t detected = nfc_ident_detect_nfca(atqa, dump->id.sak);

            switch(detected)
            {
                case NFC_CARD_ULTRALIGHT_OR_NTAG:
                    if(dump->read_mode == POOM_NFC_DUMP_READ_FULL)
                    {
                        return poom_nfc_t2t_product_to_str(poom_nfc_dump_guess_t2t_product(dump));
                    }
                    return "T2T";
                case NFC_CARD_MIFARE_MINI:        return "MINI";
                case NFC_CARD_MIFARE_CLASSIC_1K:  return "C1K";
                case NFC_CARD_MIFARE_CLASSIC_4K:  return "C4K";
                case NFC_CARD_MIFARE_PLUS:        return "PLUS";
                case NFC_CARD_MIFARE_DESFIRE:     return "DESFIRE";
                case NFC_CARD_NTAG424DNA:         return "NTAG424";
                case NFC_CARD_JCOP:               return "JCOP";
                case NFC_CARD_JEWEL:              return "JEWEL";
                case NFC_CARD_OTHER:              return "OTHER";
                default:                          return "NFCA";
            }
        }
        return "NFCA";
    }

    return "NFC";
}

/**
 * @brief Internal helper for menu_nfc_mfr_abbr.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_nfc_mfr_abbr_(const poom_nfc_dump_t *dump)
{
    if((dump == NULL) || !poom_nfc_card_id_is_valid(&dump->id) || (dump->id.uid_len == 0U))
    {
        return "UNK";
    }

    if(!((dump->id.type == 0U) || (dump->id.type == 10U)))
    {
        return "UNK";
    }

    const char *m = nfc_ident_nfca_manufacturer(dump->id.uid[0]);
    if(m == NULL)
    {
        return "UNK";
    }

    if(strstr(m, "NXP") != NULL) return "NXP";
    if(strstr(m, "STMicro") != NULL) return "ST";
    if(strstr(m, "Infineon") != NULL) return "INF";
    if(strstr(m, "Texas") != NULL) return "TI";
    if(strstr(m, "Sony") != NULL) return "SNY";
    if(strstr(m, "Samsung") != NULL) return "SAM";
    if(strstr(m, "Unknown") != NULL) return "UNK";

    return "UNK";
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] id Parameter passed to the helper.
 * @param[in] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_format_uid_compact_(const poom_nfc_card_id_t *id, char *out, size_t out_len)
{
    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';

    if((id == NULL) || !poom_nfc_card_id_is_valid(id) || (id->uid_len == 0U))
    {
        return;
    }

    size_t w = 0U;
    const uint8_t show = (id->uid_len <= 4U) ? id->uid_len : 4U;
    for(uint8_t i = 0U; i < show; i++)
    {
        const char *sep = (i == 0U) ? "" : " ";
        w += (size_t)snprintf(&out[w], (w < out_len) ? (out_len - w) : 0U, "%s%02X", sep, (unsigned)id->uid[i]);
        if(w >= out_len)
        {
            out[out_len - 1U] = '\0';
            return;
        }
    }

    if(id->uid_len > show)
    {
        (void)snprintf(&out[w], (w < out_len) ? (out_len - w) : 0U, "...");
    }
}

/**
 * @brief Internal helper for `menu_nfc_dump_has_ndef`.
 *
 * @param[in] dump Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_dump_has_ndef_(const poom_nfc_dump_t *dump)
{
    if((dump == NULL) || (dump->read_mode != POOM_NFC_DUMP_READ_FULL))
    {
        return false;
    }

    const uint16_t start_page = (dump->user_mem_start_page > 0U) ? dump->user_mem_start_page : 4U;
    if(dump->pages_read <= start_page)
    {
        return false;
    }

    for(uint16_t page = start_page; page < dump->pages_read; page++)
    {
        for(uint8_t i = 0U; i < POOM_NFC_DUMP_PAGE_SIZE; i++)
        {
            const uint8_t b = dump->pages[page][i];
            if(b == 0x00U)
            {
                continue;
            }
            return (b == 0x03U);
        }
    }

    return false;
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_scan_result_(void)
{
    menu_nfc_draw_frame_("NFC");

    if((s_scan_meta == NULL) ||
       ((s_scan_meta->summary_lines[0][0] == '\0') &&
        (!s_scan_dump_valid || !poom_nfc_card_id_is_valid(&s_scan_dump.id))))
    {
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No tag / invalid"));

        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }
    for(uint8_t i = 0U; i < MENU_NFC_SCAN_SUMMARY_LINES; i++)
    {
        poom_arduboy_set_cursor(4, (int16_t)(MENU_NFC_INFO_Y0 + (int16_t)i * MENU_NFC_INFO_STEP));
        (void)poom_arduboy_print(s_scan_meta->summary_lines[i]);
    }

    menu_nfc_draw_scan_footer_(false, s_scan_meta->info_count > MENU_NFC_SCAN_SUMMARY_LINES);
    poom_arduboy_display();
}

/**
 * @brief Draws the scrollable scan info detail view.
 *
 * @return void
 */
static void menu_nfc_draw_scan_info_(void)
{
    uint8_t scroll = 0U;
    uint8_t total = 0U;

    menu_nfc_draw_frame_("NFC");

    if(s_scan_meta == NULL)
    {
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No scan detail"));
        menu_nfc_draw_scan_footer_(false, false);
        poom_arduboy_display();
        return;
    }

    scroll = s_scan_meta->info_scroll;
    total = menu_nfc_scan_total_info_lines_();

    if(total <= MENU_NFC_SCAN_INFO_VISIBLE_LINES)
    {
        scroll = 0U;
    }
    else if((uint8_t)(scroll + MENU_NFC_SCAN_INFO_VISIBLE_LINES) > total)
    {
        scroll = (uint8_t)(total - MENU_NFC_SCAN_INFO_VISIBLE_LINES);
    }
    s_scan_meta->info_scroll = scroll;

    for(uint8_t row = 0U; row < MENU_NFC_SCAN_INFO_VISIBLE_LINES; row++)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        if(idx >= total)
        {
            break;
        }

        poom_arduboy_set_cursor(4, (int16_t)(MENU_NFC_INFO_Y0 + (int16_t)row * MENU_NFC_INFO_STEP));
        (void)poom_arduboy_print(menu_nfc_scan_info_line_at_(idx));
    }

    menu_nfc_draw_scan_footer_(
        scroll > 0U, (uint8_t)(scroll + MENU_NFC_SCAN_INFO_VISIBLE_LINES) < total);
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_scan_actions_(void)
{
    uint8_t scroll = 0U;

    menu_nfc_draw_frame_("NFC");

    if((s_scan_meta == NULL) || (s_scan_meta->action_count == 0U))
    {
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No actions"));
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    menu_nfc_scan_actions_adjust_scroll_();
    scroll = s_scan_meta->action_scroll;

    for(uint8_t row = 0U; row < VISIBLE_ROWS; row++)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        const int16_t y = (int16_t)(LIST_Y0 + (int16_t)row * ROW_STEP);
        if(idx >= s_scan_meta->action_count)
        {
            break;
        }

        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(menu_nfc_scan_action_label_(s_scan_meta->actions[idx]));

        if(idx == s_scan_meta->action_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_nfc_draw_emv_app_select_(void)
{
    uint8_t scroll;

    menu_nfc_draw_frame_("EMV APPS");
    if(s_scan_meta == NULL || s_scan_meta->emv_apps == NULL || s_scan_meta->emv_app_count < 2U)
    {
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No app choices"));
        poom_arduboy_display();
        return;
    }

    scroll = s_scan_meta->emv_app_scroll;
    if(s_scan_meta->emv_app_sel < scroll)
    {
        scroll = s_scan_meta->emv_app_sel;
    }
    else if(s_scan_meta->emv_app_sel >= (uint8_t)(scroll + VISIBLE_ROWS))
    {
        scroll = (uint8_t)(s_scan_meta->emv_app_sel - VISIBLE_ROWS + 1U);
    }
    s_scan_meta->emv_app_scroll = scroll;

    for(uint8_t row = 0U; row < VISIBLE_ROWS; row++)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        const int16_t y = (int16_t)(LIST_Y0 + (int16_t)row * ROW_STEP);
        char line[22];
        const char* name;
        const char* brand;
        menu_nfc_emv_candidate_t* candidate;

        if(idx >= s_scan_meta->emv_app_count)
        {
            break;
        }
        candidate = &s_scan_meta->emv_apps[idx];
        brand = menu_nfc_emv_brand_from_aid_(candidate->aid, candidate->aid_len);
        name = (candidate->label[0] != '\0') ? candidate->label :
               ((brand != NULL) ? brand : "EMV APP");
        if(candidate->aid_len >= 2U)
        {
            (void)snprintf(line,
                           sizeof(line),
                           "%.15s %02X%02X",
                           name,
                           candidate->aid[candidate->aid_len - 2U],
                           candidate->aid[candidate->aid_len - 1U]);
        }
        else
        {
            (void)snprintf(line, sizeof(line), "%.21s", name);
        }

        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(line);
        if(idx == s_scan_meta->emv_app_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SELECT"));
    poom_arduboy_set_cursor(82, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_emulate_source_(void)
{
    static const char *const labels[MENU_NFC_EMU_SRC_COUNT] = {
        "Embedded",
        "SD card .nfc",
        "T4T default",
    };

    menu_nfc_draw_frame_("NFC");

    for(int row = 0; row < (int)MENU_NFC_EMU_SRC_COUNT; row++)
    {
        const int16_t y = (int16_t)(MAIN_LIST_Y0 + (int16_t)row * MAIN_ROW_STEP);
        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(labels[row]);

        if(row == (int)s_emu_source_sel)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MAIN_ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Compares internal items for sorting.
 *
 * @param[in] a Parameter passed to the helper.
 * @param[in] b Parameter passed to the helper.
 * @return int
 */
static int menu_nfc_sd_dump_cmp_(const void *a, const void *b)
{
    const menu_nfc_sd_item_t *sa = (const menu_nfc_sd_item_t *)a;
    const menu_nfc_sd_item_t *sb = (const menu_nfc_sd_item_t *)b;
    return strcmp(sa->name, sb->name);
}

/**
 * @brief Internal helper for `menu_nfc_path_has_ext`.
 *
 * @param[in] name Parameter passed to the helper.
 * @param[in] ext Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_path_has_ext_(const char *name, const char *ext)
{
    const size_t nlen = (name != NULL) ? strlen(name) : 0U;
    const size_t elen = (ext != NULL) ? strlen(ext) : 0U;
    if((nlen == 0U) || (elen == 0U) || (nlen < elen))
    {
        return false;
    }
    return strcmp(&name[nlen - elen], ext) == 0;
}

/**
 * @brief Internal helper for `menu_nfc_sd_file_has_page_dump`.
 *
 * @param[in] rel_path Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_sd_file_has_page_dump_(const char *rel_path)
{
    char abs_path[128];
    char line[160];
    FILE *f = NULL;
    bool has_pages = false;

    if(rel_path == NULL)
    {
        return false;
    }

    (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
    f = fopen(abs_path, "r");
    if(f == NULL)
    {
        return false;
    }

    while(fgets(line, sizeof(line), f) != NULL)
    {
        const char *p = line;
        while((*p == ' ') || (*p == '\t'))
        {
            p++;
        }

        if(strncmp(p, "Page 0:", 7) == 0)
        {
            has_pages = true;
            break;
        }
    }

    (void)fclose(f);
    return has_pages;
}

static bool menu_nfc_sd_file_is_mfc_dump_(const char* rel_path)
{
    char abs_path[128];
    char line[160];
    FILE* file;
    bool is_classic = false;

    if(rel_path == NULL) return false;
    (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
    file = fopen(abs_path, "r");
    if(file == NULL) return false;
    while(fgets(line, sizeof(line), file) != NULL)
    {
        if(strstr(line, "Device type: Mifare Classic") != NULL)
        {
            is_classic = true;
            break;
        }
    }
    (void)fclose(file);
    return is_classic;
}

/**
 * @brief Internal helper for `menu_nfc_sd_add_item`.
 *
 * @param[in] name Parameter passed to the helper.
 * @param[in] rel_path Parameter passed to the helper.
 * @param[in] kind Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_sd_add_item_(const char *name, const char *rel_path, menu_nfc_sd_item_kind_t kind)
{
    if((name == NULL) || (rel_path == NULL) || (s_sd_dump_count >= MENU_NFC_SD_MAX_FILES))
    {
        return;
    }

    (void)memset(&s_sd_dump_files[s_sd_dump_count], 0, sizeof(s_sd_dump_files[s_sd_dump_count]));
    (void)strncpy(s_sd_dump_files[s_sd_dump_count].name, name, MENU_NFC_SD_NAME_MAX - 1U);
    (void)strncpy(s_sd_dump_files[s_sd_dump_count].rel_path, rel_path,
                  sizeof(s_sd_dump_files[s_sd_dump_count].rel_path) - 1U);
    s_sd_dump_files[s_sd_dump_count].kind = kind;
    s_sd_dump_count++;
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @param[in] kind_filter Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_sd_load_items_(menu_nfc_sd_item_kind_t kind_filter)
{
    s_sd_dump_count = 0;
    s_sd_dump_selected = 0;
    s_sd_dump_scroll = 0;

    if(sd_card_is_not_mounted())
    {
        sd_card_begin();
        if(sd_card_mount() != ESP_OK)
        {
            return;
        }
    }

    {
        static const char dump_dir[] = "/nfc";
        char dir_path[96];
        DIR *dir;
        struct dirent *ent;

        (void)snprintf(dir_path, sizeof(dir_path), "%s%s", SD_CARD_PATH, dump_dir);
        dir = opendir(dir_path);
        if(dir == NULL)
        {
            return;
        }
        while((ent = readdir(dir)) != NULL)
        {
            const char *name;
            size_t name_len;
            char rel_path[96];
            const size_t prefix_len = sizeof(dump_dir) - 1U;

            if(ent->d_name[0] == '.')
                continue;
            name = ent->d_name;
            name_len = strlen(name);
            if(name_len < 4U || !menu_nfc_path_has_ext_(name, ".nfc"))
                continue;

            if((prefix_len + 1U + name_len + 1U) > sizeof(rel_path))
                continue;
            (void)memcpy(rel_path, dump_dir, prefix_len);
            rel_path[prefix_len] = '/';
            (void)memcpy(&rel_path[prefix_len + 1U], name, name_len);
            rel_path[prefix_len + 1U + name_len] = '\0';
            if(kind_filter == MENU_NFC_SD_ITEM_MFC_DUMP &&
               !menu_nfc_sd_file_is_mfc_dump_(rel_path))
            {
                continue;
            }
            menu_nfc_sd_add_item_(name, rel_path, kind_filter);
        }
        (void)closedir(dir);
    }

    if(s_sd_dump_count > 1)
    {
        qsort(s_sd_dump_files, (size_t)s_sd_dump_count, sizeof(s_sd_dump_files[0]),
              menu_nfc_sd_dump_cmp_);
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_emulate_sd_list_(void)
{
    const int count = s_sd_dump_count;
    const bool restore = s_state == MENU_NFC_STATE_RESTORE_SD_LIST;

    if(count <= 0)
    {
        menu_nfc_draw_frame_(restore ? "RESTORE" : "NFC");
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(restore ? F("No Classic dumps") : F("No SD dumps"));
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    if(s_sd_dump_selected < 0) s_sd_dump_selected = 0;
    if(s_sd_dump_selected > (count - 1)) s_sd_dump_selected = count - 1;

    if(s_sd_dump_selected < s_sd_dump_scroll) s_sd_dump_scroll = s_sd_dump_selected;
    if(s_sd_dump_selected >= (s_sd_dump_scroll + VISIBLE_ROWS)) s_sd_dump_scroll = s_sd_dump_selected - VISIBLE_ROWS + 1;

    int max_scroll = count - VISIBLE_ROWS;
    if(max_scroll < 0) max_scroll = 0;
    if(s_sd_dump_scroll < 0) s_sd_dump_scroll = 0;
    if(s_sd_dump_scroll > max_scroll) s_sd_dump_scroll = max_scroll;

    menu_nfc_draw_frame_(restore ? "RESTORE" : "NFC");

    for(int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_sd_dump_scroll + row;
        if(idx >= count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        char line[22];
        (void)snprintf(line, sizeof(line), "%s:%.17s",
                       restore ? "MFC" : "NFC", s_sd_dump_files[idx].name);
        (void)poom_arduboy_print(line);

        if(idx == s_sd_dump_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

static void menu_nfc_draw_restore_mode_(void)
{
    menu_nfc_draw_frame_("RESTORE");

    poom_arduboy_set_cursor(4, 17);
    (void)poom_arduboy_print(F("DATA ONLY"));
    if(!s_restore_write_trailers)
        poom_arduboy_fill_rect(0, 16, ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);

    poom_arduboy_set_cursor(4, 29);
    (void)poom_arduboy_print(F("FULL + KEYS"));
    if(s_restore_write_trailers)
        poom_arduboy_fill_rect(0, 28, ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);

    poom_arduboy_set_cursor(4, 42);
    (void)poom_arduboy_print(s_restore_write_trailers ? F("Writes access bits")
                                                       : F("Skips sector keys"));
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:START"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void storage_adjust_scroll_(int count)
{
    if (count <= 0)
    {
        s_store_selected = 0;
        s_store_scroll = 0;
        return;
    }

    if (s_store_selected < 0)
    {
        s_store_selected = 0;
    }
    if (s_store_selected > (count - 1))
    {
        s_store_selected = count - 1;
    }

    if (s_store_selected < s_store_scroll)
    {
        s_store_scroll = s_store_selected;
    }
    if (s_store_selected >= (s_store_scroll + VISIBLE_ROWS))
    {
        s_store_scroll = s_store_selected - VISIBLE_ROWS + 1;
    }

    int max_scroll = count - VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }

    if (s_store_scroll < 0)
    {
        s_store_scroll = 0;
    }
    if (s_store_scroll > max_scroll)
    {
        s_store_scroll = max_scroll;
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_storage_list_(void)
{
    const int count = (int)s_store_cache.count;
    storage_adjust_scroll_(count);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(F("NFC"));

    char top[22];
    (void)snprintf(top, sizeof(top), "Cards:%u/%u", (unsigned)s_store_cache.count, (unsigned)POOM_NFC_STORE_MAX_CARDS);

    int16_t x_cards = (int16_t)(ARDUBOY_WIDTH - (int)strlen(top) * 6);
    if (x_cards < 30)
    {
        x_cards = 30;
    }
    poom_arduboy_set_cursor(x_cards, 2);
    (void)poom_arduboy_print(top);

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_store_scroll + row;
        if (idx >= count)
        {
            break;
        }

        const poom_nfc_card_id_t *id = &s_store_cache.cards[idx];
        char uid[POOM_NFC_CARD_UID_MAX * 2U + 1U];
        menu_nfc_format_uid_hex_(id, uid, sizeof(uid));

        char line[22];
        (void)snprintf(line, sizeof(line), "%c:%.18s", menu_nfc_type_char_(id->type), uid);

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);

        if (idx == s_store_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:DEL R:CLR"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_emulate_list_(void)
{
    const int count = (int)s_store_cache.count;
    storage_adjust_scroll_(count);

    menu_nfc_draw_frame_("NFC");


    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_store_scroll + row;
        if (idx >= count)
        {
            break;
        }

        const poom_nfc_card_id_t *id = &s_store_cache.cards[idx];
        char uid[POOM_NFC_CARD_UID_MAX * 2U + 1U];
        menu_nfc_format_uid_hex_(id, uid, sizeof(uid));

        char line[22];
        (void)snprintf(line, sizeof(line), "%c:%.18s", menu_nfc_type_char_(id->type), uid);

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);

        if (idx == s_store_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:START"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_emulate_running_(void)
{
    poom_nfc_emu_cfg_t emu_cfg;
    const char *mode_label = "NFC-A";

    menu_nfc_draw_frame_("NFC");

    poom_nfc_emulator_get_config(&emu_cfg);
    if(emu_cfg.mode == POOM_NFC_EMU_MODE_T4T)
    {
        mode_label = "T4T";
    }
    else if(emu_cfg.mode == POOM_NFC_EMU_MODE_MFUL)
    {
        mode_label = "MFUL";
    }
    char uid[POOM_NFC_CARD_UID_MAX * 2U + 1U];
    uid[0] = '\0';

    if(s_emu_active_id_valid)
    {
        menu_nfc_format_uid_hex_(&s_emu_active_id, uid, sizeof(uid));
    }

    poom_arduboy_set_cursor(4, MENU_NFC_INFO_Y0);
    {
        char mode_line[22];
        (void)snprintf(mode_line, sizeof(mode_line), "Emulating %s...", mode_label);
        (void)poom_arduboy_print(mode_line);
    }

    poom_arduboy_set_cursor(2, (int16_t)(MENU_NFC_INFO_Y0 + MENU_NFC_INFO_STEP));
    if (s_emu_active_id_valid)
    {
        char uid_compact[22];
        menu_nfc_format_uid_compact_(&s_emu_active_id, uid_compact, sizeof(uid_compact));

        char line_uid[22];
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "UID%u:%s", (unsigned)s_emu_active_id.uid_len, uid_compact);
        (void)snprintf(line_uid, sizeof(line_uid), "%.21s", tmp);
        (void)poom_arduboy_print(line_uid);
    }
    else
    {
        (void)poom_arduboy_print(F("UID:<none>"));
    }

    poom_arduboy_set_cursor(2, (int16_t)(MENU_NFC_INFO_Y0 + 2 * MENU_NFC_INFO_STEP));
    if (s_emu_active_id_valid &&
        ((s_emu_active_id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U) &&
        ((s_emu_active_id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
    {
        char line2[22];
        (void)snprintf(line2,
                       sizeof(line2),
                       "ATQA:%02X %02X SAK:%02X",
                       (unsigned)s_emu_active_id.atqa[0],
                       (unsigned)s_emu_active_id.atqa[1],
                       (unsigned)s_emu_active_id.sak);
        (void)poom_arduboy_print(line2);
    }
    else
    {
        (void)poom_arduboy_print(F("ATQA/SAK: N/A"));
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:STOP"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_draw_confirm_(const char *title)
{
    menu_nfc_draw_frame_("NFC");

    poom_arduboy_set_cursor(4, 22);
    (void)poom_arduboy_print(title ? title : "Confirm?");

    poom_arduboy_set_cursor(4, 34);
    if (s_yes_selected)
    {
        (void)poom_arduboy_print(F("[YES] NO"));
    }
    else
    {
        (void)poom_arduboy_print(F("YES [NO]"));
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("L/R:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("A:OK B:BK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_info_(void)
{
    menu_nfc_draw_frame_("NFC");

    poom_arduboy_set_cursor(4, 24);
    (void)poom_arduboy_print(s_info_line0);

    poom_arduboy_set_cursor(4, 36);
    (void)poom_arduboy_print(s_info_line1);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_nfc_set_info_return`.
 *
 * @param[in] l0 Parameter passed to the helper.
 * @param[in] l1 Parameter passed to the helper.
 * @param[in] return_state Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_set_info_return_(const char *l0, const char *l1, menu_nfc_state_t return_state)
{
    (void)snprintf(s_info_line0, sizeof(s_info_line0), "%.21s", l0 ? l0 : "");
    (void)snprintf(s_info_line1, sizeof(s_info_line1), "%.21s", l1 ? l1 : "");
    s_info_return_state = return_state;
    s_info_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MENU_NFC_INFO_HOLD_MS);
    s_state = MENU_NFC_STATE_INFO;
    menu_nfc_request_redraw_();
}

/**
 * @brief Internal helper for `menu_nfc_set_info`.
 *
 * @param[in] l0 Parameter passed to the helper.
 * @param[in] l1 Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_set_info_(const char *l0, const char *l1)
{
    menu_nfc_set_info_return_(l0, l1, MENU_NFC_STATE_MAIN);
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @return void
 */
static void menu_nfc_load_store_cache_(void)
{
    (void)memset(&s_store_cache, 0, sizeof(s_store_cache));
    (void)poom_nfc_store_load(&s_store_cache);
    s_store_selected = 0;
    s_store_scroll = 0;
}

/**
 * @brief Internal helper for `menu_nfc_emu_supported`.
 *
 * @param[in] id Parameter passed to the helper.
 * @return bool
 */
static bool menu_nfc_emu_supported_(const poom_nfc_card_id_t *id)
{
    if (!poom_nfc_card_id_is_valid(id))
    {
        return false;
    }

    if (!((id->type == 0U) || (id->type == 10U)))
    {
        return false;
    }
    if ((id->uid_len != 4U) && (id->uid_len != 7U))
    {
        return false;
    }
    if (((id->flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U) || ((id->flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U))
    {
        return false;
    }

    return true;
}

/**
 * @brief Stops the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_nfc_emu_stop_(void)
{
    poom_nfc_emulator_stop();
    poom_nfc_controller_stop();
    s_emu_active_id_valid = false;
}

/**
 * @brief Internal helper for `menu_nfc_emu_sync_active_id_from_cfg`.
 *
 * @return void
 */
static void menu_nfc_emu_sync_active_id_from_cfg_(void)
{
    poom_nfc_emu_cfg_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    poom_nfc_emulator_get_config(&cfg);

    (void)memset(&s_emu_active_id, 0, sizeof(s_emu_active_id));
    s_emu_active_id.type = 0U;
    s_emu_active_id.uid_len = cfg.uid_len;
    if (s_emu_active_id.uid_len > POOM_NFC_CARD_UID_MAX)
    {
        s_emu_active_id.uid_len = POOM_NFC_CARD_UID_MAX;
    }
    (void)memcpy(s_emu_active_id.uid, cfg.uid, s_emu_active_id.uid_len);

    s_emu_active_id.flags = 0U;
    if (cfg.atqa_set)
    {
        s_emu_active_id.flags |= POOM_NFC_CARD_FLAG_ATQA_SET;
        (void)memcpy(s_emu_active_id.atqa, cfg.atqa, sizeof(s_emu_active_id.atqa));
    }
    if (cfg.sak_set)
    {
        s_emu_active_id.flags |= POOM_NFC_CARD_FLAG_SAK_SET;
        s_emu_active_id.sak = cfg.sak;
    }

    s_emu_active_id_valid = poom_nfc_card_id_is_valid(&s_emu_active_id);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] rel_path Parameter passed to the helper.
 * @param[in] fail_return_state Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emu_start_mful_image_(const char *rel_path, menu_nfc_state_t fail_return_state)
{
    char abs_path[128];

    if(rel_path == NULL)
    {
        return;
    }

    if(poom_nfc_emulator_is_running())
    {
        menu_nfc_emu_stop_();
    }

    (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);

    poom_nfc_emulator_reset_config();
    (void)poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_MFUL);

    if(!poom_nfc_emulator_set_mful_image_file(abs_path))
    {
        menu_nfc_set_info_return_("Emu image config", "failed", fail_return_state);
        return;
    }

    if(!poom_nfc_controller_start())
    {
        menu_nfc_set_info_return_("NFC start failed", "", fail_return_state);
        return;
    }

    if(!poom_nfc_emulator_start())
    {
        poom_nfc_controller_stop();
        menu_nfc_set_info_return_("Emu start failed", "", fail_return_state);
        return;
    }

    s_emu_source_sd = true;
    s_emu_running_return_state = fail_return_state;
    menu_nfc_emu_sync_active_id_from_cfg_();
    s_state = MENU_NFC_STATE_EMULATE_RUNNING;
    menu_nfc_request_redraw_();
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] id Parameter passed to the helper.
 * @param[in] from_sd Parameter passed to the helper.
 * @param[in] fail_return_state Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emu_start_id_(const poom_nfc_card_id_t *id, bool from_sd, menu_nfc_state_t fail_return_state)
{
    if(id == NULL)
    {
        return;
    }

    if(!menu_nfc_emu_supported_(id))
    {
        if(poom_nfc_card_id_is_valid(id) && ((id->type == 0U) || (id->type == 10U)) &&
           ((id->uid_len == 4U) || (id->uid_len == 7U)) &&
           (((id->flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U) || ((id->flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U)))
        {
            menu_nfc_set_info_return_("Missing ATQA/SAK", "Rescan + Save", fail_return_state);
        }
        else
        {
            menu_nfc_set_info_return_("Unsupported tag", "Use NFC-A 4/7", fail_return_state);
        }
        return;
    }

    if(poom_nfc_emulator_is_running())
    {
        menu_nfc_emu_stop_();
    }

    poom_nfc_emulator_reset_config();
    (void)poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_3A);
    if(!poom_nfc_emulator_set_uid(id->uid, id->uid_len))
    {
        menu_nfc_set_info_return_("Emu config err", "UID", fail_return_state);
        return;
    }
    (void)poom_nfc_emulator_set_atqa(id->atqa);
    (void)poom_nfc_emulator_set_sak(id->sak);

    if(!poom_nfc_controller_start())
    {
        menu_nfc_set_info_return_("NFC start failed", "", fail_return_state);
        return;
    }

    if(!poom_nfc_emulator_start())
    {
        poom_nfc_controller_stop();
        menu_nfc_set_info_return_("Emu start failed", "", fail_return_state);
        return;
    }

    s_emu_source_sd = from_sd;
    s_emu_active_id = *id;
    s_emu_active_id_valid = true;
    s_emu_running_return_state = fail_return_state;
    s_state = MENU_NFC_STATE_EMULATE_RUNNING;
    menu_nfc_request_redraw_();
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] fail_return_state Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_emu_start_t4t_default_(menu_nfc_state_t fail_return_state)
{
    if (poom_nfc_emulator_is_running())
    {
        menu_nfc_emu_stop_();
    }

    poom_nfc_emulator_reset_config();

    if (!poom_nfc_controller_start())
    {
        menu_nfc_set_info_return_("NFC start failed", "", fail_return_state);
        return;
    }

    if (!poom_nfc_emulator_start())
    {
        poom_nfc_controller_stop();
        menu_nfc_set_info_return_("Emu start failed", "", fail_return_state);
        return;
    }

    s_emu_source_sd = false;
    s_emu_running_return_state = MENU_NFC_STATE_EMULATE_SOURCE;
    menu_nfc_emu_sync_active_id_from_cfg_();
    s_state = MENU_NFC_STATE_EMULATE_RUNNING;
    menu_nfc_request_redraw_();
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_nfc_emu_start_selected_(void)
{
    if (s_store_cache.count == 0U)
    {
        menu_nfc_set_info_return_("No saved cards", "", MENU_NFC_STATE_EMULATE_LIST);
        return;
    }

    if ((s_store_selected < 0) || (s_store_selected >= (int)s_store_cache.count))
    {
        return;
    }

    const poom_nfc_card_id_t *id = &s_store_cache.cards[s_store_selected];
    menu_nfc_emu_start_id_(id, false, MENU_NFC_STATE_EMULATE_LIST);
}

static void menu_nfc_restore_progress_(uint16_t completed,
                                        uint16_t total,
                                        uint8_t block,
                                        void* user_ctx)
{
    char detail[22];
    (void)user_ctx;
    (void)snprintf(detail, sizeof(detail), "Block %u", (unsigned)block);
    menu_nfc_draw_busy_progress_("RESTORE MFC", detail, completed, total);
}

static void menu_nfc_run_restore_(void)
{
    poom_nfc_dump_t* target;
    poom_mifare_restore_result_t result = {0};
    poom_mifare_restore_status_t status;
    nfc_card_type_t card_type;
    uint16_t target_blocks = 0U;
    char line0[22];
    char line1[22];

    target = (poom_nfc_dump_t*)calloc(1U, sizeof(*target));
    if(target == NULL)
    {
        menu_nfc_set_info_return_("No RAM for restore", "", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }

    menu_nfc_draw_busy_progress_("RESTORE MFC", "Scan target card", 0U, 1U);
    if(!poom_nfc_controller_capture_dump(MENU_NFC_SCAN_TIMEOUT_MS, target))
    {
        poom_nfc_controller_stop();
        free(target);
        menu_nfc_set_info_return_("Target not found", "Hold card near", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }
    card_type = menu_nfc_scan_type_from_dump_(target);
    if(!poom_mifare_classic_is_supported_card(card_type))
    {
        poom_nfc_controller_stop();
        free(target);
        menu_nfc_set_info_return_("Not MIFARE Classic", "Wrong target card", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }

    menu_nfc_draw_busy_progress_("RESTORE MFC", "Discovering keys", 0U, 1U);
    if(!poom_nfc_controller_connect() ||
       !poom_mifare_classic_bind_card(target->id.uid, target->id.uid_len, card_type))
    {
        poom_nfc_controller_stop();
        free(target);
        menu_nfc_set_info_return_("Connect failed", "Hold card steady", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }
    target_blocks = (uint16_t)(poom_mifare_classic_get_max_block() + 1U);
    status = poom_mifare_classic_restore_file(
        s_restore_rel_path,
        s_restore_write_trailers,
        &result,
        menu_nfc_restore_progress_,
        NULL);
    poom_nfc_controller_stop();
    free(target);

    printf("  mifare restore: status=%d source=%u compared=%u same=%u written=%u "
           "verified=%u skipped=%u failed=%u trailers=%u block0_diff=%s\r\n",
           (int)status,
           (unsigned)result.source_blocks,
           (unsigned)result.compared,
           (unsigned)result.unchanged,
           (unsigned)result.written,
           (unsigned)result.verified,
           (unsigned)result.skipped,
           (unsigned)result.failed,
           (unsigned)result.trailers_written,
           result.block0_different ? "yes" : "no");

    if(status == POOM_MIFARE_RESTORE_INVALID_FILE)
    {
        menu_nfc_set_info_return_("Invalid Classic dump", "", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }
    if(status == POOM_MIFARE_RESTORE_SIZE_MISMATCH)
    {
        (void)snprintf(line1, sizeof(line1), "File:%u Card:%u",
                       (unsigned)result.source_blocks, (unsigned)target_blocks);
        menu_nfc_set_info_return_("Size mismatch", line1, MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }
    if(status == POOM_MIFARE_RESTORE_NO_MEMORY)
    {
        menu_nfc_set_info_return_("No PSRAM for dump", "", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }
    if(status == POOM_MIFARE_RESTORE_NO_CARD)
    {
        menu_nfc_set_info_return_("Card disconnected", "Try again", MENU_NFC_STATE_RESTORE_SD_LIST);
        return;
    }

    (void)snprintf(line0, sizeof(line0), "W%u S%u V%u",
                   (unsigned)result.written,
                   (unsigned)result.unchanged,
                   (unsigned)result.verified);
    (void)snprintf(line1, sizeof(line1), "F%u K%u%s",
                   (unsigned)result.failed,
                   (unsigned)result.skipped,
                   result.block0_different ? " B0" : "");
    menu_nfc_set_info_return_(line0, line1, MENU_NFC_STATE_RESTORE_SD_LIST);
}

/**
 * @brief Internal helper for `menu_nfc_run_scan`.
 *
 * @return void
 */
static void menu_nfc_run_scan_(void)
{
    nfc_card_type_t card_type;
    poom_nfc_profile_t profile;
    bool profile_ok = false;

    menu_nfc_draw_scanning_();

    if(!menu_nfc_scan_meta_acquire_())
    {
        menu_nfc_set_info_return_("No RAM for read", "", MENU_NFC_STATE_MAIN);
        return;
    }

    (void)memset(&s_scan_dump, 0, sizeof(s_scan_dump));
    s_scan_dump_valid = false;
    s_scan_has_ndef = false;
    s_scan_ndef_known = false;
    menu_nfc_scan_meta_reset_();
    menu_nfc_scan_actions_reset_();

    /*
     * Keep a lightweight connect-first attempt only to preserve ATS for
     * ISO-DEP cards. Do not probe EMV from the automatic scan path.
     */
    menu_nfc_draw_busy_progress_("READING CARD", "Linking...", 1U, 2U);
    MENU_NFC_TRACE("scan start");
    if(poom_nfc_controller_connect() &&
       poom_reader_get_last_profile(&profile) &&
       (profile.ats_len > 0U) &&
       menu_nfc_fill_dump_from_profile_(&profile, &s_scan_dump))
    {
        s_scan_dump_valid = true;
        s_scan_ndef_known = false;
        s_scan_has_ndef = false;
        s_scan_meta->profile = profile;
        s_scan_meta->profile_valid = true;
        profile_ok = true;
        MENU_NFC_TRACE(
            "scan connect-first ats=%u sak=%02X",
            (unsigned)profile.ats_len,
            (unsigned)(profile.sak_set ? profile.sak : 0U));
        poom_nfc_controller_stop();
    }
    else
    {
        MENU_NFC_TRACE("scan connect-first miss");
        poom_nfc_controller_stop();
    }

    if(!profile_ok)
    {
        const bool ok = poom_nfc_controller_capture_dump(MENU_NFC_SCAN_TIMEOUT_MS, &s_scan_dump);
        poom_nfc_controller_stop();
        MENU_NFC_TRACE("scan dump capture=%s", ok ? "ok" : "fail");

        if(!ok)
        {
            menu_nfc_set_info_return_("Scan failed", "Try again", MENU_NFC_STATE_MAIN);
            return;
        }

        s_scan_dump_valid = true;
        s_scan_ndef_known = (s_scan_dump.read_mode == POOM_NFC_DUMP_READ_FULL) && s_scan_dump.read_ok;
        s_scan_has_ndef = s_scan_ndef_known ? menu_nfc_dump_has_ndef_(&s_scan_dump) : false;
    }

    card_type = menu_nfc_scan_type_from_dump_(&s_scan_dump);
    MENU_NFC_TRACE(
        "scan dump type=%s isodep=%s read_mode=%u",
        nfc_ident_card_type_to_str(card_type),
        menu_nfc_scan_is_isodep_candidate_(&s_scan_dump) ? "yes" : "no",
        (unsigned)s_scan_dump.read_mode);
    menu_nfc_prepare_basic_scan_view_(card_type);
    s_scan_meta->info_scroll = 0U;
    s_state = menu_nfc_scan_primary_state_();
    menu_nfc_request_redraw_();
}

/**
 * @brief Saves the current EMV summary as a small `.nfc` text file.
 *
 * @param[out] out_rel_path Parameter passed to the helper.
 * @param[in] out_rel_path_len Parameter passed to the helper.
 * @return esp_err_t
 */
static void menu_nfc_emv_file_write_hex_(FILE* f, const uint8_t* data, size_t data_len)
{
    static const char k_hex[] = "0123456789ABCDEF";
    char encoded[64];
    size_t encoded_len = 0U;

    if(f == NULL || (data == NULL && data_len > 0U))
    {
        return;
    }
    for(size_t i = 0U; i < data_len; i++)
    {
        encoded[encoded_len++] = k_hex[(data[i] >> 4U) & 0x0FU];
        encoded[encoded_len++] = k_hex[data[i] & 0x0FU];
        if(encoded_len == sizeof(encoded))
        {
            (void)fwrite(encoded, 1U, encoded_len, f);
            encoded_len = 0U;
        }
    }
    if(encoded_len > 0U)
    {
        (void)fwrite(encoded, 1U, encoded_len, f);
    }
}

static esp_err_t menu_nfc_scan_save_emv_summary_(char* out_rel_path,
                                                 size_t out_rel_path_len,
                                                 int* out_io_errno)
{
    char aid_name[33];
    char uid_name[21];
    char rel_path[96];
    char abs_path[160];
    char file_buffer[256];
    FILE* f = NULL;

    if(out_io_errno != NULL)
    {
        *out_io_errno = 0;
    }

    if(out_rel_path != NULL && out_rel_path_len > 0U)
    {
        out_rel_path[0] = '\0';
    }

    if(sd_card_is_not_mounted())
    {
        sd_card_begin();
        const esp_err_t mount_err = sd_card_mount();
        if(mount_err != ESP_OK)
        {
            return mount_err;
        }
    }

    {
        const esp_err_t dir_err = sd_card_create_dir("/nfc");
        if(dir_err != ESP_OK)
        {
            return dir_err;
        }
    }
    aid_name[0] = '\0';
    if((s_scan_meta != NULL) && s_scan_meta->emv_data_valid &&
       s_scan_meta->emv_data->aid_len > 0U)
    {
        size_t w = 0U;
        for(size_t i = 0U;
            i < s_scan_meta->emv_data->aid_len && w + 2U < sizeof(aid_name);
            i++)
        {
            w += (size_t)snprintf(
                &aid_name[w], sizeof(aid_name) - w, "%02X", s_scan_meta->emv_data->aid[i]);
        }
    }
    else if((s_scan_meta != NULL) && (s_scan_meta->summary_lines[2][0] != '\0'))
    {
        size_t w = 0U;
        for(size_t i = 0U; i < strlen(s_scan_meta->summary_lines[2]) && w < sizeof(aid_name) - 1U; i++)
        {
            const char c = s_scan_meta->summary_lines[2][i];
            if((c >= '0') && (c <= '9'))
            {
                aid_name[w++] = c;
            }
            else if((c >= 'A') && (c <= 'F'))
            {
                aid_name[w++] = c;
            }
        }
        aid_name[w] = '\0';
    }
    if(aid_name[0] == '\0')
    {
        (void)snprintf(aid_name, sizeof(aid_name), "%llu", (unsigned long long)(esp_timer_get_time() / 1000ULL));
    }

    uid_name[0] = '\0';
    if(s_scan_dump_valid && s_scan_dump.id.uid_len > 0U)
    {
        menu_nfc_format_uid_hex_(&s_scan_dump.id, uid_name, sizeof(uid_name));
    }
    if(uid_name[0] != '\0')
    {
        const char* scheme_id = "UN";
        if((s_scan_meta != NULL) && s_scan_meta->emv_data_valid)
        {
            if(s_scan_meta->emv_data->scheme == POOM_NFC_EMV_SCHEME_VISA)
            {
                scheme_id = "VI";
            }
            else if(s_scan_meta->emv_data->scheme == POOM_NFC_EMV_SCHEME_MASTERCARD)
            {
                scheme_id = "MC";
            }
        }

        /*
         * CONFIG_FATFS_MAX_LFN is 31. A 10-byte UID is 20 hex characters, so
         * "EMV_<UID>_<scheme>.nfc" is exactly 31 characters at most. The full
         * AID remains stored inside the file.
         */
        (void)snprintf(rel_path, sizeof(rel_path), "/nfc/EMV_%s_%s.nfc", uid_name, scheme_id);
    }
    else
    {
        /* Keep the basename within the configured FATFS 31-character limit. */
        (void)snprintf(rel_path, sizeof(rel_path), "/nfc/EMV_%.22s.nfc", aid_name);
    }
    (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);

    errno = 0;
    f = fopen(abs_path, "w");
    if(f == NULL)
    {
        if(out_io_errno != NULL)
        {
            *out_io_errno = (errno != 0) ? errno : EIO;
        }
        return ESP_FAIL;
    }
    /* A small caller-owned buffer avoids heap use without turning every hex
     * byte into a separate FATFS write, which can stall long EMV saves. */
    (void)setvbuf(f, file_buffer, _IOFBF, sizeof(file_buffer));

    (void)fprintf(f, "Filetype: POOM NFC device\n");
    (void)fprintf(f, "Version: 2\n");
    (void)fprintf(f, "Device type: EMV\n");
    (void)fprintf(f, "Summary: %s\n", (s_scan_meta != NULL) ? s_scan_meta->summary_lines[1] : "");
    if(s_scan_dump_valid && s_scan_dump.id.uid_len > 0U)
    {
        (void)fprintf(f, "UID: ");
        menu_nfc_emv_file_write_hex_(f, s_scan_dump.id.uid, s_scan_dump.id.uid_len);
        (void)fprintf(f, "\n");
    }
    if(s_scan_dump_valid &&
       (s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U)
    {
        (void)fprintf(f, "ATQA: ");
        menu_nfc_emv_file_write_hex_(f, s_scan_dump.id.atqa, sizeof(s_scan_dump.id.atqa));
        (void)fprintf(f, "\n");
    }
    if(s_scan_dump_valid &&
       (s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
    {
        (void)fprintf(f, "SAK: %02X\n", s_scan_dump.id.sak);
    }
    if(s_scan_meta != NULL && s_scan_meta->profile_valid && s_scan_meta->profile.ats_len > 0U)
    {
        (void)fprintf(f, "ATS: ");
        menu_nfc_emv_file_write_hex_(
            f, s_scan_meta->profile.ats, s_scan_meta->profile.ats_len);
        (void)fprintf(f, "\n");
    }
    if((s_scan_meta != NULL) && s_scan_meta->emv_data_valid)
    {
        (void)fprintf(f, "AID:");
        for(size_t i = 0U; i < s_scan_meta->emv_data->aid_len; i++)
        {
            (void)fprintf(f, "%02X", s_scan_meta->emv_data->aid[i]);
        }
        (void)fprintf(f, "\n");
    }
    else
    {
        (void)fprintf(f, "AID: %s\n", (s_scan_meta != NULL) ? s_scan_meta->summary_lines[2] : "");
    }
    if((s_scan_meta != NULL) && s_scan_meta->emv_data_valid)
    {
        const poom_nfc_emv_card_t* card = s_scan_meta->emv_data;
        char decoded[320];
        (void)fprintf(f, "Read status: %s\n", poom_nfc_emv_read_status_str(card->read_status));
        (void)fprintf(f, "Scheme: %s\n", poom_nfc_emv_scheme_str(card->scheme));
        (void)fprintf(f, "Kernel hint: %s\n", poom_nfc_emv_kernel_hint_str(card->scheme));
        (void)fprintf(f, "GPO: %s\n", card->gpo_succeeded ? "ok" : "failed");
        (void)fprintf(f, "AFL: %s\n", card->afl_present ? "present" : "not present");
        (void)fprintf(f, "Application label: %s\n", card->application_label);
        (void)fprintf(f, "Application name: %s\n", card->application_name);
        (void)fprintf(f, "Cardholder name: %s\n", card->cardholder_name);
        (void)fprintf(f, "PAN: %s\n", card->pan);
        if(card->has_aip)
        {
            (void)fprintf(f, "AIP: %02X %02X\n", card->aip[0], card->aip[1]);
            poom_nfc_emv_format_aip(card, decoded, sizeof(decoded));
            (void)fprintf(f, "AIP decoded: %s\n", decoded);
        }
        if(card->has_expiration_date)
        {
            (void)fprintf(f,
                          "Expiration date: %02X %02X %02X\n",
                          card->expiration_date[0],
                          card->expiration_date[1],
                          card->expiration_date[2]);
        }
        if(card->has_effective_date)
        {
            (void)fprintf(f,
                          "Effective date: %02X %02X %02X\n",
                          card->effective_date[0],
                          card->effective_date[1],
                          card->effective_date[2]);
        }
        if(card->has_country_code) (void)fprintf(f, "Country code: %04X\n", card->country_code);
        if(card->has_currency_code) (void)fprintf(f, "Currency code: %04X\n", card->currency_code);
        (void)fprintf(f,
                      "Optional data unsupported: %s\n",
                      card->optional_data_unsupported ? "yes" : "no");
        if(card->has_pan_sequence_number)
        {
            (void)fprintf(f, "PAN sequence number (5F34): %02X\n", card->pan_sequence_number);
        }
        if(card->has_service_code)
        {
            (void)fprintf(f, "Service code: %03u\n", (unsigned)card->service_code);
        }
        if(card->has_issuer_code_table_index)
        {
            (void)fprintf(f, "Issuer code table index (9F11): %02X\n", card->issuer_code_table_index);
        }
        if(card->pdol_definition_len > 0U)
        {
            (void)fprintf(f, "PDOL definition (9F38): ");
            menu_nfc_emv_file_write_hex_(f, card->pdol_definition, card->pdol_definition_len);
            (void)fprintf(f, "\n");
        }
        if(card->issuer_application_data_len > 0U)
        {
            (void)fprintf(f,
                          "Issuer application data (9F10, %s raw): ",
                          poom_nfc_emv_scheme_str(card->scheme));
            menu_nfc_emv_file_write_hex_(
                f, card->issuer_application_data, card->issuer_application_data_len);
            (void)fprintf(f, "\n");
        }
        if(card->application_cryptogram_len > 0U)
        {
            (void)fprintf(f, "Application cryptogram (9F26): ");
            menu_nfc_emv_file_write_hex_(
                f, card->application_cryptogram, card->application_cryptogram_len);
            (void)fprintf(f, "\n");
        }
        if(card->has_cryptogram_information_data)
        {
            (void)fprintf(f,
                          "Cryptogram information data (9F27): %02X (%s)\n",
                          card->cryptogram_information_data,
                          poom_nfc_emv_cryptogram_type_str(card->cryptogram_information_data));
        }
        if(card->application_program_identifier_len > 0U)
        {
            (void)fprintf(f, "Visa application program identifier (9F5A): ");
            menu_nfc_emv_file_write_hex_(f,
                                         card->application_program_identifier,
                                         card->application_program_identifier_len);
            (void)fprintf(f, "\n");
        }
        if(card->has_offline_spending_amount)
        {
            (void)fprintf(f,
                          "Visa available offline spending amount (9F5D): %llu\n",
                          (unsigned long long)card->offline_spending_amount);
        }
        if(card->card_transaction_qualifiers_len > 0U)
        {
            (void)fprintf(f, "Visa card transaction qualifiers (9F6C): ");
            menu_nfc_emv_file_write_hex_(f,
                                         card->card_transaction_qualifiers,
                                         card->card_transaction_qualifiers_len);
            (void)fprintf(f, "\n");
        }
        if(card->form_factor_indicator_len > 0U)
        {
            (void)fprintf(f, "Visa form factor indicator (9F6E): ");
            menu_nfc_emv_file_write_hex_(
                f, card->form_factor_indicator, card->form_factor_indicator_len);
            (void)fprintf(f, "\n");
        }
        if(card->details != NULL)
        {
            const poom_nfc_emv_details_t* details = card->details;
            if(details->language_preferences_len > 0U)
            {
                char languages[POOM_NFC_EMV_LANGUAGE_MAX_LEN + 1U];
                menu_nfc_emv_copy_ascii_(details->language_preferences,
                                         details->language_preferences_len,
                                         languages,
                                         sizeof(languages));
                (void)fprintf(f, "Language preference (5F2D): %s [", languages);
                menu_nfc_emv_file_write_hex_(
                    f, details->language_preferences, details->language_preferences_len);
                (void)fprintf(f, "]\n");
            }
            if(details->has_application_version_number)
            {
                (void)fprintf(f,
                              "Application version number (9F08): %04X\n",
                              details->application_version_number);
            }
            if(details->has_application_usage_control)
            {
                (void)fprintf(f,
                              "Application usage control (9F07): %02X%02X\n",
                              details->application_usage_control[0],
                              details->application_usage_control[1]);
                poom_nfc_emv_format_auc(card, decoded, sizeof(decoded));
                (void)fprintf(f, "Application usage decoded: %s\n", decoded);
            }
            if(details->cvm_list_len > 0U)
            {
                (void)fprintf(f, "CVM list (8E): ");
                menu_nfc_emv_file_write_hex_(f, details->cvm_list, details->cvm_list_len);
                (void)fprintf(f, "\n");
                poom_nfc_emv_format_cvm(card, decoded, sizeof(decoded));
                (void)fprintf(f, "CVM methods decoded: %s\n", decoded);
                if(details->cvm_list_len >= 8U)
                {
                    const uint32_t amount_x = ((uint32_t)details->cvm_list[0] << 24U) |
                                              ((uint32_t)details->cvm_list[1] << 16U) |
                                              ((uint32_t)details->cvm_list[2] << 8U) |
                                              details->cvm_list[3];
                    const uint32_t amount_y = ((uint32_t)details->cvm_list[4] << 24U) |
                                              ((uint32_t)details->cvm_list[5] << 16U) |
                                              ((uint32_t)details->cvm_list[6] << 8U) |
                                              details->cvm_list[7];
                    (void)fprintf(f, "CVM amount X: %lu\n", (unsigned long)amount_x);
                    (void)fprintf(f, "CVM amount Y: %lu\n", (unsigned long)amount_y);
                }
            }
            if(details->cdol1_len > 0U)
            {
                (void)fprintf(f, "CDOL1 (8C): ");
                menu_nfc_emv_file_write_hex_(f, details->cdol1, details->cdol1_len);
                (void)fprintf(f, "\n");
                poom_nfc_emv_format_dol(details->cdol1,
                                         details->cdol1_len,
                                         decoded,
                                         sizeof(decoded));
                (void)fprintf(f, "CDOL1 decoded: %s\n", decoded);
            }
            if(details->cdol2_len > 0U)
            {
                (void)fprintf(f, "CDOL2 (8D): ");
                menu_nfc_emv_file_write_hex_(f, details->cdol2, details->cdol2_len);
                (void)fprintf(f, "\n");
                poom_nfc_emv_format_dol(details->cdol2,
                                         details->cdol2_len,
                                         decoded,
                                         sizeof(decoded));
                (void)fprintf(f, "CDOL2 decoded: %s\n", decoded);
            }
            if(details->has_issuer_action_code_default)
            {
                (void)fprintf(f, "Issuer action code default (9F0D): ");
                menu_nfc_emv_file_write_hex_(
                    f, details->issuer_action_code_default, POOM_NFC_EMV_IAC_LEN);
                (void)fprintf(f, "\n");
            }
            if(details->has_issuer_action_code_denial)
            {
                (void)fprintf(f, "Issuer action code denial (9F0E): ");
                menu_nfc_emv_file_write_hex_(
                    f, details->issuer_action_code_denial, POOM_NFC_EMV_IAC_LEN);
                (void)fprintf(f, "\n");
            }
            if(details->has_issuer_action_code_online)
            {
                (void)fprintf(f, "Issuer action code online (9F0F): ");
                menu_nfc_emv_file_write_hex_(
                    f, details->issuer_action_code_online, POOM_NFC_EMV_IAC_LEN);
                (void)fprintf(f, "\n");
            }
            if(details->sda_tag_list_len > 0U)
            {
                (void)fprintf(f, "SDA tag list (9F4A): ");
                menu_nfc_emv_file_write_hex_(f, details->sda_tag_list, details->sda_tag_list_len);
                (void)fprintf(f, "\n");
            }
            if(details->has_ca_public_key_index)
            {
                (void)fprintf(f,
                              "CA public key index (8F): %02X\n",
                              details->ca_public_key_index);
            }
            if(details->issuer_public_key_certificate_len > 0U)
            {
                (void)fprintf(f,
                              "Issuer public key certificate (90): present, %u bytes; full value in R-APDU capture\n",
                              (unsigned)details->issuer_public_key_certificate_len);
            }
            if(details->issuer_public_key_remainder_len > 0U)
            {
                (void)fprintf(f,
                              "Issuer public key remainder (92): present, %u bytes; full value in R-APDU capture\n",
                              (unsigned)details->issuer_public_key_remainder_len);
            }
            if(details->issuer_public_key_exponent_len > 0U)
            {
                (void)fprintf(f, "Issuer public key exponent (9F32): ");
                menu_nfc_emv_file_write_hex_(f,
                                             details->issuer_public_key_exponent,
                                             details->issuer_public_key_exponent_len);
                (void)fprintf(f, "\n");
            }
            if(details->icc_public_key_certificate_len > 0U)
            {
                (void)fprintf(f,
                              "ICC public key certificate (9F46): present, %u bytes; full value in R-APDU capture\n",
                              (unsigned)details->icc_public_key_certificate_len);
            }
            if(details->icc_public_key_remainder_len > 0U)
            {
                (void)fprintf(f,
                              "ICC public key remainder (9F48): present, %u bytes; full value in R-APDU capture\n",
                              (unsigned)details->icc_public_key_remainder_len);
            }
            if(details->icc_public_key_exponent_len > 0U)
            {
                (void)fprintf(f, "ICC public key exponent (9F47): ");
                menu_nfc_emv_file_write_hex_(f,
                                             details->icc_public_key_exponent,
                                             details->icc_public_key_exponent_len);
                (void)fprintf(f, "\n");
            }
            if(details->ddol_len > 0U)
            {
                (void)fprintf(f, "DDOL (9F49): ");
                menu_nfc_emv_file_write_hex_(f, details->ddol, details->ddol_len);
                (void)fprintf(f, "\n");
                poom_nfc_emv_format_dol(
                    details->ddol, details->ddol_len, decoded, sizeof(decoded));
                (void)fprintf(f, "DDOL decoded: %s\n", decoded);
            }
            if(details->customer_exclusive_data_len > 0U)
            {
                (void)fprintf(f, "Customer exclusive data (9F7C, raw prefix): ");
                menu_nfc_emv_file_write_hex_(f,
                                             details->customer_exclusive_data,
                                             details->customer_exclusive_data_len);
                (void)fprintf(f, "\n");
            }
            if(details->mastercard_application_capabilities_len > 0U)
            {
                (void)fprintf(f, "Mastercard application capabilities (9F5D): ");
                menu_nfc_emv_file_write_hex_(
                    f,
                    details->mastercard_application_capabilities,
                    details->mastercard_application_capabilities_len);
                (void)fprintf(f, "\n");
            }
            if(details->mastercard_9f6c_len > 0U)
            {
                (void)fprintf(f, "Mastercard profile-specific data (9F6C, raw): ");
                menu_nfc_emv_file_write_hex_(
                    f, details->mastercard_9f6c, details->mastercard_9f6c_len);
                (void)fprintf(f, "\n");
            }
            if(details->mastercard_third_party_data_len > 0U)
            {
                (void)fprintf(f, "Mastercard third party data (9F6E): ");
                menu_nfc_emv_file_write_hex_(f,
                                             details->mastercard_third_party_data,
                                             details->mastercard_third_party_data_len);
                (void)fprintf(f, "\n");
            }
        }
        if(card->has_pin_try_counter)
        {
            (void)fprintf(f, "PIN try counter: %u\n", (unsigned)card->pin_try_counter);
        }
        if(card->has_application_transaction_counter)
        {
            (void)fprintf(f,
                          "Application transaction counter: %u\n",
                          (unsigned)card->application_transaction_counter);
        }
        if(card->has_last_online_atc)
        {
            (void)fprintf(f, "Last online ATC: %u\n", (unsigned)card->last_online_atc);
        }
        (void)fprintf(f, "Transaction count: %u\n", (unsigned)card->transaction_count);
        for(size_t i = 0U; i < card->transaction_count; i++)
        {
            const poom_nfc_emv_transaction_t* transaction = &card->transactions[i];
            (void)fprintf(f, "Transaction %u:", (unsigned)(i + 1U));
            if(transaction->has_atc) (void)fprintf(f, " ATC=%u", (unsigned)transaction->atc);
            if(transaction->has_amount)
            {
                (void)fprintf(f, " amount=%llu", (unsigned long long)transaction->amount);
            }
            if(transaction->has_country_code)
            {
                (void)fprintf(f, " country=%04X", transaction->country_code);
            }
            if(transaction->has_currency_code)
            {
                (void)fprintf(f, " currency=%04X", transaction->currency_code);
            }
            if(transaction->has_date)
            {
                (void)fprintf(f,
                              " date=%02X%02X%02X",
                              transaction->date[0],
                              transaction->date[1],
                              transaction->date[2]);
            }
            if(transaction->has_time)
            {
                (void)fprintf(f,
                              " time=%02X%02X%02X",
                              transaction->time[0],
                              transaction->time[1],
                              transaction->time[2]);
            }
            (void)fprintf(f, "\n");
        }
        if(card->capture != NULL)
        {
            const poom_nfc_emv_capture_t* capture = card->capture;
            (void)fprintf(f, "APDU exchange count: %u\n", (unsigned)capture->exchange_count);
            (void)fprintf(f, "APDU capture truncated: %s\n", capture->truncated ? "yes" : "no");
            for(size_t i = 0U; i < capture->exchange_count; i++)
            {
                const poom_nfc_emv_exchange_t* exchange = &capture->exchanges[i];
                const bool command_valid =
                    (size_t)exchange->command_offset + exchange->command_len <= capture->data_len;
                const bool response_valid =
                    (size_t)exchange->response_offset + exchange->response_len <= capture->data_len;
                (void)fprintf(f,
                              "Exchange %u transport: %s\n",
                              (unsigned)(i + 1U),
                              exchange->transport_ok ? "ok" : "failed");
                (void)fprintf(f, "Exchange %u C-APDU: ", (unsigned)(i + 1U));
                if(command_valid)
                {
                    menu_nfc_emv_file_write_hex_(f,
                                                 &capture->data[exchange->command_offset],
                                                 exchange->command_len);
                }
                (void)fprintf(f, "\n");
                (void)fprintf(f, "Exchange %u R-APDU: ", (unsigned)(i + 1U));
                if(response_valid)
                {
                    menu_nfc_emv_file_write_hex_(f,
                                                 &capture->data[exchange->response_offset],
                                                 exchange->response_len);
                }
                (void)fprintf(f, "\n");
            }
        }
    }
    {
        int io_errno = 0;
        if(ferror(f) != 0)
        {
            io_errno = (errno != 0) ? errno : EIO;
        }
        errno = 0;
        if(fclose(f) != 0 && io_errno == 0)
        {
            io_errno = (errno != 0) ? errno : EIO;
        }
        if(io_errno != 0)
        {
            if(out_io_errno != NULL)
            {
                *out_io_errno = io_errno;
            }
            return ESP_FAIL;
        }
    }

    if(out_rel_path != NULL && out_rel_path_len > 0U)
    {
        (void)snprintf(out_rel_path, out_rel_path_len, "%s", rel_path);
    }

    return ESP_OK;
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_nfc_scan_save_to_sd_(void)
{
    esp_err_t err = ESP_FAIL;
    int emv_save_errno = 0;
    char rel_path[160];

    if(!s_scan_dump_valid)
    {
        menu_nfc_set_info_return_("No scan data", "", menu_nfc_scan_primary_state_());
        return;
    }

    rel_path[0] = '\0';
    menu_nfc_draw_busy_("SAVE .NFC", "Saving...");

    if((s_scan_meta != NULL) && (s_scan_meta->kind == MENU_NFC_SCAN_KIND_MIFARE_READ))
    {
        (void)poom_nfc_controller_connect();
        if(poom_mifare_classic_dump_to_flipper_file("/nfc", true, rel_path, sizeof(rel_path)))
        {
            err = ESP_OK;
        }
    }
    else if((s_scan_meta != NULL) && (s_scan_meta->kind == MENU_NFC_SCAN_KIND_EMV))
    {
        /* The card data and APDU capture are already copied; release RF/driver
         * memory before FATFS opens and writes the larger EMV report. */
        poom_nfc_controller_stop();
        err = menu_nfc_scan_save_emv_summary_(
            rel_path, sizeof(rel_path), &emv_save_errno);
    }
    else
    {
        err = poom_nfc_dump_save_to_sd(&s_scan_dump, rel_path, sizeof(rel_path));
    }
    poom_nfc_controller_stop();

    if(err == ESP_OK)
    {
        char line0[22];
        char line1[22];
        (void)snprintf(line0, sizeof(line0), "Saved");
        (void)snprintf(line1, sizeof(line1), "%.21s", (rel_path[0] != '\0') ? rel_path : "/nfc");
        menu_nfc_set_info_return_(line0, line1, menu_nfc_scan_primary_state_());
        return;
    }

    {
        char line0[22];
        char line1[22];
        (void)snprintf(line0, sizeof(line0), "SD save failed");
        if(emv_save_errno != 0)
        {
            (void)snprintf(line1, sizeof(line1), "I/O errno=%d", emv_save_errno);
        }
        else
        {
            (void)snprintf(line1, sizeof(line1), "err=%d", (int)err);
        }
        menu_nfc_set_info_return_(line0, line1, menu_nfc_scan_primary_state_());
    }
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_nfc_scan_save_embedded_(void)
{
    if(!s_scan_dump_valid || !poom_nfc_card_id_is_valid(&s_scan_dump.id))
    {
        menu_nfc_set_info_return_("No tag data", "", menu_nfc_scan_primary_state_());
        return;
    }

    size_t added = 0U;
    size_t already = 0U;
    size_t no_space = 0U;

    const esp_err_t st = poom_nfc_store_add_cards(&s_scan_dump.id, 1U, &added, &already, &no_space);
    if(st != ESP_OK)
    {
        char buf[22];
        (void)snprintf(buf, sizeof(buf), "err=%d", (int)st);
        menu_nfc_set_info_return_("Save failed", buf, menu_nfc_scan_primary_state_());
        return;
    }

    menu_nfc_refresh_saved_count_();

    char line0[22];
    char line1[22];
    (void)snprintf(line0, sizeof(line0), "Saved embedded");
    (void)snprintf(line1, sizeof(line1), "Saved:%u/%u", (unsigned)s_saved_total, (unsigned)POOM_NFC_STORE_MAX_CARDS);
    menu_nfc_set_info_return_(line0, line1, menu_nfc_scan_primary_state_());
    (void)already;
    (void)no_space;
}

/**
 * @brief Internal helper for `menu_nfc_do_delete_selected`.
 *
 * @return void
 */
static void menu_nfc_do_delete_selected_(void)
{
    if (s_store_cache.count == 0U)
    {
        menu_nfc_set_info_("No cards", "");
        return;
    }

    if ((s_store_selected < 0) || (s_store_selected >= (int)s_store_cache.count))
    {
        return;
    }

    bool removed = false;
    (void)poom_nfc_store_remove_index((uint8_t)s_store_selected, &removed);

    if (removed)
    {
        menu_nfc_refresh_saved_count_();
        menu_nfc_load_store_cache_();
        menu_nfc_set_info_("Deleted", "");
    }
    else
    {
        menu_nfc_set_info_("Not found", "");
    }
}

/**
 * @brief Clears the internal state used by this menu module.
 *
 * @return void
 */
static void menu_nfc_do_clear_all_(void)
{
    (void)poom_nfc_store_clear();
    menu_nfc_refresh_saved_count_();
    menu_nfc_load_store_cache_();
    menu_nfc_set_info_("Cleared", "All cards");
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_nfc_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_nfc_active = false;
    s_menu_nfc_exit_requested = false;
    s_menu_nfc_scan_requested = false;
    s_menu_nfc_restore_requested = false;

    poom_nfc_emulator_stop();
    poom_nfc_controller_stop();

    if (s_menu_nfc_ui_task != NULL)
    {
        if (s_menu_nfc_ui_task != current_task)
        {
            TaskHandle_t task = s_menu_nfc_ui_task;
            s_menu_nfc_ui_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_nfc_ui_task = NULL;
        }
    }

    if (s_menu_nfc_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_nfc_button_cb_, s_menu_nfc_sbus_user);
        s_menu_nfc_buttons_subscribed = false;
    }

    menu_nfc_scan_meta_release_();

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(menu_nfc_button_msg_t)))
    {
        return;
    }

    menu_nfc_button_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BTN_B)
    {
        if (s_state == MENU_NFC_STATE_MAIN)
        {
            s_menu_nfc_exit_requested = true;
        }
        else if (s_state == MENU_NFC_STATE_EMULATE_RUNNING)
        {
            menu_nfc_emu_stop_();
            s_state = s_emu_running_return_state;
        }
        else if ((s_state == MENU_NFC_STATE_EMULATE_LIST) || (s_state == MENU_NFC_STATE_EMULATE_SD_LIST))
        {
            s_state = MENU_NFC_STATE_EMULATE_SOURCE;
        }
        else if(s_state == MENU_NFC_STATE_RESTORE_MODE)
        {
            s_state = MENU_NFC_STATE_RESTORE_SD_LIST;
        }
        else if(s_state == MENU_NFC_STATE_RESTORE_SD_LIST)
        {
            s_state = MENU_NFC_STATE_MAIN;
        }
        else if (s_state == MENU_NFC_STATE_SCAN_INFO)
        {
            if(menu_nfc_scan_primary_state_() == MENU_NFC_STATE_SCAN_INFO)
            {
                menu_nfc_scan_meta_release_();
                s_state = MENU_NFC_STATE_MAIN;
            }
            else
            {
                s_state = MENU_NFC_STATE_SCAN_RESULT;
            }
        }
        else if (s_state == MENU_NFC_STATE_INFO)
        {
            s_state = s_info_return_state;
        }
        else if ((s_state == MENU_NFC_STATE_SCAN_SCANNING) || (s_state == MENU_NFC_STATE_SCAN_RESULT))
        {
            menu_nfc_scan_meta_release_();
            s_state = MENU_NFC_STATE_MAIN;
        }
        else if (s_state == MENU_NFC_STATE_SCAN_ACTIONS)
        {
            s_state = menu_nfc_scan_primary_state_();
        }
        else if(s_state == MENU_NFC_STATE_EMV_APP_SELECT)
        {
            if(s_scan_meta != NULL)
            {
                free(s_scan_meta->emv_apps);
                s_scan_meta->emv_apps = NULL;
                s_scan_meta->emv_app_count = 0U;
            }
            s_state = MENU_NFC_STATE_SCAN_ACTIONS;
        }
        else if (s_state == MENU_NFC_STATE_EMULATE_SOURCE)
        {
            s_state = MENU_NFC_STATE_MAIN;
        }
        else
        {
            s_state = MENU_NFC_STATE_MAIN;
        }
        menu_nfc_request_redraw_();
        return;
    }

    if (s_state == MENU_NFC_STATE_MAIN)
    {
        if (ev.button == BTN_UP)
        {
            if (s_opt > 0)
            {
                s_opt = (menu_nfc_opt_t)((int)s_opt - 1);
            }
        }
        else if (ev.button == BTN_DOWN)
        {
            if (((int)s_opt + 1) < (int)MENU_NFC_OPT_COUNT)
            {
                s_opt = (menu_nfc_opt_t)((int)s_opt + 1);
            }
        }
        else if (ev.button == BTN_A)
        {
            if (s_opt == MENU_NFC_OPT_SCAN)
            {
                if(!menu_nfc_scan_meta_acquire_())
                {
                    menu_nfc_set_info_return_("No RAM for read", "", MENU_NFC_STATE_MAIN);
                    return;
                }
                s_menu_nfc_scan_requested = true;
            }
            else if (s_opt == MENU_NFC_OPT_EMULATE)
            {
                s_emu_source_sel = MENU_NFC_EMU_SRC_EMBEDDED;
                s_state = MENU_NFC_STATE_EMULATE_SOURCE;
            }
            else if(s_opt == MENU_NFC_OPT_RESTORE)
            {
                menu_nfc_sd_load_items_(MENU_NFC_SD_ITEM_MFC_DUMP);
                s_state = MENU_NFC_STATE_RESTORE_SD_LIST;
            }
            else if (s_opt == MENU_NFC_OPT_STORAGE)
            {
                menu_nfc_load_store_cache_();
                s_state = MENU_NFC_STATE_STORAGE_LIST;
            }
        }
        menu_nfc_request_redraw_();
        return;
    }

    if (s_state == MENU_NFC_STATE_SCAN_RESULT)
    {
        if (ev.button == BTN_A)
        {
            if((s_scan_meta != NULL) && (s_scan_meta->kind == MENU_NFC_SCAN_KIND_EMV))
            {
                menu_nfc_scan_save_to_sd_();
            }
            else
            {
                s_state = MENU_NFC_STATE_SCAN_ACTIONS;
                menu_nfc_request_redraw_();
            }
        }
        else if ((ev.button == BTN_DOWN) && (s_scan_meta != NULL) &&
                 (menu_nfc_scan_total_info_lines_() > MENU_NFC_SCAN_SUMMARY_LINES))
        {
            s_scan_meta->info_scroll = 0U;
            s_state = MENU_NFC_STATE_SCAN_INFO;
            menu_nfc_request_redraw_();
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_SCAN_INFO)
    {
        if (ev.button == BTN_A)
        {
            if((s_scan_meta != NULL) && (s_scan_meta->kind == MENU_NFC_SCAN_KIND_EMV))
            {
                menu_nfc_scan_save_to_sd_();
            }
            else
            {
                s_state = MENU_NFC_STATE_SCAN_ACTIONS;
                menu_nfc_request_redraw_();
            }
        }
        else if (ev.button == BTN_UP)
        {
            if ((s_scan_meta != NULL) && (s_scan_meta->info_scroll > 0U))
            {
                s_scan_meta->info_scroll--;
                menu_nfc_request_redraw_();
            }
        }
        else if (ev.button == BTN_DOWN)
        {
            const uint8_t total = menu_nfc_scan_total_info_lines_();
            if ((s_scan_meta != NULL) &&
                (total > MENU_NFC_SCAN_INFO_VISIBLE_LINES) &&
                ((uint8_t)(s_scan_meta->info_scroll + MENU_NFC_SCAN_INFO_VISIBLE_LINES) < total))
            {
                s_scan_meta->info_scroll++;
                menu_nfc_request_redraw_();
            }
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_SCAN_ACTIONS)
    {
        if (ev.button == BTN_UP)
        {
            if ((s_scan_meta != NULL) && (s_scan_meta->action_sel > 0U))
            {
                s_scan_meta->action_sel--;
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            if((s_scan_meta != NULL) && (s_scan_meta->action_count > 0U) &&
               ((uint8_t)(s_scan_meta->action_sel + 1U) < s_scan_meta->action_count))
            {
                s_scan_meta->action_sel++;
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            if((s_scan_meta != NULL) && (s_scan_meta->action_count > 0U))
            {
                menu_nfc_scan_run_action_(menu_nfc_scan_action_selected_());
            }
        }
        return;
    }

    if(s_state == MENU_NFC_STATE_EMV_APP_SELECT)
    {
        if(ev.button == BTN_UP)
        {
            if(s_scan_meta != NULL && s_scan_meta->emv_app_sel > 0U)
            {
                s_scan_meta->emv_app_sel--;
            }
            menu_nfc_request_redraw_();
        }
        else if(ev.button == BTN_DOWN)
        {
            if(s_scan_meta != NULL &&
               (uint8_t)(s_scan_meta->emv_app_sel + 1U) < s_scan_meta->emv_app_count)
            {
                s_scan_meta->emv_app_sel++;
            }
            menu_nfc_request_redraw_();
        }
        else if(ev.button == BTN_A)
        {
            if(!menu_nfc_prepare_selected_emv_app_())
            {
                menu_nfc_set_info_return_(
                    "Read failed", "Hold card near", MENU_NFC_STATE_EMV_APP_SELECT);
                return;
            }
            s_scan_meta->info_scroll = 0U;
            s_state = menu_nfc_scan_primary_state_();
            menu_nfc_request_redraw_();
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_EMULATE_SOURCE)
    {
        if (ev.button == BTN_UP)
        {
            if (s_emu_source_sel > 0)
            {
                s_emu_source_sel = (menu_nfc_emu_source_t)((int)s_emu_source_sel - 1);
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            if (((int)s_emu_source_sel + 1) < (int)MENU_NFC_EMU_SRC_COUNT)
            {
                s_emu_source_sel = (menu_nfc_emu_source_t)((int)s_emu_source_sel + 1);
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            if (s_emu_source_sel == MENU_NFC_EMU_SRC_EMBEDDED)
            {
                menu_nfc_load_store_cache_();
                if (s_store_cache.count == 0U)
                {
                    menu_nfc_set_info_return_("No embedded cards", "Scan + Save first", MENU_NFC_STATE_EMULATE_SOURCE);
                }
                else
                {
                    s_state = MENU_NFC_STATE_EMULATE_LIST;
                    menu_nfc_request_redraw_();
                }
            }
            else if (s_emu_source_sel == MENU_NFC_EMU_SRC_SD)
            {
                menu_nfc_sd_load_items_(MENU_NFC_SD_ITEM_NFC_DUMP);
                s_state = MENU_NFC_STATE_EMULATE_SD_LIST;
                menu_nfc_request_redraw_();
            }
            else
            {
                menu_nfc_emu_start_t4t_default_(MENU_NFC_STATE_EMULATE_SOURCE);
            }
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_STORAGE_LIST)
    {
        if (ev.button == BTN_UP)
        {
            s_store_selected--;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            s_store_selected++;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_RIGHT)
        {
            s_yes_selected = false;
            s_state = MENU_NFC_STATE_STORAGE_CONFIRM_CLEAR;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            s_yes_selected = true;
            s_state = MENU_NFC_STATE_STORAGE_CONFIRM_DEL;
            menu_nfc_request_redraw_();
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_EMULATE_LIST)
    {
        if (ev.button == BTN_UP)
        {
            s_store_selected--;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            s_store_selected++;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            menu_nfc_emu_start_selected_();
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_EMULATE_SD_LIST)
    {
        if (ev.button == BTN_UP)
        {
            s_sd_dump_selected--;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            s_sd_dump_selected++;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            if ((s_sd_dump_selected >= 0) && (s_sd_dump_selected < s_sd_dump_count))
            {
                const menu_nfc_sd_item_t *item = &s_sd_dump_files[s_sd_dump_selected];
                if(menu_nfc_sd_file_has_page_dump_(item->rel_path))
                {
                    menu_nfc_emu_start_mful_image_(item->rel_path, MENU_NFC_STATE_EMULATE_SD_LIST);
                }
                else
                {
                    poom_nfc_card_id_t id;
                    const esp_err_t err = poom_nfc_dump_load_card_id_from_sd(item->rel_path, &id);
                    if (err != ESP_OK)
                    {
                        char line1[22];
                        (void)snprintf(line1, sizeof(line1), "err=%d", (int)err);
                        menu_nfc_set_info_return_("Load failed", line1, MENU_NFC_STATE_EMULATE_SD_LIST);
                    }
                    else
                    {
                        menu_nfc_emu_start_id_(&id, true, MENU_NFC_STATE_EMULATE_SD_LIST);
                    }
                }
            }
        }
        return;
    }

    if(s_state == MENU_NFC_STATE_RESTORE_SD_LIST)
    {
        if(ev.button == BTN_UP)
        {
            s_sd_dump_selected--;
            menu_nfc_request_redraw_();
        }
        else if(ev.button == BTN_DOWN)
        {
            s_sd_dump_selected++;
            menu_nfc_request_redraw_();
        }
        else if(ev.button == BTN_A && s_sd_dump_selected >= 0 &&
                s_sd_dump_selected < s_sd_dump_count)
        {
            (void)snprintf(s_restore_rel_path, sizeof(s_restore_rel_path), "%s",
                           s_sd_dump_files[s_sd_dump_selected].rel_path);
            s_restore_write_trailers = false;
            s_state = MENU_NFC_STATE_RESTORE_MODE;
            menu_nfc_request_redraw_();
        }
        return;
    }

    if(s_state == MENU_NFC_STATE_RESTORE_MODE)
    {
        if(ev.button == BTN_UP || ev.button == BTN_DOWN ||
           ev.button == BTN_LEFT || ev.button == BTN_RIGHT)
        {
            s_restore_write_trailers = !s_restore_write_trailers;
            menu_nfc_request_redraw_();
        }
        else if(ev.button == BTN_A)
        {
            s_menu_nfc_restore_requested = true;
            menu_nfc_request_redraw_();
        }
        return;
    }

    if ((s_state == MENU_NFC_STATE_STORAGE_CONFIRM_DEL) || (s_state == MENU_NFC_STATE_STORAGE_CONFIRM_CLEAR))
    {
        if ((ev.button == BTN_LEFT) || (ev.button == BTN_RIGHT))
        {
            s_yes_selected = !s_yes_selected;
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            if (s_yes_selected)
            {
                if (s_state == MENU_NFC_STATE_STORAGE_CONFIRM_DEL)
                {
                    menu_nfc_do_delete_selected_();
                }
                else
                {
                    menu_nfc_do_clear_all_();
                }
            }
            else
            {
                s_state = MENU_NFC_STATE_STORAGE_LIST;
                menu_nfc_request_redraw_();
            }
        }
        return;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_nfc_ui_task_(void *arg)
{
    (void)arg;

    menu_nfc_refresh_saved_count_();
    s_menu_nfc_input_dirty = true;

    while (s_menu_nfc_active)
    {
        if (s_menu_nfc_exit_requested)
        {
            menu_nfc_exit_();
            break;
        }

        if ((s_state == MENU_NFC_STATE_INFO) && (xTaskGetTickCount() >= s_info_until_tick))
        {
            s_state = s_info_return_state;
            if(s_state == MENU_NFC_STATE_MAIN)
            {
                menu_nfc_scan_meta_release_();
            }
            menu_nfc_refresh_saved_count_();
            s_menu_nfc_input_dirty = true;
        }

        if (s_menu_nfc_scan_requested)
        {
            s_menu_nfc_scan_requested = false;
            s_state = MENU_NFC_STATE_SCAN_SCANNING;
            s_menu_nfc_input_dirty = true;
            menu_nfc_run_scan_();
        }

        if(s_menu_nfc_restore_requested)
        {
            s_menu_nfc_restore_requested = false;
            menu_nfc_run_restore_();
        }

        if (s_menu_nfc_input_dirty)
        {
            switch (s_state)
            {
                case MENU_NFC_STATE_MAIN:                 menu_nfc_draw_main_(); break;
                case MENU_NFC_STATE_SCAN_SCANNING:        menu_nfc_draw_scanning_(); break;
                case MENU_NFC_STATE_SCAN_RESULT:          menu_nfc_draw_scan_result_(); break;
                case MENU_NFC_STATE_SCAN_INFO:            menu_nfc_draw_scan_info_(); break;
                case MENU_NFC_STATE_SCAN_ACTIONS:         menu_nfc_draw_scan_actions_(); break;
                case MENU_NFC_STATE_EMV_APP_SELECT:       menu_nfc_draw_emv_app_select_(); break;
                case MENU_NFC_STATE_EMULATE_LIST:         menu_nfc_draw_emulate_list_(); break;
                case MENU_NFC_STATE_EMULATE_SOURCE:       menu_nfc_draw_emulate_source_(); break;
                case MENU_NFC_STATE_EMULATE_SD_LIST:      menu_nfc_draw_emulate_sd_list_(); break;
                case MENU_NFC_STATE_EMULATE_RUNNING:      menu_nfc_draw_emulate_running_(); break;
                case MENU_NFC_STATE_RESTORE_SD_LIST:      menu_nfc_draw_emulate_sd_list_(); break;
                case MENU_NFC_STATE_RESTORE_MODE:         menu_nfc_draw_restore_mode_(); break;
                case MENU_NFC_STATE_STORAGE_LIST:         menu_nfc_draw_storage_list_(); break;
                case MENU_NFC_STATE_STORAGE_CONFIRM_DEL:  menu_nfc_draw_confirm_("Delete selected?"); break;
                case MENU_NFC_STATE_STORAGE_CONFIRM_CLEAR: menu_nfc_draw_confirm_("Clear ALL cards?"); break;
                case MENU_NFC_STATE_INFO:                 menu_nfc_draw_info_(); break;

                default:
                    s_state = MENU_NFC_STATE_MAIN;
                    menu_nfc_draw_main_();
                    break;
            }
            s_menu_nfc_input_dirty = false;
        }

        TickType_t wait_ticks = pdMS_TO_TICKS(MENU_NFC_UI_POLL_MS);
        const TickType_t now = xTaskGetTickCount();
        if ((s_state == MENU_NFC_STATE_INFO) && (s_info_until_tick > now))
        {
            const TickType_t until = s_info_until_tick - now;
            if (until < wait_ticks)
            {
                wait_ticks = until;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }

    s_menu_nfc_ui_task = NULL;
    vTaskDelete(NULL);
}

void menu_nfc_show(void)
{
    if (s_menu_nfc_ui_task != NULL)
    {
        return;
    }

    s_menu_nfc_active = true;
    s_menu_nfc_exit_requested = false;
    s_menu_nfc_scan_requested = false;
    s_menu_nfc_input_dirty = true;
    s_scan_dump_valid = false;
    s_scan_ndef_known = false;
    menu_nfc_scan_meta_release_();
    menu_nfc_scan_actions_reset_();
    s_emu_active_id_valid = false;
    s_emu_source_sd = false;
    s_emu_running_return_state = MENU_NFC_STATE_EMULATE_LIST;
    s_info_until_tick = 0;

    s_state = MENU_NFC_STATE_MAIN;
    s_opt = MENU_NFC_OPT_SCAN;

    if (!s_menu_nfc_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_nfc_button_cb_, s_menu_nfc_sbus_user))
        {
            s_menu_nfc_buttons_subscribed = true;
        }
        else
        {
            s_menu_nfc_active = false;
            const uint8_t token = 1U;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    (void)xTaskCreate(menu_nfc_ui_task_, "menu_nfc", MENU_NFC_STACK, NULL, MENU_NFC_PRIO, &s_menu_nfc_ui_task);
}
