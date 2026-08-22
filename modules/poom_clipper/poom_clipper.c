// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_clipper.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "poom_nfc_controller.h"
#include "poom_nfc_iso14443_4.h"

typedef struct
{
    uint16_t id;
    const char* name;
} poom_clipper_id_map_t;

typedef struct
{
    uint8_t apdu_buf[300];
    char apdu_hex_buf[700];
    uint8_t rapdu_buf[300];
    uint8_t file_identity[64];
    uint8_t file_ecash[64];
    uint8_t file_hist_idx[32];
    uint8_t file_history[600];
} poom_clipper_ctx_t;

static const poom_clipper_id_map_t poom_clipper_agencies[] = {
    {.id = 0x0001, .name = "AC Transit"},
    {.id = 0x0004, .name = "BART"},
    {.id = 0x0006, .name = "Caltrain"},
    {.id = 0x0008, .name = "CCTA"},
    {.id = 0x000B, .name = "GGT"},
    {.id = 0x000F, .name = "SamTrans"},
    {.id = 0x0011, .name = "VTA"},
    {.id = 0x0012, .name = "Muni"},
    {.id = 0x0019, .name = "GG Ferry"},
    {.id = 0x001B, .name = "SF Bay Ferry"},
};

static const poom_clipper_id_map_t poom_clipper_bart_zones[] = {
    {.id = 0x0001, .name = "Colma"},
    {.id = 0x0002, .name = "Daly City"},
    {.id = 0x0003, .name = "Balboa Park"},
    {.id = 0x0004, .name = "Glen Park"},
    {.id = 0x0005, .name = "24th St Mission"},
    {.id = 0x0006, .name = "16th St Mission"},
    {.id = 0x0007, .name = "Civic Center"},
    {.id = 0x0008, .name = "Powell St"},
    {.id = 0x0009, .name = "Montgomery St"},
    {.id = 0x000A, .name = "Embarcadero"},
    {.id = 0x000B, .name = "West Oakland"},
    {.id = 0x000C, .name = "12th St/Oakland City Center"},
    {.id = 0x000D, .name = "19th St/Oakland"},
    {.id = 0x000E, .name = "MacArthur"},
    {.id = 0x000F, .name = "Rockridge"},
    {.id = 0x0010, .name = "Orinda"},
    {.id = 0x0011, .name = "Lafayette"},
    {.id = 0x0012, .name = "Walnut Creek"},
    {.id = 0x0013, .name = "Pleasant Hill"},
    {.id = 0x0014, .name = "Concord"},
    {.id = 0x0015, .name = "North Concord/Martinez"},
    {.id = 0x0016, .name = "Pittsburg/Bay Point"},
    {.id = 0x0017, .name = "Ashby"},
    {.id = 0x0018, .name = "Downtown Berkeley"},
    {.id = 0x0019, .name = "North Berkeley"},
    {.id = 0x001A, .name = "El Cerrito Plaza"},
    {.id = 0x001B, .name = "El Cerrito Del Norte"},
    {.id = 0x001C, .name = "Richmond"},
    {.id = 0x001D, .name = "Lake Merritt"},
    {.id = 0x001E, .name = "Fruitvale"},
    {.id = 0x001F, .name = "Coliseum"},
    {.id = 0x0020, .name = "San Leandro"},
    {.id = 0x0021, .name = "Bay Fair"},
    {.id = 0x0022, .name = "Hayward"},
    {.id = 0x0023, .name = "South Hayward"},
    {.id = 0x0024, .name = "Union City"},
    {.id = 0x0025, .name = "Fremont"},
    {.id = 0x0026, .name = "Castro Valley"},
    {.id = 0x0027, .name = "Dublin/Pleasanton"},
    {.id = 0x0028, .name = "South San Francisco"},
    {.id = 0x0029, .name = "San Bruno"},
    {.id = 0x002A, .name = "SFO Airport"},
    {.id = 0x002B, .name = "Millbrae"},
    {.id = 0x002C, .name = "West Dublin/Pleasanton"},
    {.id = 0x002D, .name = "OAK Airport"},
    {.id = 0x002E, .name = "Warm Springs/South Fremont"},
    {.id = 0x002F, .name = "Milpitas"},
    {.id = 0x0030, .name = "Berryessa/North San Jose"},
};

static const poom_clipper_id_map_t poom_clipper_muni_zones[] = {
    {.id = 0x0000, .name = "City Street"},
    {.id = 0x0005, .name = "Embarcadero"},
    {.id = 0x0006, .name = "Montgomery"},
    {.id = 0x0007, .name = "Powell"},
    {.id = 0x0008, .name = "Civic Center"},
    {.id = 0x0009, .name = "Van Ness"},
    {.id = 0x000A, .name = "Church"},
    {.id = 0x000B, .name = "Castro"},
    {.id = 0x000C, .name = "Forest Hill"},
    {.id = 0x000D, .name = "West Portal"},
    {.id = 0x0019, .name = "Union Square/Market Street"},
    {.id = 0x001A, .name = "Chinatown - Rose Pak"},
    {.id = 0x001B, .name = "Yerba Buena/Moscone"},
};

static const poom_clipper_id_map_t poom_clipper_caltrain_zones[] = {
    {.id = 0x0001, .name = "Zone 1"},
    {.id = 0x0002, .name = "Zone 2"},
    {.id = 0x0003, .name = "Zone 3"},
    {.id = 0x0004, .name = "Zone 4"},
    {.id = 0x0005, .name = "Zone 5"},
    {.id = 0x0006, .name = "Zone 6"},
};

/**
 * @brief Internal helper for `poom_u16be`.
 *
 * @param[in] p Parameter passed to the function.
 * @return uint16_t
 */
static uint16_t poom_u16be_(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * @brief Internal helper for `poom_u32be`.
 *
 * @param[in] p Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t poom_u32be_(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * @brief Internal helper for `poom_i16be`.
 *
 * @param[in] p Parameter passed to the function.
 * @return int16_t
 */
static int16_t poom_i16be_(const uint8_t* p)
{
    return (int16_t)poom_u16be_(p);
}

/**
 * @brief Internal helper for `poom_clipper_find_name`.
 *
 * @param[in] id Parameter passed to the function.
 * @param[in] map Parameter passed to the function.
 * @param[in] map_len Parameter passed to the function.
 * @param[in] out_name Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_find_name_(uint16_t id,
                                   const poom_clipper_id_map_t* map,
                                   size_t map_len,
                                   const char** out_name)
{
    if(map == NULL || out_name == NULL)
    {
        return false;
    }

    for(size_t i = 0; i < map_len; i++)
    {
        if(map[i].id == id)
        {
            *out_name = map[i].name;
            return true;
        }
    }
    return false;
}

/**
 * @brief Internal helper for `poom_clipper_agency_name`.
 *
 * @param[in] agency_id Parameter passed to the function.
 * @return const char*
 */
static const char* poom_clipper_agency_name_(uint16_t agency_id)
{
    const char* name = NULL;
    if(poom_clipper_find_name_(agency_id,
                              poom_clipper_agencies,
                              sizeof(poom_clipper_agencies) / sizeof(poom_clipper_agencies[0]),
                              &name))
    {
        return name;
    }
    return "Unknown";
}

/**
 * @brief Internal helper for `poom_clipper_zone_name`.
 *
 * @param[in] agency_id Parameter passed to the function.
 * @param[in] zone_id Parameter passed to the function.
 * @return const char*
 */
static const char* poom_clipper_zone_name_(uint16_t agency_id, uint16_t zone_id)
{
    const char* name = NULL;

    switch(agency_id)
    {
        case 0x0004:
            if(poom_clipper_find_name_(
                   zone_id,
                   poom_clipper_bart_zones,
                   sizeof(poom_clipper_bart_zones) / sizeof(poom_clipper_bart_zones[0]),
                   &name))
            {
                return name;
            }
            break;
        case 0x0012:
            if(poom_clipper_find_name_(
                   zone_id,
                   poom_clipper_muni_zones,
                   sizeof(poom_clipper_muni_zones) / sizeof(poom_clipper_muni_zones[0]),
                   &name))
            {
                return name;
            }
            break;
        case 0x0006:
            if(poom_clipper_find_name_(
                   zone_id,
                   poom_clipper_caltrain_zones,
                   sizeof(poom_clipper_caltrain_zones) / sizeof(poom_clipper_caltrain_zones[0]),
                   &name))
            {
                return name;
            }
            break;
        default:
            break;
    }

    return "Unknown";
}

/**
 * @brief Internal helper for `poom_clipper_print_money`.
 *
 * @param[in] label Parameter passed to the function.
 * @param[in] cents Parameter passed to the function.
 * @return void
 */
static void poom_clipper_print_money_(const char* label, int16_t cents)
{
    int32_t raw = (int32_t)cents;
    int32_t abs_v = (raw < 0) ? -raw : raw;
    printf("%s%s$%ld.%02ld\r\n",
           (label != NULL) ? label : "",
           (raw < 0) ? "-" : "",
           (long)(abs_v / 100),
           (long)(abs_v % 100));
}

/**
 * @brief Internal helper for `poom_clipper_print_ts`.
 *
 * @param[in] label Parameter passed to the function.
 * @param[in] ts_1900 Parameter passed to the function.
 * @return void
 */
static void poom_clipper_print_ts_(const char* label, uint32_t ts_1900)
{
    const uint32_t unix_offset = 2208988800UL;
    time_t unix_ts = 0;
    struct tm tm = {0};

    if(ts_1900 < unix_offset)
    {
        printf("%s<invalid>\r\n", (label != NULL) ? label : "");
        return;
    }

    unix_ts = (time_t)(ts_1900 - unix_offset);
    gmtime_r(&unix_ts, &tm);
    printf("%s%04d-%02d-%02d %02d:%02d:%02d UTC\r\n",
           (label != NULL) ? label : "",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday,
           tm.tm_hour,
           tm.tm_min,
           tm.tm_sec);
}

/**
 * @brief Internal helper for `poom_clipper_apdu_hex`.
 *
 * @param[in] apdu Parameter passed to the function.
 * @param[in] apdu_len Parameter passed to the function.
 * @param[in] out_hex Parameter passed to the function.
 * @param[in] out_hex_sz Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_apdu_hex_(const uint8_t* apdu,
                                  size_t apdu_len,
                                  char* out_hex,
                                  size_t out_hex_sz)
{
    if((apdu == NULL) || (out_hex == NULL) || (out_hex_sz < 3U))
    {
        return false;
    }
    if((apdu_len * 2U + 1U) > out_hex_sz)
    {
        return false;
    }

    size_t wr = 0U;
    for(size_t i = 0U; i < apdu_len; i++)
    {
        (void)snprintf(&out_hex[wr], out_hex_sz - wr, "%02X", apdu[i]);
        wr += 2U;
    }
    out_hex[wr] = '\0';
    return true;
}

/**
 * @brief Internal helper for `poom_clipper_ctx_alloc`.
 *
 * @return poom_clipper_ctx_t*
 */
static poom_clipper_ctx_t* poom_clipper_ctx_alloc_(void)
{
    return (poom_clipper_ctx_t*)calloc(1U, sizeof(poom_clipper_ctx_t));
}

/**
 * @brief Internal helper for `poom_clipper_desfire_cmd_collect_ctx`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] ins Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] data_len Parameter passed to the function.
 * @param[in] out_buf Parameter passed to the function.
 * @param[in] out_buf_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @param[in] out_status Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_desfire_cmd_collect_ctx_(poom_clipper_ctx_t* ctx,
                                                  uint8_t ins,
                                                  const uint8_t* data,
                                                  size_t data_len,
                                                  uint8_t* out_buf,
                                                  size_t out_buf_max,
                                                  size_t* out_len,
                                                  uint8_t* out_status)
{
    size_t total = 0U;

    if((ctx == NULL) || (data_len > 250U) || (out_len == NULL) || (out_status == NULL))
    {
        return false;
    }

    ctx->apdu_buf[0] = 0x90;
    ctx->apdu_buf[1] = ins;
    ctx->apdu_buf[2] = 0x00;
    ctx->apdu_buf[3] = 0x00;
    ctx->apdu_buf[4] = (uint8_t)data_len;
    if(data_len > 0U && data != NULL)
    {
        memcpy(&ctx->apdu_buf[5], data, data_len);
        ctx->apdu_buf[5 + data_len] = 0x00;
    }

    for(uint8_t round = 0U; round < 32U; round++)
    {
        size_t rapdu_len = 0U;
        uint8_t sw = 0x00;
        size_t payload_len = 0U;
        const size_t apdu_len = (data_len > 0U) ? (6U + data_len) : 5U;

        if(!poom_clipper_apdu_hex_(
               ctx->apdu_buf,
               apdu_len,
               ctx->apdu_hex_buf,
               sizeof(ctx->apdu_hex_buf)))
        {
            return false;
        }
        if(!poom_nfc_controller_send_raw_hex(ctx->apdu_hex_buf))
        {
            return false;
        }

        if(!poom_reader_get_last_rapdu(
               ctx->rapdu_buf, sizeof(ctx->rapdu_buf), &rapdu_len) ||
           rapdu_len < 2U)
        {
            printf("clipper: failed to fetch R-APDU\r\n");
            return false;
        }
        if(ctx->rapdu_buf[rapdu_len - 2U] != 0x91U)
        {
            printf("clipper: unexpected SW1=%02X\r\n", ctx->rapdu_buf[rapdu_len - 2U]);
            return false;
        }

        sw = ctx->rapdu_buf[rapdu_len - 1U];
        payload_len = rapdu_len - 2U;

        if(payload_len > 0U)
        {
            if(out_buf == NULL || (total + payload_len) > out_buf_max)
            {
                printf("clipper: response buffer too small\r\n");
                return false;
            }
            memcpy(&out_buf[total], ctx->rapdu_buf, payload_len);
            total += payload_len;
        }

        if(sw != 0xAFU)
        {
            *out_len = total;
            *out_status = sw;
            return true;
        }

        ctx->apdu_buf[0] = 0x90;
        ctx->apdu_buf[1] = 0xAF;
        ctx->apdu_buf[2] = 0x00;
        ctx->apdu_buf[3] = 0x00;
        ctx->apdu_buf[4] = 0x00;
        data = NULL;
        data_len = 0U;
    }

    printf("clipper: too many continuation frames\r\n");
    return false;
}

bool poom_clipper_desfire_cmd_collect(uint8_t ins,
                                     const uint8_t* data,
                                     size_t data_len,
                                     uint8_t* out_buf,
                                     size_t out_buf_max,
                                     size_t* out_len,
                                     uint8_t* out_status)
{
    bool ok;
    poom_clipper_ctx_t* ctx = poom_clipper_ctx_alloc_();

    if(ctx == NULL)
    {
        printf("clipper: no memory\r\n");
        return false;
    }

    ok = poom_clipper_desfire_cmd_collect_ctx_(
        ctx, ins, data, data_len, out_buf, out_buf_max, out_len, out_status);
    free(ctx);
    return ok;
}

/**
 * @brief Handles the current module action.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] aid Parameter passed to the function.
 * @param[in] label Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_select_app_(poom_clipper_ctx_t* ctx,
                                     const uint8_t aid[3],
                                     const char* label)
{
    uint8_t status = 0U;
    size_t out_len = 0U;

    if(!poom_clipper_desfire_cmd_collect_ctx_(
           ctx, 0x5A, aid, 3U, NULL, 0U, &out_len, &status))
    {
        return false;
    }
    if(status != 0x00U)
    {
        printf("clipper: select %s app failed SW=91%02X\r\n", label, status);
        return false;
    }

    return true;
}

/**
 * @brief Internal helper for `poom_clipper_read_file`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] file_id Parameter passed to the function.
 * @param[in] offset Parameter passed to the function.
 * @param[in] length Parameter passed to the function.
 * @param[in] out_data Parameter passed to the function.
 * @param[in] out_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_read_file_(poom_clipper_ctx_t* ctx,
                                    uint8_t file_id,
                                    uint32_t offset,
                                    uint32_t length,
                                    uint8_t* out_data,
                                    size_t out_max,
                                    size_t* out_len)
{
    uint8_t params[7];
    uint8_t status = 0U;

    params[0] = file_id;
    params[1] = (uint8_t)(offset & 0xFFU);
    params[2] = (uint8_t)((offset >> 8) & 0xFFU);
    params[3] = (uint8_t)((offset >> 16) & 0xFFU);
    params[4] = (uint8_t)(length & 0xFFU);
    params[5] = (uint8_t)((length >> 8) & 0xFFU);
    params[6] = (uint8_t)((length >> 16) & 0xFFU);

    if(!poom_clipper_desfire_cmd_collect_ctx_(
           ctx, 0xBD, params, sizeof(params), out_data, out_max, out_len, &status))
    {
        return false;
    }
    if(status != 0x00U)
    {
        printf("clipper: read file %u failed SW=91%02X\r\n", (unsigned)file_id, status);
        return false;
    }
    return true;
}

int poom_clipper_print_history(void)
{
    static const uint8_t aid_card[3] = {0x90, 0x11, 0xF2};
    static const uint8_t aid_mobile[3] = {0x91, 0x11, 0xF2};
    poom_clipper_ctx_t* ctx = NULL;
    size_t len_identity = 0U;
    size_t len_ecash = 0U;
    size_t len_hist_idx = 0U;
    size_t len_history = 0U;
    int rc = 1;

    ctx = poom_clipper_ctx_alloc_();
    if(ctx == NULL)
    {
        printf("clipper: no memory\r\n");
        return 1;
    }

    if(!poom_nfc_controller_start())
    {
        goto out_free;
    }

    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_A);
    if(!poom_nfc_controller_connect())
    {
        printf("clipper: card connect failed\r\n");
        goto out;
    }

    if(!poom_clipper_select_app_(ctx, aid_card, "card"))
    {
        if(!poom_clipper_select_app_(ctx, aid_mobile, "mobile"))
        {
            printf("clipper: app 0x9011F2/0x9111F2 not found\r\n");
            goto out;
        }
    }

    if(!poom_clipper_read_file_(ctx,
                                8U,
                                0U,
                                32U,
                                ctx->file_identity,
                                sizeof(ctx->file_identity),
                                &len_identity) ||
       len_identity < 5U)
    {
        printf("clipper: failed to read identity file\r\n");
        goto out;
    }

    if(!poom_clipper_read_file_(ctx,
                                2U,
                                0U,
                                32U,
                                ctx->file_ecash,
                                sizeof(ctx->file_ecash),
                                &len_ecash) ||
       len_ecash < 0x14U)
    {
        printf("clipper: failed to read e-cash file\r\n");
        goto out;
    }

    if(!poom_clipper_read_file_(ctx,
                                6U,
                                0U,
                                16U,
                                ctx->file_hist_idx,
                                sizeof(ctx->file_hist_idx),
                                &len_hist_idx) ||
       len_hist_idx < 16U)
    {
        printf("clipper: failed to read history index file\r\n");
        goto out;
    }

    if(!poom_clipper_read_file_(ctx,
                                14U,
                                0U,
                                512U,
                                ctx->file_history,
                                sizeof(ctx->file_history),
                                &len_history) ||
       len_history < 32U)
    {
        printf("clipper: failed to read history file\r\n");
        goto out;
    }

    {
        const uint32_t serial = poom_u32be_(&ctx->file_identity[1]);
        const uint16_t counter = poom_u16be_(&ctx->file_ecash[2]);
        const uint32_t ts_1900 = poom_u32be_(&ctx->file_ecash[4]);
        const uint16_t terminal = poom_u16be_(&ctx->file_ecash[8]);
        const uint16_t txn_id = poom_u16be_(&ctx->file_ecash[0x10]);
        const int16_t balance = poom_i16be_(&ctx->file_ecash[0x12]);

        printf("Clipper\r\n");
        printf("  Serial: %" PRIu32 "\r\n", serial);
        poom_clipper_print_money_("  Balance: ", balance);
        poom_clipper_print_ts_("  Last Update: ", ts_1900);
        printf("  Terminal: 0x%04X\r\n", (unsigned)terminal);
        printf("  Txn Id: %u\r\n", (unsigned)txn_id);
        printf("  Counter: %u\r\n", (unsigned)counter);
    }

    printf("Ride History\r\n");
    for(size_t i = 0U; i < 16U; i++)
    {
        const uint8_t rec_no = ctx->file_hist_idx[i];
        const size_t rec_off = (size_t)rec_no * 0x20U;
        const uint8_t* rec;
        uint16_t agency_id;
        uint16_t zone_on;
        uint16_t zone_off;
        uint16_t vehicle;
        int16_t fare;
        uint32_t time_on;
        uint32_t time_off;

        if(rec_no == 0xFFU)
        {
            break;
        }
        if((rec_off + 0x20U) > len_history)
        {
            break;
        }

        rec = &ctx->file_history[rec_off];
        if(rec[0] != 0x10U)
        {
            continue;
        }

        agency_id = poom_u16be_(&rec[2]);
        if(agency_id == 0U)
        {
            continue;
        }
        fare = poom_i16be_(&rec[6]);
        vehicle = poom_u16be_(&rec[0x0A]);
        time_on = poom_u32be_(&rec[0x0C]);
        time_off = poom_u32be_(&rec[0x10]);
        zone_on = poom_u16be_(&rec[0x14]);
        zone_off = poom_u16be_(&rec[0x16]);

        printf("  #%u\r\n", (unsigned)(i + 1U));
        poom_clipper_print_ts_("    Date: ", time_on);
        poom_clipper_print_money_("    Fare: ", fare);
        printf("    Agency: %s (0x%04X)\r\n",
               poom_clipper_agency_name_(agency_id),
               (unsigned)agency_id);
        printf("    On: %s (0x%04X)\r\n",
               poom_clipper_zone_name_(agency_id, zone_on),
               (unsigned)zone_on);
        if(vehicle != 0U)
        {
            printf("    Vehicle: %u\r\n", (unsigned)vehicle);
        }
        if(time_off != 0U)
        {
            printf("    Off: %s (0x%04X)\r\n",
                   poom_clipper_zone_name_(agency_id, zone_off),
                   (unsigned)zone_off);
            poom_clipper_print_ts_("    Date Off: ", time_off);
        }
    }

    rc = 0;

out:
    poom_nfc_controller_stop();
out_free:
    free(ctx);
    return rc;
}
