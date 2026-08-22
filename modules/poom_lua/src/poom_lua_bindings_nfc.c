// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"

#include "poom_nfc_controller.h"
#include "poom_nfc_dump.h"
#include "poom_nfc_emulator.h"
#include "poom_nfc_iso14443_4.h"
#include "poom_nfc_profile_store.h"
#include "rfal_nfc.h"

#include "lua.h"
#include "lauxlib.h"

// ============================================================
// Helpers
// ============================================================

/**
 * @brief Internal helper for `poom_lua_timeout_ms_arg`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] idx Parameter passed to the function.
 * @param[in] default_ms Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t poom_lua_timeout_ms_arg_(lua_State* L, int idx, uint32_t default_ms)
{
    lua_Integer v = luaL_optinteger(L, idx, (lua_Integer)default_ms);
    if(v < 0)
    {
        v = 0;
    }
    if(v > 60000)
    {
        v = 60000;
    }
    return (uint32_t)v;
}

/**
 * @brief Internal helper for `poom_lua_hex_nibble`.
 *
 * @param[in] c Parameter passed to the function.
 * @param[in] ok Parameter passed to the function.
 * @return char
 */
static char poom_lua_hex_nibble_(char c, bool* ok)
{
    if(ok == NULL)
    {
        return 0;
    }
    if((c >= '0') && (c <= '9'))
    {
        *ok = true;
        return (char)(c - '0');
    }
    if((c >= 'a') && (c <= 'f'))
    {
        *ok = true;
        return (char)(10 + (c - 'a'));
    }
    if((c >= 'A') && (c <= 'F'))
    {
        *ok = true;
        return (char)(10 + (c - 'A'));
    }
    *ok = false;
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_hex_to_bytes`.
 *
 * @param[in] s Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_lua_hex_to_bytes_(const char* s, uint8_t* out, size_t out_max, size_t* out_len)
{
    size_t out_i = 0U;
    bool have_hi = false;
    uint8_t hi = 0U;

    if(out_len != NULL)
    {
        *out_len = 0U;
    }

    if((s == NULL) || (out == NULL) || (out_max == 0U))
    {
        return false;
    }

    for(size_t i = 0U; s[i] != '\0'; i++)
    {
        const char c = s[i];
        if((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n') || (c == ':') || (c == '-'))
        {
            continue;
        }

        bool ok = false;
        const char n = poom_lua_hex_nibble_(c, &ok);
        if(!ok)
        {
            return false;
        }

        if(!have_hi)
        {
            hi = (uint8_t)n;
            have_hi = true;
            continue;
        }

        if(out_i >= out_max)
        {
            return false;
        }

        out[out_i++] = (uint8_t)((hi << 4) | (uint8_t)n);
        have_hi = false;
        hi = 0U;
    }

    if(have_hi)
    {
        return false;
    }

    if(out_len != NULL)
    {
        *out_len = out_i;
    }
    return true;
}

/**
 * @brief Internal helper for `poom_lua_bytes_to_hex`.
 *
 * @param[in] out Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @param[in] bytes Parameter passed to the function.
 * @param[in] bytes_len Parameter passed to the function.
 * @return void
 */
static void poom_lua_bytes_to_hex_(char* out, size_t out_len, const uint8_t* bytes, size_t bytes_len)
{
    static const char k_hex[] = "0123456789ABCDEF";

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';

    if((bytes == NULL) || (bytes_len == 0U))
    {
        return;
    }

    if(out_len < (bytes_len * 2U + 1U))
    {
        return;
    }

    for(size_t i = 0U; i < bytes_len; i++)
    {
        out[i * 2U + 0U] = k_hex[(bytes[i] >> 4) & 0x0F];
        out[i * 2U + 1U] = k_hex[bytes[i] & 0x0F];
    }
    out[bytes_len * 2U] = '\0';
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] t Parameter passed to the function.
 * @return const char*
 */
static const char* poom_lua_rfal_listen_type_to_str_(uint8_t t)
{
    switch((uint32_t)t)
    {
        case RFAL_NFC_LISTEN_TYPE_NFCA: return "NFC-A";
        case RFAL_NFC_LISTEN_TYPE_NFCB: return "NFC-B";
        case RFAL_NFC_LISTEN_TYPE_NFCF: return "NFC-F";
        case RFAL_NFC_LISTEN_TYPE_NFCV: return "NFC-V";
        case RFAL_NFC_LISTEN_TYPE_ST25TB: return "ST25TB";
        case RFAL_NFC_POLL_TYPE_NFCA: return "NFC-A";
        case RFAL_NFC_POLL_TYPE_NFCB: return "NFC-B";
        case RFAL_NFC_POLL_TYPE_NFCF: return "NFC-F";
        case RFAL_NFC_POLL_TYPE_NFCV: return "NFC-V";
        default: return "NFC";
    }
}

/**
 * @brief Internal helper for `poom_lua_push_card_id`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] id Parameter passed to the function.
 * @return void
 */
static void poom_lua_push_card_id_(lua_State* L, const poom_nfc_card_id_t* id)
{
    char hex[POOM_NFC_CARD_UID_MAX * 2U + 1U];

    if(L == NULL)
    {
        return;
    }

    lua_newtable(L);
    if(id == NULL)
    {
        return;
    }

    poom_lua_bytes_to_hex_(hex, sizeof(hex), id->uid, id->uid_len);
    lua_pushstring(L, hex);
    lua_setfield(L, -2, "uid_hex");

    lua_pushinteger(L, (lua_Integer)id->uid_len);
    lua_setfield(L, -2, "uid_len");

    lua_pushinteger(L, (lua_Integer)id->type);
    lua_setfield(L, -2, "type");

    lua_pushstring(L, poom_lua_rfal_listen_type_to_str_(id->type));
    lua_setfield(L, -2, "type_str");

    if((id->flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U)
    {
        char atqa_hex[2U * 2U + 1U];
        poom_lua_bytes_to_hex_(atqa_hex, sizeof(atqa_hex), id->atqa, sizeof(id->atqa));
        lua_pushstring(L, atqa_hex);
        lua_setfield(L, -2, "atqa_hex");
    }

    if((id->flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
    {
        lua_pushinteger(L, (lua_Integer)id->sak);
        lua_setfield(L, -2, "sak");
    }
}


// ============================================================
// NFC
// ============================================================

/**
 * @brief Internal helper for `poom_lua_nfc_ensure_started`.
 *
 * @return bool
 */
static bool poom_lua_nfc_ensure_started_(void)
{
    return poom_nfc_controller_start();
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] s Parameter passed to the function.
 * @param[in] ok Parameter passed to the function.
 * @return poom_nfc_ctrl_tech_t
 */
static poom_nfc_ctrl_tech_t poom_lua_nfc_parse_tech_(const char* s, bool* ok)
{
    if(ok != NULL)
    {
        *ok = false;
    }
    if(s == NULL)
    {
        return POOM_NFC_CTRL_TECH_ALL;
    }

    if((strcmp(s, "all") == 0) || (strcmp(s, "ALL") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_ALL;
    }
    if((strcmp(s, "a") == 0) || (strcmp(s, "A") == 0) || (strcmp(s, "nfc-a") == 0) || (strcmp(s, "NFC-A") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_A;
    }
    if((strcmp(s, "b") == 0) || (strcmp(s, "B") == 0) || (strcmp(s, "nfc-b") == 0) || (strcmp(s, "NFC-B") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_B;
    }
    if((strcmp(s, "f") == 0) || (strcmp(s, "F") == 0) || (strcmp(s, "nfc-f") == 0) || (strcmp(s, "NFC-F") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_F;
    }
    if((strcmp(s, "v") == 0) || (strcmp(s, "V") == 0) || (strcmp(s, "nfc-v") == 0) || (strcmp(s, "NFC-V") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_V;
    }
    if((strcmp(s, "st25tb") == 0) || (strcmp(s, "ST25TB") == 0))
    {
        if(ok) *ok = true;
        return POOM_NFC_CTRL_TECH_ST25TB;
    }

    return POOM_NFC_CTRL_TECH_ALL;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] mode Parameter passed to the function.
 * @return const char*
 */
static const char* poom_lua_nfc_emul_mode_to_str_(poom_nfc_emu_mode_t mode)
{
    switch(mode)
    {
        case POOM_NFC_EMU_MODE_3A: return "3a";
        case POOM_NFC_EMU_MODE_T4T: return "t4t";
        case POOM_NFC_EMU_MODE_MFUL: return "mful";
        default: return "3a";
    }
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] s Parameter passed to the function.
 * @param[in] out_mode Parameter passed to the function.
 * @return bool
 */
static bool poom_lua_nfc_parse_emul_mode_(const char* s, poom_nfc_emu_mode_t* out_mode)
{
    if(out_mode == NULL)
    {
        return false;
    }

    if(s == NULL)
    {
        return false;
    }

    if((strcmp(s, "3a") == 0) || (strcmp(s, "3A") == 0))
    {
        *out_mode = POOM_NFC_EMU_MODE_3A;
        return true;
    }
    if((strcmp(s, "t4t") == 0) || (strcmp(s, "T4T") == 0))
    {
        *out_mode = POOM_NFC_EMU_MODE_T4T;
        return true;
    }
    if((strcmp(s, "mful") == 0) || (strcmp(s, "MFUL") == 0))
    {
        *out_mode = POOM_NFC_EMU_MODE_MFUL;
        return true;
    }

    return false;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_start_(lua_State* L)
{
    (void)L;
    lua_pushboolean(L, poom_nfc_controller_start() ? 1 : 0);
    return 1;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_stop_(lua_State* L)
{
    (void)L;
    poom_nfc_emulator_stop();
    poom_nfc_controller_stop();
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_set_tech`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_set_tech_(lua_State* L)
{
    const char* tech_s = luaL_checkstring(L, 1);
    bool ok = false;
    poom_nfc_ctrl_tech_t tech = poom_lua_nfc_parse_tech_(tech_s, &ok);
    if(!ok)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    poom_nfc_controller_set_technology(tech);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_get_tech`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_get_tech_(lua_State* L)
{
    poom_nfc_ctrl_tech_t tech = poom_nfc_controller_get_technology();
    lua_pushstring(L, poom_nfc_controller_technology_to_str(tech));
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_scan`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_scan_(lua_State* L)
{
    const uint32_t timeout_ms = poom_lua_timeout_ms_arg_(L, 1, 500U);
    poom_nfc_card_id_t cards[16];
    size_t count = 0U;

    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    const bool ok = poom_nfc_controller_scan_found_cards(timeout_ms, cards, (size_t)(sizeof(cards) / sizeof(cards[0])), &count);
    if(!ok)
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    lua_pushboolean(L, 1);
    lua_createtable(L, (int)count, 0);
    for(size_t i = 0U; i < count; i++)
    {
        poom_lua_push_card_id_(L, &cards[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_push_dump`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] dump Parameter passed to the function.
 * @return void
 */
static void poom_lua_push_dump_(lua_State* L, const poom_nfc_dump_t* dump)
{
    if(L == NULL)
    {
        return;
    }

    lua_newtable(L);
    if(dump == NULL)
    {
        return;
    }

    lua_pushboolean(L, dump->read_ok ? 1 : 0);
    lua_setfield(L, -2, "read_ok");

    lua_pushstring(L, (dump->read_mode == POOM_NFC_DUMP_READ_FULL) ? "full" : "id");
    lua_setfield(L, -2, "read_mode");

    poom_lua_push_card_id_(L, &dump->id);
    lua_setfield(L, -2, "id");

    if(dump->has_version_bytes)
    {
        char vhex[8U * 2U + 1U];
        poom_lua_bytes_to_hex_(vhex, sizeof(vhex), dump->version_bytes, sizeof(dump->version_bytes));
        lua_pushstring(L, vhex);
        lua_setfield(L, -2, "version_hex");
    }

    if(dump->has_signature)
    {
        char sighex[32U * 2U + 1U];
        poom_lua_bytes_to_hex_(sighex, sizeof(sighex), dump->signature, sizeof(dump->signature));
        lua_pushstring(L, sighex);
        lua_setfield(L, -2, "signature_hex");
    }

    lua_pushinteger(L, (lua_Integer)dump->pages_total);
    lua_setfield(L, -2, "pages_total");
    lua_pushinteger(L, (lua_Integer)dump->pages_read);
    lua_setfield(L, -2, "pages_read");
    lua_pushinteger(L, (lua_Integer)dump->page_size);
    lua_setfield(L, -2, "page_size");

    lua_pushinteger(L, (lua_Integer)dump->user_mem_start_page);
    lua_setfield(L, -2, "user_mem_start_page");
    lua_pushinteger(L, (lua_Integer)dump->user_mem_end_page);
    lua_setfield(L, -2, "user_mem_end_page");
    lua_pushinteger(L, (lua_Integer)dump->lock_bytes_page);
    lua_setfield(L, -2, "lock_bytes_page");
    lua_pushinteger(L, (lua_Integer)dump->dynamic_lock_bytes_page);
    lua_setfield(L, -2, "dynamic_lock_bytes_page");
    lua_pushinteger(L, (lua_Integer)dump->config_start_page);
    lua_setfield(L, -2, "config_start_page");

    if((dump->read_mode == POOM_NFC_DUMP_READ_FULL) && (dump->pages_read > 0U) && (dump->pages_read <= POOM_NFC_DUMP_MAX_PAGES))
    {
        lua_createtable(L, (int)dump->pages_read, 0);
        for(uint16_t i = 0U; i < dump->pages_read; i++)
        {
            char page_hex[POOM_NFC_DUMP_PAGE_SIZE * 2U + 1U];
            poom_lua_bytes_to_hex_(page_hex, sizeof(page_hex), dump->pages[i], POOM_NFC_DUMP_PAGE_SIZE);
            lua_pushstring(L, page_hex);
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        lua_setfield(L, -2, "pages");

        const poom_nfc_t2t_product_t p = poom_nfc_dump_guess_t2t_product(dump);
        lua_pushstring(L, poom_nfc_t2t_product_to_str(p));
        lua_setfield(L, -2, "t2t_product");
    }
}

/**
 * @brief Internal helper for `poom_lua_nfc_capture_dump`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_capture_dump_(lua_State* L)
{
    const uint32_t timeout_ms = poom_lua_timeout_ms_arg_(L, 1, 800U);
    poom_nfc_dump_t dump;
    (void)memset(&dump, 0, sizeof(dump));

    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    const bool ok = poom_nfc_controller_capture_dump(timeout_ms, &dump);
    if(!ok)
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    lua_pushboolean(L, 1);
    poom_lua_push_dump_(L, &dump);
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_nfc_dump_to_sd`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_dump_to_sd_(lua_State* L)
{
    const uint32_t timeout_ms = poom_lua_timeout_ms_arg_(L, 1, 800U);
    char rel_path[128];
    (void)memset(rel_path, 0, sizeof(rel_path));

    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)ESP_FAIL);
        return 3;
    }

    const esp_err_t err = poom_nfc_controller_dump_to_sd(timeout_ms, rel_path, sizeof(rel_path));
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushstring(L, (err == ESP_OK) ? rel_path : "");
    lua_pushinteger(L, (lua_Integer)err);
    return 3;
}

/**
 * @brief Internal helper for `poom_lua_nfc_mful_to_sd`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_mful_to_sd_(lua_State* L)
{
    const uint32_t timeout_ms = poom_lua_timeout_ms_arg_(L, 1, 800U);
    char rel_path[128];
    (void)memset(rel_path, 0, sizeof(rel_path));

    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)ESP_FAIL);
        return 3;
    }

    const esp_err_t err = poom_nfc_controller_mful_bin_to_sd(timeout_ms, rel_path, sizeof(rel_path));
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushstring(L, (err == ESP_OK) ? rel_path : "");
    lua_pushinteger(L, (lua_Integer)err);
    return 3;
}

/**
 * @brief Internal helper for `poom_lua_nfc_connect`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_connect_(lua_State* L)
{
    (void)L;
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_controller_connect() ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_get_last_rapdu`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_get_last_rapdu_(lua_State* L)
{
    uint8_t rapdu[260];
    size_t rapdu_len = 0U;
    char hex[260U * 2U + 1U];

    (void)memset(rapdu, 0, sizeof(rapdu));
    (void)memset(hex, 0, sizeof(hex));

    if(!poom_reader_get_last_rapdu(rapdu, sizeof(rapdu), &rapdu_len))
    {
        return 0;
    }

    poom_lua_bytes_to_hex_(hex, sizeof(hex), rapdu, rapdu_len);
    lua_pushstring(L, hex);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_send`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_send_(lua_State* L)
{
    const char* hex_ascii = luaL_checkstring(L, 1);

    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    const bool ok = poom_nfc_controller_send_raw_hex(hex_ascii);
    lua_pushboolean(L, ok ? 1 : 0);
    if(!ok)
    {
        lua_pushnil(L);
        return 2;
    }

    if(poom_lua_nfc_get_last_rapdu_(L) == 0)
    {
        lua_pushnil(L);
    }
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_push_profile`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] p Parameter passed to the function.
 * @return void
 */
static void poom_lua_push_profile_(lua_State* L, const poom_nfc_profile_t* p)
{
    if(L == NULL)
    {
        return;
    }

    lua_newtable(L);
    if(p == NULL)
    {
        return;
    }

    lua_pushstring(L, poom_lua_nfc_emul_mode_to_str_(p->mode));
    lua_setfield(L, -2, "mode");

    {
        char uid_hex[sizeof(p->uid) * 2U + 1U];
        poom_lua_bytes_to_hex_(uid_hex, sizeof(uid_hex), p->uid, p->uid_len);
        lua_pushstring(L, uid_hex);
        lua_setfield(L, -2, "uid_hex");
        lua_pushinteger(L, (lua_Integer)p->uid_len);
        lua_setfield(L, -2, "uid_len");
    }

    if(p->atqa_set)
    {
        char atqa_hex[2U * 2U + 1U];
        poom_lua_bytes_to_hex_(atqa_hex, sizeof(atqa_hex), p->atqa, sizeof(p->atqa));
        lua_pushstring(L, atqa_hex);
        lua_setfield(L, -2, "atqa_hex");
    }

    if(p->sak_set)
    {
        lua_pushinteger(L, (lua_Integer)p->sak);
        lua_setfield(L, -2, "sak");
    }

    if(p->ats_len > 0U)
    {
        char ats_hex[sizeof(p->ats) * 2U + 1U];
        poom_lua_bytes_to_hex_(ats_hex, sizeof(ats_hex), p->ats, p->ats_len);
        lua_pushstring(L, ats_hex);
        lua_setfield(L, -2, "ats_hex");
        lua_pushinteger(L, (lua_Integer)p->ats_len);
        lua_setfield(L, -2, "ats_len");
    }

    if(p->name_set)
    {
        lua_pushstring(L, p->name);
        lua_setfield(L, -2, "name");
    }
}

/**
 * @brief Internal helper for `poom_lua_nfc_get_last_profile`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_get_last_profile_(lua_State* L)
{
    poom_nfc_profile_t p;
    (void)memset(&p, 0, sizeof(p));

    if(!poom_reader_get_last_profile(&p))
    {
        return 0;
    }

    poom_lua_push_profile_(L, &p);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_profiles_list`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_profiles_list_(lua_State* L)
{
    poom_nfc_profile_store_t store;
    (void)memset(&store, 0, sizeof(store));

    const esp_err_t err = poom_nfc_profile_store_load(&store);
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_createtable(L, (int)store.count, 0);
    for(uint8_t i = 0U; i < store.count; i++)
    {
        poom_lua_push_profile_(L, &store.profiles[i]);
        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "index");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_pushinteger(L, (lua_Integer)err);
    return 3;
}

/**
 * @brief Internal helper for `poom_lua_nfc_profile_from_table`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] idx Parameter passed to the function.
 * @param[in] out_profile Parameter passed to the function.
 * @return bool
 */
static bool poom_lua_nfc_profile_from_table_(lua_State* L, int idx, poom_nfc_profile_t* out_profile)
{
    if((L == NULL) || (out_profile == NULL))
    {
        return false;
    }

    if(!lua_istable(L, idx))
    {
        return false;
    }

    poom_nfc_profile_clear(out_profile);

    lua_getfield(L, idx, "mode");
    const char* mode_s = lua_tostring(L, -1);
    poom_nfc_emu_mode_t mode;
    if(!poom_lua_nfc_parse_emul_mode_(mode_s, &mode))
    {
        lua_pop(L, 1);
        return false;
    }
    out_profile->mode = mode;
    lua_pop(L, 1);

    lua_getfield(L, idx, "uid_hex");
    const char* uid_s = lua_tostring(L, -1);
    size_t uid_len = 0U;
    if(!poom_lua_hex_to_bytes_(uid_s, out_profile->uid, sizeof(out_profile->uid), &uid_len))
    {
        lua_pop(L, 1);
        return false;
    }
    out_profile->uid_len = (uint8_t)uid_len;
    lua_pop(L, 1);

    lua_getfield(L, idx, "atqa_hex");
    if(!lua_isnil(L, -1))
    {
        const char* atqa_s = lua_tostring(L, -1);
        size_t atqa_len = 0U;
        if(!poom_lua_hex_to_bytes_(atqa_s, out_profile->atqa, sizeof(out_profile->atqa), &atqa_len) || (atqa_len != sizeof(out_profile->atqa)))
        {
            lua_pop(L, 1);
            return false;
        }
        out_profile->atqa_set = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "sak");
    if(!lua_isnil(L, -1))
    {
        lua_Integer sak = lua_tointeger(L, -1);
        if((sak < 0) || (sak > 255))
        {
            lua_pop(L, 1);
            return false;
        }
        out_profile->sak = (uint8_t)sak;
        out_profile->sak_set = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "ats_hex");
    if(!lua_isnil(L, -1))
    {
        const char* ats_s = lua_tostring(L, -1);
        size_t ats_len = 0U;
        if(!poom_lua_hex_to_bytes_(ats_s, out_profile->ats, sizeof(out_profile->ats), &ats_len))
        {
            lua_pop(L, 1);
            return false;
        }
        out_profile->ats_len = (uint8_t)ats_len;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "name");
    if(!lua_isnil(L, -1))
    {
        const char* name_s = lua_tostring(L, -1);
        if((name_s != NULL) && (name_s[0] != '\0'))
        {
            (void)strncpy(out_profile->name, name_s, sizeof(out_profile->name) - 1U);
            out_profile->name[sizeof(out_profile->name) - 1U] = '\0';
            out_profile->name_set = true;
        }
    }
    lua_pop(L, 1);

    return (out_profile->uid_len > 0U);
}

/**
 * @brief Internal helper for `poom_lua_nfc_profiles_add`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_profiles_add_(lua_State* L)
{
    poom_nfc_profile_t p;
    size_t added = 0U;
    size_t updated = 0U;
    size_t no_space = 0U;

    if(!poom_lua_nfc_profile_from_table_(L, 1, &p))
    {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, (lua_Integer)ESP_ERR_INVALID_ARG);
        return 2;
    }

    const esp_err_t err = poom_nfc_profile_store_add(&p, &added, &updated, &no_space);
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)added);
    lua_pushinteger(L, (lua_Integer)updated);
    lua_pushinteger(L, (lua_Integer)no_space);
    lua_pushinteger(L, (lua_Integer)err);
    return 5;
}

/**
 * @brief Internal helper for `poom_lua_nfc_profiles_add_last`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_profiles_add_last_(lua_State* L)
{
    const char* name_s = luaL_optstring(L, 1, NULL);
    poom_nfc_profile_t p;
    size_t added = 0U;
    size_t updated = 0U;
    size_t no_space = 0U;

    (void)memset(&p, 0, sizeof(p));
    if(!poom_reader_get_last_profile(&p))
    {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, (lua_Integer)ESP_ERR_INVALID_STATE);
        return 2;
    }

    if((name_s != NULL) && (name_s[0] != '\0'))
    {
        (void)strncpy(p.name, name_s, sizeof(p.name) - 1U);
        p.name[sizeof(p.name) - 1U] = '\0';
        p.name_set = true;
    }

    const esp_err_t err = poom_nfc_profile_store_add(&p, &added, &updated, &no_space);
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)added);
    lua_pushinteger(L, (lua_Integer)updated);
    lua_pushinteger(L, (lua_Integer)no_space);
    lua_pushinteger(L, (lua_Integer)err);
    return 5;
}

/**
 * @brief Internal helper for `poom_lua_nfc_profiles_remove`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_profiles_remove_(lua_State* L)
{
    lua_Integer idx = luaL_checkinteger(L, 1);
    if((idx < 0) || (idx > 255))
    {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, (lua_Integer)ESP_ERR_INVALID_ARG);
        return 2;
    }

    bool removed = false;
    const esp_err_t err = poom_nfc_profile_store_remove_index((uint8_t)idx, &removed);
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushboolean(L, removed ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)err);
    return 3;
}

/**
 * @brief Clears the internal state used by this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_profiles_clear_(lua_State* L)
{
    (void)L;
    const esp_err_t err = poom_nfc_profile_store_clear();
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)err);
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_push_tune_result`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] r Parameter passed to the function.
 * @return void
 */
static void poom_lua_push_tune_result_(lua_State* L, const poom_nfc_tuning_result_t* r)
{
    if(L == NULL)
    {
        return;
    }

    lua_newtable(L);
    if(r == NULL)
    {
        return;
    }

    lua_pushinteger(L, (lua_Integer)r->aat_a);
    lua_setfield(L, -2, "aat_a");
    lua_pushinteger(L, (lua_Integer)r->aat_b);
    lua_setfield(L, -2, "aat_b");
    lua_pushinteger(L, (lua_Integer)r->phase_raw);
    lua_setfield(L, -2, "phase_raw");
    lua_pushinteger(L, (lua_Integer)r->amplitude_raw);
    lua_setfield(L, -2, "amplitude_raw");
    lua_pushinteger(L, (lua_Integer)r->phase_degree);
    lua_setfield(L, -2, "phase_degree");
    lua_pushinteger(L, (lua_Integer)r->amplitude_mvpp);
    lua_setfield(L, -2, "amplitude_mvpp");
    lua_pushinteger(L, (lua_Integer)r->measure_count);
    lua_setfield(L, -2, "measure_count");
}

/**
 * @brief Internal helper for `poom_lua_nfc_tune_auto`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_tune_auto_(lua_State* L)
{
    poom_nfc_tuning_result_t r;
    (void)memset(&r, 0, sizeof(r));

    const bool ok = poom_nfc_controller_tune_auto(&r);
    lua_pushboolean(L, ok ? 1 : 0);
    if(!ok)
    {
        lua_pushnil(L);
        return 2;
    }
    poom_lua_push_tune_result_(L, &r);
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_nfc_tune_get`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_tune_get_(lua_State* L)
{
    poom_nfc_tuning_result_t r;
    (void)memset(&r, 0, sizeof(r));

    const bool ok = poom_nfc_controller_tune_get(&r);
    lua_pushboolean(L, ok ? 1 : 0);
    if(!ok)
    {
        lua_pushnil(L);
        return 2;
    }
    poom_lua_push_tune_result_(L, &r);
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_nfc_tune_set`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_tune_set_(lua_State* L)
{
    lua_Integer a = luaL_checkinteger(L, 1);
    lua_Integer b = luaL_checkinteger(L, 2);
    if((a < 0) || (a > 255) || (b < 0) || (b > 255))
    {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        return 2;
    }

    poom_nfc_tuning_result_t r;
    (void)memset(&r, 0, sizeof(r));
    const bool ok = poom_nfc_controller_tune_set((uint8_t)a, (uint8_t)b, &r);
    lua_pushboolean(L, ok ? 1 : 0);
    if(!ok)
    {
        lua_pushnil(L);
        return 2;
    }
    poom_lua_push_tune_result_(L, &r);
    return 2;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_reset`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_reset_(lua_State* L)
{
    (void)L;
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(poom_nfc_emulator_is_running())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    poom_nfc_emulator_reset_config();
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_is_running`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_is_running_(lua_State* L)
{
    (void)L;
    lua_pushboolean(L, poom_nfc_emulator_is_running() ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_get_config`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_get_config_(lua_State* L)
{
    poom_nfc_emu_cfg_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    poom_nfc_emulator_get_config(&cfg);

    lua_newtable(L);
    lua_pushstring(L, poom_lua_nfc_emul_mode_to_str_(cfg.mode));
    lua_setfield(L, -2, "mode");

    {
        char uid_hex[sizeof(cfg.uid) * 2U + 1U];
        poom_lua_bytes_to_hex_(uid_hex, sizeof(uid_hex), cfg.uid, cfg.uid_len);
        lua_pushstring(L, uid_hex);
        lua_setfield(L, -2, "uid_hex");
        lua_pushinteger(L, (lua_Integer)cfg.uid_len);
        lua_setfield(L, -2, "uid_len");
    }

    if(cfg.atqa_set)
    {
        char atqa_hex[2U * 2U + 1U];
        poom_lua_bytes_to_hex_(atqa_hex, sizeof(atqa_hex), cfg.atqa, sizeof(cfg.atqa));
        lua_pushstring(L, atqa_hex);
        lua_setfield(L, -2, "atqa_hex");
    }
    if(cfg.sak_set)
    {
        lua_pushinteger(L, (lua_Integer)cfg.sak);
        lua_setfield(L, -2, "sak");
    }
    if(cfg.ats_len > 0U)
    {
        char ats_hex[sizeof(cfg.ats) * 2U + 1U];
        poom_lua_bytes_to_hex_(ats_hex, sizeof(ats_hex), cfg.ats, cfg.ats_len);
        lua_pushstring(L, ats_hex);
        lua_setfield(L, -2, "ats_hex");
    }
    if(cfg.uri_set)
    {
        lua_pushstring(L, cfg.uri);
        lua_setfield(L, -2, "uri");
    }
    if(cfg.image_path_set)
    {
        lua_pushstring(L, cfg.image_path);
        lua_setfield(L, -2, "image");
    }

    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_mode`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_mode_(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    poom_nfc_emu_mode_t mode;
    if(!poom_lua_nfc_parse_emul_mode_(s, &mode))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_mode(mode) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_uid`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_uid_(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    uint8_t uid[10];
    size_t uid_len = 0U;

    if(!poom_lua_hex_to_bytes_(s, uid, sizeof(uid), &uid_len))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_uid(uid, (uint8_t)uid_len) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_atqa`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_atqa_(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    uint8_t atqa[2];
    size_t atqa_len = 0U;

    if(!poom_lua_hex_to_bytes_(s, atqa, sizeof(atqa), &atqa_len) || (atqa_len != sizeof(atqa)))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_atqa(atqa) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_sak`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_sak_(lua_State* L)
{
    lua_Integer sak = luaL_checkinteger(L, 1);
    if((sak < 0) || (sak > 255))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_sak((uint8_t)sak) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_ats`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_ats_(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    uint8_t ats[32];
    size_t ats_len = 0U;

    if(!poom_lua_hex_to_bytes_(s, ats, sizeof(ats), &ats_len))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_ats(ats, (uint8_t)ats_len) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_uri`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_uri_(lua_State* L)
{
    const char* uri = luaL_checkstring(L, 1);
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_uri(uri) ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_image`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_image_(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_set_mful_image_file(path) ? 1 : 0);
    return 1;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_start_(lua_State* L)
{
    (void)L;
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, poom_nfc_emulator_start() ? 1 : 0);
    return 1;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_stop_(lua_State* L)
{
    (void)L;
    poom_nfc_emulator_stop();
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_nfc_emul_set_from_last_profile`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_set_from_last_profile_(lua_State* L)
{
    poom_nfc_profile_t p;
    (void)memset(&p, 0, sizeof(p));

    if(!poom_reader_get_last_profile(&p))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if(!poom_lua_nfc_ensure_started_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    bool ok = true;
    ok = ok && poom_nfc_emulator_set_mode(p.mode);
    ok = ok && poom_nfc_emulator_set_uid(p.uid, p.uid_len);
    if(p.atqa_set)
    {
        ok = ok && poom_nfc_emulator_set_atqa(p.atqa);
    }
    if(p.sak_set)
    {
        ok = ok && poom_nfc_emulator_set_sak(p.sak);
    }
    if(p.ats_len > 0U)
    {
        ok = ok && poom_nfc_emulator_set_ats(p.ats, p.ats_len);
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_nfc_emul_start_last_(lua_State* L)
{
    const bool ok = (poom_lua_nfc_emul_set_from_last_profile_(L) != 0) && lua_toboolean(L, -1);
    lua_pop(L, 1);
    if(!ok)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    return poom_lua_nfc_emul_start_(L);
}

static const luaL_Reg k_nfc_emul_lib[] = {
    {"reset", poom_lua_nfc_emul_reset_},
    {"isRunning", poom_lua_nfc_emul_is_running_},
    {"getConfig", poom_lua_nfc_emul_get_config_},
    {"setMode", poom_lua_nfc_emul_set_mode_},
    {"setUid", poom_lua_nfc_emul_set_uid_},
    {"setAtqa", poom_lua_nfc_emul_set_atqa_},
    {"setSak", poom_lua_nfc_emul_set_sak_},
    {"setAts", poom_lua_nfc_emul_set_ats_},
    {"setUri", poom_lua_nfc_emul_set_uri_},
    {"setImage", poom_lua_nfc_emul_set_image_},
    {"setFromLastProfile", poom_lua_nfc_emul_set_from_last_profile_},
    {"startLast", poom_lua_nfc_emul_start_last_},
    {"start", poom_lua_nfc_emul_start_},
    {"stop", poom_lua_nfc_emul_stop_},
    {NULL, NULL},
};

static const luaL_Reg k_nfc_tune_lib[] = {
    {"auto", poom_lua_nfc_tune_auto_},
    {"get", poom_lua_nfc_tune_get_},
    {"set", poom_lua_nfc_tune_set_},
    {NULL, NULL},
};

static const luaL_Reg k_nfc_profiles_lib[] = {
    {"list", poom_lua_nfc_profiles_list_},
    {"add", poom_lua_nfc_profiles_add_},
    {"addLast", poom_lua_nfc_profiles_add_last_},
    {"remove", poom_lua_nfc_profiles_remove_},
    {"clear", poom_lua_nfc_profiles_clear_},
    {NULL, NULL},
};

static const luaL_Reg k_nfc_lib[] = {
    {"start", poom_lua_nfc_start_},
    {"stop", poom_lua_nfc_stop_},
    {"setTech", poom_lua_nfc_set_tech_},
    {"getTech", poom_lua_nfc_get_tech_},
    {"scan", poom_lua_nfc_scan_},
    {"captureDump", poom_lua_nfc_capture_dump_},
    {"dumpToSd", poom_lua_nfc_dump_to_sd_},
    {"mfulToSd", poom_lua_nfc_mful_to_sd_},
    {"connect", poom_lua_nfc_connect_},
    {"send", poom_lua_nfc_send_},
    {"getLastRapdu", poom_lua_nfc_get_last_rapdu_},
    {"getLastProfile", poom_lua_nfc_get_last_profile_},
    {NULL, NULL},
};


void poom_lua_bindings_nfc_cleanup(void)
{
    poom_nfc_emulator_stop();
    poom_nfc_controller_stop();
}

void poom_lua_bindings_nfc_register(lua_State* L)
{
    if(L == NULL)
    {
        return;
    }

    luaL_newlib(L, k_nfc_lib);
    lua_pushstring(L, "all");
    lua_setfield(L, -2, "TECH_ALL");
    lua_pushstring(L, "a");
    lua_setfield(L, -2, "TECH_A");
    lua_pushstring(L, "b");
    lua_setfield(L, -2, "TECH_B");
    lua_pushstring(L, "f");
    lua_setfield(L, -2, "TECH_F");
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "TECH_V");
    lua_pushstring(L, "st25tb");
    lua_setfield(L, -2, "TECH_ST25TB");

    luaL_newlib(L, k_nfc_emul_lib);
    lua_setfield(L, -2, "emul");

    luaL_newlib(L, k_nfc_tune_lib);
    lua_setfield(L, -2, "tune");

    luaL_newlib(L, k_nfc_profiles_lib);
    lua_setfield(L, -2, "profiles");

    lua_setglobal(L, "NFC");
}
