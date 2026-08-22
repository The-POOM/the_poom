// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_nfc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

#include "poom_nfc_dump.h"
#include "poom_nfc_card_ident.h"
#include "poom_nfc_controller.h"
#include "poom_nfc_emulator.h"
#include "poom_nfc_store.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_NFC_REFRESH_MS (180U)
#define MENU_NFC_UI_POLL_MS (250U)
#define MENU_NFC_STACK (4096U)
#define MENU_NFC_PRIO (4U)

#define MENU_NFC_SCAN_TIMEOUT_MS (2500U)
#define MENU_NFC_SCAN_MAX_FOUND (12U)
#define MENU_NFC_INFO_HOLD_MS (1800U)

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
    MENU_NFC_OPT_STORAGE,
    MENU_NFC_OPT_COUNT,
} menu_nfc_opt_t;

typedef enum
{
    MENU_NFC_STATE_MAIN = 0,
    MENU_NFC_STATE_SCAN_SCANNING,
    MENU_NFC_STATE_SCAN_RESULT,
    MENU_NFC_STATE_SCAN_ACTIONS,
    MENU_NFC_STATE_EMULATE_LIST,
    MENU_NFC_STATE_EMULATE_SOURCE,
    MENU_NFC_STATE_EMULATE_SD_LIST,
    MENU_NFC_STATE_EMULATE_RUNNING,
    MENU_NFC_STATE_STORAGE_LIST,
    MENU_NFC_STATE_STORAGE_CONFIRM_DEL,
    MENU_NFC_STATE_STORAGE_CONFIRM_CLEAR,
    MENU_NFC_STATE_INFO,
} menu_nfc_state_t;

static const char *const k_opt_labels[MENU_NFC_OPT_COUNT] = {
    "SCAN",
    "EMULATE",
    "STORAGE",
};

static bool s_menu_nfc_active = false;
static bool s_menu_nfc_buttons_subscribed = false;
static bool s_menu_nfc_exit_requested = false;
static bool s_menu_nfc_scan_requested = false;
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
    MENU_NFC_SCAN_ACT_SAVE_SD = 0,
    MENU_NFC_SCAN_ACT_SAVE_EMBEDDED,
    MENU_NFC_SCAN_ACT_SCAN_AGAIN,
    MENU_NFC_SCAN_ACT_BACK,
    MENU_NFC_SCAN_ACT_COUNT,
} menu_nfc_scan_action_t;

static menu_nfc_scan_action_t s_scan_action = MENU_NFC_SCAN_ACT_SAVE_SD;

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

    if(!s_scan_dump_valid || !poom_nfc_card_id_is_valid(&s_scan_dump.id))
    {
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No tag / invalid"));

        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    char uid_compact[22];
    menu_nfc_format_uid_compact_(&s_scan_dump.id, uid_compact, sizeof(uid_compact));

    char line0[22];
    char line1[22];
    char line2[22];
    char line3[22];

    uint16_t user_bytes = 0U;
    uint16_t total_bytes = 0U;
    const bool has_user_bytes = poom_nfc_dump_get_t2t_user_bytes(&s_scan_dump, &user_bytes);
    const bool has_total_bytes = poom_nfc_dump_get_t2t_total_bytes(&s_scan_dump, &total_bytes);

    {
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "Type:%s M:%s", menu_nfc_scan_card_short_(&s_scan_dump), menu_nfc_mfr_abbr_(&s_scan_dump));
        (void)snprintf(line0, sizeof(line0), "%.21s", tmp);
    }
    {
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "UID%u:%s", (unsigned)s_scan_dump.id.uid_len, uid_compact);
        (void)snprintf(line1, sizeof(line1), "%.21s", tmp);
    }

    if(((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U) &&
       ((s_scan_dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U))
    {
        (void)snprintf(line2,
                       sizeof(line2),
                       "ATQA:%02X %02X SAK:%02X",
                       (unsigned)s_scan_dump.id.atqa[0],
                       (unsigned)s_scan_dump.id.atqa[1],
                       (unsigned)s_scan_dump.id.sak);
    }
    else
    {
        (void)snprintf(line2, sizeof(line2), "ATQA/SAK: N/A");
    }

    if(has_user_bytes && has_total_bytes)
    {
        (void)snprintf(line3,
                       sizeof(line3),
                       "R:%s N:%s %u/%u",
                       menu_nfc_scan_mode_str_(&s_scan_dump),
                       s_scan_ndef_known ? (s_scan_has_ndef ? "Y" : "N") : "U",
                       (unsigned)user_bytes,
                       (unsigned)total_bytes);
    }
    else
    {
        (void)snprintf(line3,
                       sizeof(line3),
                       "Read:%s NDEF:%s",
                       menu_nfc_scan_mode_str_(&s_scan_dump),
                       s_scan_ndef_known ? (s_scan_has_ndef ? "YES" : "NO") : "UNK");
    }

    poom_arduboy_set_cursor(4, MENU_NFC_INFO_Y0);
    (void)poom_arduboy_print(line0);
    poom_arduboy_set_cursor(4, (int16_t)(MENU_NFC_INFO_Y0 + MENU_NFC_INFO_STEP));
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, (int16_t)(MENU_NFC_INFO_Y0 + 2 * MENU_NFC_INFO_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(4, (int16_t)(MENU_NFC_INFO_Y0 + 3 * MENU_NFC_INFO_STEP));
    (void)poom_arduboy_print(line3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:MENU"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_nfc_draw_scan_actions_(void)
{
    static const char *const labels[MENU_NFC_SCAN_ACT_COUNT] = {
        "Save to SD",
        "Save embedded",
        "Scan again",
        "Back",
    };

    menu_nfc_draw_frame_("NFC");

    for(int row = 0; row < (int)MENU_NFC_SCAN_ACT_COUNT; row++)
    {
        const int16_t y = (int16_t)(MENU_NFC_INFO_Y0 + (int16_t)row * MENU_NFC_INFO_STEP);
        poom_arduboy_set_cursor(4, y);
        (void)poom_arduboy_print(labels[row]);

        if(row == (int)s_scan_action)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MAIN_ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OK"));
    poom_arduboy_set_cursor(72, 56);
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
    (void)kind_filter;
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

    char dir_path[96];
    (void)snprintf(dir_path, sizeof(dir_path), "%s/nfc_dumps", SD_CARD_PATH);

    DIR *dir = opendir(dir_path);
    if(dir == NULL)
    {
        return;
    }

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL)
    {
        if(ent->d_name[0] == '.')
        {
            continue;
        }

        const char *name = ent->d_name;
        const size_t name_len = strlen(name);
        if(name_len < 4U)
        {
            continue;
        }

        if(!menu_nfc_path_has_ext_(name, ".nfc"))
        {
            continue;
        }

        char rel_path[96];
        static const char prefix[] = "/nfc_dumps/";
        const size_t plen = sizeof(prefix) - 1U;
        if((plen + name_len + 1U) > sizeof(rel_path))
        {
            continue;
        }
        (void)memcpy(rel_path, prefix, plen);
        (void)memcpy(&rel_path[plen], name, name_len);
        rel_path[plen + name_len] = '\0';

        menu_nfc_sd_add_item_(name, rel_path, kind_filter);
    }

    (void)closedir(dir);

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

    if(count <= 0)
    {
        menu_nfc_draw_frame_("NFC");
        poom_arduboy_set_cursor(6, 30);
        (void)poom_arduboy_print(F("No SD dumps"));
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

    menu_nfc_draw_frame_("NFC");

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
        (void)snprintf(line, sizeof(line), "NFC:%.17s", s_sd_dump_files[idx].name);
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
    menu_nfc_draw_frame_("NFC");

    char uid[POOM_NFC_CARD_UID_MAX * 2U + 1U];
    uid[0] = '\0';

    if(s_emu_active_id_valid)
    {
        menu_nfc_format_uid_hex_(&s_emu_active_id, uid, sizeof(uid));
    }

    poom_arduboy_set_cursor(4, MENU_NFC_INFO_Y0);
    (void)poom_arduboy_print(F("Emulating..."));

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
    s_emu_running_return_state = from_sd ? MENU_NFC_STATE_EMULATE_SD_LIST : MENU_NFC_STATE_EMULATE_LIST;
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

/**
 * @brief Internal helper for `menu_nfc_run_scan`.
 *
 * @return void
 */
static void menu_nfc_run_scan_(void)
{
    menu_nfc_draw_scanning_();

    (void)memset(&s_scan_dump, 0, sizeof(s_scan_dump));
    s_scan_dump_valid = false;
    s_scan_has_ndef = false;
    s_scan_ndef_known = false;

    const bool ok = poom_nfc_controller_capture_dump(MENU_NFC_SCAN_TIMEOUT_MS, &s_scan_dump);
    poom_nfc_controller_stop();

    if(!ok)
    {
        menu_nfc_set_info_return_("Scan failed", "Try again", MENU_NFC_STATE_MAIN);
        return;
    }

    s_scan_dump_valid = true;
    s_scan_ndef_known = (s_scan_dump.read_mode == POOM_NFC_DUMP_READ_FULL) && s_scan_dump.read_ok;
    s_scan_has_ndef = s_scan_ndef_known ? menu_nfc_dump_has_ndef_(&s_scan_dump) : false;
    s_scan_action = MENU_NFC_SCAN_ACT_SAVE_SD;
    s_state = MENU_NFC_STATE_SCAN_RESULT;
    menu_nfc_request_redraw_();
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_nfc_scan_save_to_sd_(void)
{
    if(!s_scan_dump_valid)
    {
        menu_nfc_set_info_return_("No scan data", "", MENU_NFC_STATE_SCAN_RESULT);
        return;
    }

    char rel_path[64];
    rel_path[0] = '\0';

    menu_nfc_draw_frame_("NFC");
    poom_arduboy_set_cursor(10, 30);
    (void)poom_arduboy_print(F("Saving to SD..."));
    poom_arduboy_display();

    const esp_err_t err = poom_nfc_dump_save_to_sd(&s_scan_dump, rel_path, sizeof(rel_path));
    poom_nfc_controller_stop();

    if(err == ESP_OK)
    {
        char line0[22];
        char line1[22];
        (void)snprintf(line0, sizeof(line0), "Saved to SD");
        (void)snprintf(line1, sizeof(line1), "%.21s", (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/");
        menu_nfc_set_info_return_(line0, line1, MENU_NFC_STATE_SCAN_RESULT);
        return;
    }

    {
        char line0[22];
        char line1[22];
        (void)snprintf(line0, sizeof(line0), "SD save failed");
        (void)snprintf(line1, sizeof(line1), "err=%d", (int)err);
        menu_nfc_set_info_return_(line0, line1, MENU_NFC_STATE_SCAN_RESULT);
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
        menu_nfc_set_info_return_("No tag data", "", MENU_NFC_STATE_SCAN_RESULT);
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
        menu_nfc_set_info_return_("Save failed", buf, MENU_NFC_STATE_SCAN_RESULT);
        return;
    }

    menu_nfc_refresh_saved_count_();

    char line0[22];
    char line1[22];
    (void)snprintf(line0, sizeof(line0), "Saved embedded");
    (void)snprintf(line1, sizeof(line1), "Saved:%u/%u", (unsigned)s_saved_total, (unsigned)POOM_NFC_STORE_MAX_CARDS);
    menu_nfc_set_info_return_(line0, line1, MENU_NFC_STATE_SCAN_RESULT);
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
        else if (s_state == MENU_NFC_STATE_SCAN_ACTIONS)
        {
            s_state = MENU_NFC_STATE_SCAN_RESULT;
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
                s_menu_nfc_scan_requested = true;
            }
            else if (s_opt == MENU_NFC_OPT_EMULATE)
            {
                s_emu_source_sel = MENU_NFC_EMU_SRC_EMBEDDED;
                s_state = MENU_NFC_STATE_EMULATE_SOURCE;
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
            s_scan_action = MENU_NFC_SCAN_ACT_SAVE_SD;
            s_state = MENU_NFC_STATE_SCAN_ACTIONS;
            menu_nfc_request_redraw_();
        }
        return;
    }

    if (s_state == MENU_NFC_STATE_SCAN_ACTIONS)
    {
        if (ev.button == BTN_UP)
        {
            if (s_scan_action > 0)
            {
                s_scan_action = (menu_nfc_scan_action_t)((int)s_scan_action - 1);
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_DOWN)
        {
            if (((int)s_scan_action + 1) < (int)MENU_NFC_SCAN_ACT_COUNT)
            {
                s_scan_action = (menu_nfc_scan_action_t)((int)s_scan_action + 1);
            }
            menu_nfc_request_redraw_();
        }
        else if (ev.button == BTN_A)
        {
            if (s_scan_action == MENU_NFC_SCAN_ACT_SAVE_SD)
            {
                menu_nfc_scan_save_to_sd_();
            }
            else if (s_scan_action == MENU_NFC_SCAN_ACT_SAVE_EMBEDDED)
            {
                menu_nfc_scan_save_embedded_();
            }
            else if (s_scan_action == MENU_NFC_SCAN_ACT_SCAN_AGAIN)
            {
                s_menu_nfc_scan_requested = true;
                s_state = MENU_NFC_STATE_SCAN_SCANNING;
                menu_nfc_request_redraw_();
            }
            else
            {
                s_state = MENU_NFC_STATE_SCAN_RESULT;
                menu_nfc_request_redraw_();
            }
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

        if (s_menu_nfc_input_dirty)
        {
            switch (s_state)
            {
                case MENU_NFC_STATE_MAIN:                 menu_nfc_draw_main_(); break;
                case MENU_NFC_STATE_SCAN_SCANNING:        menu_nfc_draw_scanning_(); break;
                case MENU_NFC_STATE_SCAN_RESULT:          menu_nfc_draw_scan_result_(); break;
                case MENU_NFC_STATE_SCAN_ACTIONS:         menu_nfc_draw_scan_actions_(); break;
                case MENU_NFC_STATE_EMULATE_LIST:         menu_nfc_draw_emulate_list_(); break;
                case MENU_NFC_STATE_EMULATE_SOURCE:       menu_nfc_draw_emulate_source_(); break;
                case MENU_NFC_STATE_EMULATE_SD_LIST:      menu_nfc_draw_emulate_sd_list_(); break;
                case MENU_NFC_STATE_EMULATE_RUNNING:      menu_nfc_draw_emulate_running_(); break;
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
