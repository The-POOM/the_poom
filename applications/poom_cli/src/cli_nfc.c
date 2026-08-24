#include "argtable3/argtable3.h"
#include "cli.h"
#include "cli_nfc.h"
#include "esp_console.h"
#include "poom_nfc_controller.h"
#include "poom_nfc_debug.h"
#include "poom_nfc_dump.h"
#include "poom_nfc_ats.h"
#include "poom_nfc_emulator.h"
#include "poom_nfc_emv.h"
#include "poom_nfc_iso14443_4.h"
#include "poom_nfc_iso7816.h"
#include "poom_nfc_mifare_classic.h"
#include "poom_nfc_profile_store.h"
#include "poom_nfc_store.h"
#include "poom_nfc_tlv.h"
#include "poom_clipper.h"
#include "sd_card.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

/**
 * @brief Internal helper for `poom_nfc_type_char`.
 *
 * @param[in] type Parameter passed to the function.
 * @return char
 */
static char poom_nfc_type_char(uint8_t type)
{
    switch(type)
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
 * @brief Internal helper for `print_uid_hex`.
 *
 * @param[in] uid Parameter passed to the function.
 * @param[in] uid_len Parameter passed to the function.
 * @return void
 */
static void print_uid_hex(const uint8_t* uid, uint8_t uid_len)
{
    if(uid == NULL || uid_len == 0)
    {
        return;
    }
    for(uint8_t i = 0; i < uid_len; i++)
    {
        printf("%02X", uid[i]);
    }
}

/**
 * @brief Internal helper for `poom_hex_nibble`.
 *
 * @param[in] c Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return bool
 */
static bool poom_hex_nibble(char c, uint8_t* out)
{
    if(c >= '0' && c <= '9')
    {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if(c >= 'a' && c <= 'f')
    {
        *out = (uint8_t)(10 + (c - 'a'));
        return true;
    }
    if(c >= 'A' && c <= 'F')
    {
        *out = (uint8_t)(10 + (c - 'A'));
        return true;
    }
    return false;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] in Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_parse_hex_bytes(const char* in,
                                 uint8_t* out,
                                 size_t out_max,
                                 size_t* out_len)
{
    size_t wr = 0;
    int8_t hi = -1;

    if(in == NULL || out == NULL || out_len == NULL)
        return false;

    for(const char* p = in; *p != '\0'; p++)
    {
        uint8_t nibble = 0;
        char c         = *p;

        if(c == ':' || c == '-' || c == ' ' || c == '\t' || c == ',')
        {
            continue;
        }
        if(c == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            p++;
            continue;
        }
        if(!poom_hex_nibble(c, &nibble))
        {
            return false;
        }

        if(hi < 0)
        {
            hi = (int8_t)nibble;
        }
        else
        {
            if(wr >= out_max)
                return false;
            out[wr++] = (uint8_t)(((uint8_t)hi << 4) | nibble);
            hi        = -1;
        }
    }

    if(hi >= 0)
    {
        return false;
    }
    *out_len = wr;
    return true;
}

/**
 * @brief Internal helper for `poom_join_argv`.
 *
 * @param[in] out Parameter passed to the function.
 * @param[in] out_sz Parameter passed to the function.
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @param[in] start_idx Parameter passed to the function.
 * @return bool
 */
static bool poom_join_argv(char* out,
                           size_t out_sz,
                           int argc,
                           char** argv,
                           int start_idx)
{
    size_t used = 0;

    if(out == NULL || out_sz == 0 || argv == NULL || argc <= start_idx)
    {
        return false;
    }
    out[0] = '\0';

    for(int i = start_idx; i < argc; i++)
    {
        size_t part_len = strlen(argv[i]);

        if(i > start_idx)
        {
            if((used + 1U) >= out_sz)
            {
                return false;
            }
            out[used++] = ' ';
            out[used]   = '\0';
        }

        if((used + part_len) >= out_sz)
        {
            return false;
        }

        memcpy(&out[used], argv[i], part_len);
        used += part_len;
        out[used] = '\0';
    }

    return true;
}

/**
 * @brief Internal helper for `print_key6`.
 *
 * @param[in] key Parameter passed to the function.
 * @return void
 */
static void print_key6(const uint8_t key[6])
{
    if(key == NULL)
    {
        return;
    }
    for(size_t i = 0; i < 6U; i++)
    {
        printf("%02X", key[i]);
    }
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] st Parameter passed to the function.
 * @return const char*
 */
static const char* poom_mifare_auth_status_str_(poom_mifare_auth_status_t st)
{
    switch(st)
    {
        case POOM_MIFARE_AUTH_STATUS_FULL:
            return "full";
        case POOM_MIFARE_AUTH_STATUS_PARTIAL:
            return "partial";
        case POOM_MIFARE_AUTH_STATUS_FAIL:
            return "fail";
        default:
            return "none";
    }
}

/**
 * @brief Internal helper for `poom_trim_spaces_inplace`.
 *
 * @param[in] s Parameter passed to the function.
 * @return void
 */
static void poom_trim_spaces_inplace_(char* s)
{
    size_t len;
    size_t start = 0U;
    size_t end;

    if(s == NULL)
    {
        return;
    }

    len = strlen(s);
    while((start < len) && (s[start] == ' ' || s[start] == '\t'))
    {
        start++;
    }

    end = len;
    while((end > start) && (s[end - 1U] == ' ' || s[end - 1U] == '\t'))
    {
        end--;
    }

    if(start > 0U)
    {
        (void)memmove(s, &s[start], end - start);
    }
    s[end - start] = '\0';
}

/**
 * @brief Internal helper for `poom_normalize_emul_image_path`.
 *
 * @param[in] path Parameter passed to the function.
 * @param[in] path_sz Parameter passed to the function.
 * @return bool
 */
static bool poom_normalize_emul_image_path_(char* path, size_t path_sz)
{
    static const char* k_hint_suffix = " (emu-ready MFUL .nfc)";
    char* hint_pos;

    if(path == NULL || path_sz == 0U)
    {
        return false;
    }

    poom_trim_spaces_inplace_(path);

    hint_pos = strstr(path, k_hint_suffix);
    if(hint_pos != NULL)
    {
        *hint_pos = '\0';
        poom_trim_spaces_inplace_(path);
    }

    if(path[0] == '\0')
    {
        return false;
    }

    if((strncmp(path, SD_CARD_PATH "/", strlen(SD_CARD_PATH) + 1U) != 0) &&
       ((strncmp(path, "/nfc_dumps/", 11) == 0) ||
        (strncmp(path, "/NFCDUMP/", 9) == 0) ||
        (strncmp(path, "nfc_dumps/", 10) == 0) ||
        (strncmp(path, "NFCDUMP/", 8) == 0)))
    {
        char tmp[160];
        int n;

        if(path[0] == '/')
        {
            n = snprintf(tmp, sizeof(tmp), "%s%s", SD_CARD_PATH, path);
        }
        else
        {
            n = snprintf(tmp, sizeof(tmp), "%s/%s", SD_CARD_PATH, path);
        }
        if(n <= 0 || (size_t)n >= sizeof(tmp) || (size_t)n >= path_sz)
        {
            return false;
        }
        (void)memcpy(path, tmp, (size_t)n + 1U);
    }

    return true;
}

typedef struct
{
    uint16_t id;
    const char* name;
} poom_clipper_id_map_t;
#if 0

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
           label,
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
    const uint32_t epoch_delta = 2208988800UL;
    if(ts_1900 < epoch_delta)
    {
        printf("%s<invalid>\r\n", label);
        return;
    }

    time_t unix_ts = (time_t)(ts_1900 - epoch_delta);
    struct tm utc_tm;
    char buf[32];

    if(gmtime_r(&unix_ts, &utc_tm) == NULL)
    {
        printf("%s<invalid>\r\n", label);
        return;
    }

    if(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &utc_tm) == 0)
    {
        printf("%s<invalid>\r\n", label);
        return;
    }

    printf("%s%s\r\n", label, buf);
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
    if(apdu == NULL || out_hex == NULL || out_hex_sz == 0U)
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

static uint8_t poom_clipper_apdu_buf_[300];
static char poom_clipper_apdu_hex_buf_[700];
static uint8_t poom_clipper_rapdu_buf_[300];
static uint8_t poom_isodep_rapdu_ppse_[300];
static uint8_t poom_isodep_desfire_gv_[300];

#define POOM_NFC_CLI_DUMP_DIR "/nfc_dumps"

/**
 * @brief Internal helper for `poom_clipper_desfire_cmd_collect`.
 *
 * @param[in] ins Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] data_len Parameter passed to the function.
 * @param[in] out_buf Parameter passed to the function.
 * @param[in] out_buf_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @param[in] out_status Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_desfire_cmd_collect_(uint8_t ins,
                                              const uint8_t* data,
                                              size_t data_len,
                                              uint8_t* out_buf,
                                              size_t out_buf_max,
                                              size_t* out_len,
                                              uint8_t* out_status)
{
    size_t total = 0U;

    if(data_len > 250U || out_len == NULL || out_status == NULL)
    {
        return false;
    }

    poom_clipper_apdu_buf_[0] = 0x90;
    poom_clipper_apdu_buf_[1] = ins;
    poom_clipper_apdu_buf_[2] = 0x00;
    poom_clipper_apdu_buf_[3] = 0x00;
    poom_clipper_apdu_buf_[4] = (uint8_t)data_len;
    if(data_len > 0U && data != NULL)
    {
        memcpy(&poom_clipper_apdu_buf_[5], data, data_len);
        poom_clipper_apdu_buf_[5 + data_len] = 0x00;
    }

    for(uint8_t round = 0U; round < 32U; round++)
    {
        size_t rapdu_len = 0U;
        uint8_t sw = 0x00;
        size_t payload_len = 0U;
        const size_t apdu_len = (data_len > 0U) ? (6U + data_len) : 5U;

        if(!poom_clipper_apdu_hex_(
               poom_clipper_apdu_buf_,
               apdu_len,
               poom_clipper_apdu_hex_buf_,
               sizeof(poom_clipper_apdu_hex_buf_)))
        {
            return false;
        }
        if(!poom_nfc_controller_send_raw_hex(poom_clipper_apdu_hex_buf_))
        {
            return false;
        }

        if(!poom_reader_get_last_rapdu(
               poom_clipper_rapdu_buf_, sizeof(poom_clipper_rapdu_buf_), &rapdu_len) ||
           rapdu_len < 2U)
        {
            printf("clipper: failed to fetch R-APDU\r\n");
            return false;
        }
        if(poom_clipper_rapdu_buf_[rapdu_len - 2U] != 0x91U)
        {
            printf("clipper: unexpected SW1=%02X\r\n", poom_clipper_rapdu_buf_[rapdu_len - 2U]);
            return false;
        }

        sw = poom_clipper_rapdu_buf_[rapdu_len - 1U];
        payload_len = rapdu_len - 2U;

        if(payload_len > 0U)
        {
            if(out_buf == NULL || (total + payload_len) > out_buf_max)
            {
                printf("clipper: response buffer too small\r\n");
                return false;
            }
            memcpy(&out_buf[total], poom_clipper_rapdu_buf_, payload_len);
            total += payload_len;
        }

        if(sw != 0xAFU)
        {
            *out_len = total;
            *out_status = sw;
            return true;
        }

        poom_clipper_apdu_buf_[0] = 0x90;
        poom_clipper_apdu_buf_[1] = 0xAF;
        poom_clipper_apdu_buf_[2] = 0x00;
        poom_clipper_apdu_buf_[3] = 0x00;
        poom_clipper_apdu_buf_[4] = 0x00;
        data = NULL;
        data_len = 0U;
    }

    printf("clipper: too many continuation frames\r\n");
    return false;
}

/**
 * @brief Handles the current module action.
 *
 * @param[in] aid Parameter passed to the function.
 * @param[in] label Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_select_app_(const uint8_t aid[3], const char* label)
{
    uint8_t status = 0U;
    size_t out_len = 0U;

    if(!poom_clipper_desfire_cmd_collect_(
           0x5A, aid, 3U, NULL, 0U, &out_len, &status))
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
#endif /* clipper moved to modules/poom_clipper */

static uint8_t poom_isodep_rapdu_ppse_[300];
static uint8_t poom_isodep_desfire_gv_[300];

/**
 * @brief Internal helper for `poom_fprint_hex_spaced`.
 *
 * @param[in] f Parameter passed to the function.
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return void
 */
static void poom_fprint_hex_spaced_(FILE* f, const uint8_t* data, size_t len)
{
    if((f == NULL) || (data == NULL))
    {
        return;
    }

    for(size_t i = 0U; i < len; i++)
    {
        (void)fprintf(f, "%02X%s", (unsigned)data[i], (i + 1U < len) ? " " : "");
    }
}

/**
 * @brief Internal helper for `poom_path_exists`.
 *
 * @param[in] path Parameter passed to the function.
 * @return bool
 */
static bool poom_path_exists_(const char* path)
{
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

/**
 * @brief Internal helper for `poom_isodep_send_apdu_capture`.
 *
 * @param[in] apdu_hex Parameter passed to the function.
 * @param[in] out_rapdu Parameter passed to the function.
 * @param[in] out_rapdu_max Parameter passed to the function.
 * @param[in] out_rapdu_len Parameter passed to the function.
 * @return bool
 */
static bool poom_isodep_send_apdu_capture_(const char* apdu_hex,
                                           uint8_t* out_rapdu,
                                           size_t out_rapdu_max,
                                           size_t* out_rapdu_len)
{
    if((apdu_hex == NULL) || (out_rapdu_len == NULL))
    {
        return false;
    }
    *out_rapdu_len = 0U;

    if(!poom_nfc_controller_send_raw_hex(apdu_hex))
    {
        return false;
    }

    return poom_reader_get_last_rapdu(out_rapdu, out_rapdu_max, out_rapdu_len);
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] p Parameter passed to the function.
 * @param[in] ppse_ok Parameter passed to the function.
 * @param[in] ppse_rapdu Parameter passed to the function.
 * @param[in] ppse_rapdu_len Parameter passed to the function.
 * @param[in] gv_ok Parameter passed to the function.
 * @param[in] gv_data Parameter passed to the function.
 * @param[in] gv_data_len Parameter passed to the function.
 * @param[in] gv_status Parameter passed to the function.
 * @param[in] out_rel_path Parameter passed to the function.
 * @param[in] out_rel_path_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_isodep_capture_save_sd_(const poom_nfc_profile_t* p,
                                            bool ppse_ok,
                                            const uint8_t* ppse_rapdu,
                                            size_t ppse_rapdu_len,
                                            bool gv_ok,
                                            const uint8_t* gv_data,
                                            size_t gv_data_len,
                                            uint8_t gv_status,
                                            char* out_rel_path,
                                            size_t out_rel_path_len)
{
    char rel_path[128];
    char abs_path[192];
    char abs_dump_dir[96];
    FILE* f = NULL;
    bool use_dump_dir = true;
    esp_err_t err;
    uint32_t seed = 0U;
    static const char* k_dump_dir = "/NFCDUMP";

    if(p == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)snprintf(abs_dump_dir, sizeof(abs_dump_dir), "%s%s", SD_CARD_PATH, k_dump_dir);

    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    err = sd_card_create_dir(k_dump_dir);
    if((err != ESP_OK) && !poom_path_exists_(abs_dump_dir))
    {
        use_dump_dir = false;
        printf("nfc-isodep-dump-save-sd: /NFCDUMP unavailable, fallback to SD root\r\n");
    }

    seed = (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    if(p->uid_len > 0U)
    {
        seed ^= ((uint32_t)p->uid[0] << 24);
        seed ^= ((uint32_t)p->uid[p->uid_len - 1U] << 16);
    }

    for(int i = 0; i < 1000; i++)
    {
        struct stat st;
        const uint32_t token = (seed + (uint32_t)i) & 0x0FFFFFFFU;
        if(use_dump_dir)
        {
            (void)snprintf(rel_path, sizeof(rel_path), "%s/I%07X.TXT", k_dump_dir, (unsigned)token);
        }
        else
        {
            (void)snprintf(rel_path, sizeof(rel_path), "/I%07X.TXT", (unsigned)token);
        }
        (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
        if(stat(abs_path, &st) != 0)
        {
            break;
        }
    }

    if((out_rel_path != NULL) && (out_rel_path_len > 0U))
    {
        (void)snprintf(out_rel_path, out_rel_path_len, "%s", rel_path);
    }

    (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
    f = fopen(abs_path, "w");
    if(f == NULL)
    {
        const int open_errno = errno;
        if(use_dump_dir)
        {
            use_dump_dir = false;
            for(int i = 0; i < 1000; i++)
            {
                struct stat st;
                const uint32_t token = (seed + (uint32_t)i) & 0x0FFFFFFFU;
                (void)snprintf(rel_path, sizeof(rel_path), "/I%07X.TXT", (unsigned)token);
                (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
                if(stat(abs_path, &st) != 0)
                {
                    break;
                }
            }

            (void)snprintf(abs_path, sizeof(abs_path), "%s%s", SD_CARD_PATH, rel_path);
            f = fopen(abs_path, "w");
            if(f == NULL)
            {
                printf("nfc-isodep-dump-save-sd: fopen failed [%s] errno=%d\r\n", abs_path, errno);
                return ESP_ERR_FILE_OPEN_FAILED;
            }
        }
        else
        {
            printf("nfc-isodep-dump-save-sd: fopen failed [%s] errno=%d\r\n", abs_path, open_errno);
            return ESP_ERR_FILE_OPEN_FAILED;
        }
    }

    if((out_rel_path != NULL) && (out_rel_path_len > 0U))
    {
        (void)snprintf(out_rel_path, out_rel_path_len, "%s", rel_path);
    }

    if(f == NULL)
    {
        return ESP_ERR_FILE_OPEN_FAILED;
    }

    (void)fprintf(f, "Filetype: POOM NFC ISO-DEP capture\n");
    (void)fprintf(f, "Version: 1\n\n");

    (void)fprintf(f, "UID: ");
    poom_fprint_hex_spaced_(f, p->uid, p->uid_len);
    (void)fprintf(f, "\n");
    if(p->atqa_set)
    {
        (void)fprintf(f, "ATQA: %02X %02X\n", p->atqa[0], p->atqa[1]);
    }
    if(p->sak_set)
    {
        (void)fprintf(f, "SAK: %02X\n", p->sak);
    }
    (void)fprintf(f, "ATS: ");
    if(p->ats_len > 0U)
    {
        poom_fprint_hex_spaced_(f, p->ats, p->ats_len);
    }
    else
    {
        (void)fprintf(f, "N/A");
    }
    (void)fprintf(f, "\n\n");

    (void)fprintf(f, "APDU PPSE (00A404000E325041592E5359532E444446303100): %s\n",
                  ppse_ok ? "OK" : "FAIL");
    if(ppse_ok)
    {
        (void)fprintf(f, "PPSE R-APDU: ");
        poom_fprint_hex_spaced_(f, ppse_rapdu, ppse_rapdu_len);
        (void)fprintf(f, "\n");
    }

    (void)fprintf(f, "\nDESFire GET_VERSION (90 60 00 00 00): %s\n", gv_ok ? "OK" : "FAIL");
    if(gv_ok)
    {
        (void)fprintf(f, "DESFire status: 91%02X\n", (unsigned)gv_status);
        (void)fprintf(f, "DESFire data: ");
        poom_fprint_hex_spaced_(f, gv_data, gv_data_len);
        (void)fprintf(f, "\n");
    }

    (void)fclose(f);
    return ESP_OK;
}

#if 0 /* clipper moved to modules/poom_clipper */

/**
 * @brief Internal helper for `poom_clipper_read_file`.
 *
 * @param[in] file_id Parameter passed to the function.
 * @param[in] offset Parameter passed to the function.
 * @param[in] length Parameter passed to the function.
 * @param[in] out_data Parameter passed to the function.
 * @param[in] out_max Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_clipper_read_file_(uint8_t file_id,
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

    if(!poom_clipper_desfire_cmd_collect_(
           0xBD, params, sizeof(params), out_data, out_max, out_len, &status))
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
#endif

/* =========================================================
 *  Handlers CLI
 * ========================================================= */

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return poom_nfc_controller_start() ? 0 : 1;
}

static uint32_t poom_parse_timeout_ms_or_default_(int argc,
                                                 char** argv,
                                                 uint32_t default_timeout);

/**
 * @brief Internal helper for `cmd_nfc_scan`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_scan(int argc, char** argv)
{
    const uint32_t timeout_ms = poom_parse_timeout_ms_or_default_(argc, argv, 3000U);
    return poom_nfc_controller_scan_once(timeout_ms) ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_connect`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_connect(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return poom_nfc_controller_connect() ? 0 : 1;
}

static struct
{
    struct arg_str* hex;
    struct arg_end* end;
} nfc_send_args;

static struct
{
    struct arg_lit* try_b;
    struct arg_end* end;
} nfc_mfc_discover_args;

static struct
{
    struct arg_int* block;
    struct arg_str* key;
    struct arg_end* end;
} nfc_mfc_auth_args;

static struct
{
    struct arg_int* block;
    struct arg_end* end;
} nfc_mfc_read_args;

static struct
{
    struct arg_int* block;
    struct arg_str* hex;
    struct arg_end* end;
} nfc_mfc_write_args;

static struct
{
    struct arg_int* block;
    struct arg_end* end;
} nfc_mfc_keys_args;

static struct
{
    struct arg_lit* try_b;
    struct arg_end* end;
} nfc_mfc_dump_args;

static struct
{
    struct arg_str* aid;
    struct arg_end* end;
} nfc_emv_select_args;

static struct
{
    struct arg_int* aat_a;
    struct arg_int* aat_b;
    struct arg_end* end;
} nfc_tune_set_args;

/**
 * @brief Internal helper for `print_tune_result`.
 *
 * @param[in] title Parameter passed to the function.
 * @param[in] r Parameter passed to the function.
 * @return void
 */
static void print_tune_result(const char* title,
                              const poom_nfc_tuning_result_t* r)
{
    if(title != NULL)
    {
        printf("%s\r\n", title);
    }

    printf("  aat_a = %u / 0x%02X\r\n", (unsigned)r->aat_a, (unsigned)r->aat_a);
    printf("  aat_b = %u / 0x%02X\r\n", (unsigned)r->aat_b, (unsigned)r->aat_b);
    printf("  phase = %d deg (%u / 0x%02X)\r\n", r->phase_degree,
           (unsigned)r->phase_raw, (unsigned)r->phase_raw);
    printf("  amplitude = %d mVpp (%u / 0x%02X)\r\n", r->amplitude_mvpp,
           (unsigned)r->amplitude_raw, (unsigned)r->amplitude_raw);
    if(r->measure_count > 0U)
    {
        printf("  measureCnt = %u\r\n", (unsigned)r->measure_count);
    }
}

static void poom_print_hex_compact_(const uint8_t* data, size_t len)
{
    if(data == NULL)
    {
        return;
    }

    for(size_t i = 0U; i < len; i++)
    {
        printf("%02X", (unsigned)data[i]);
    }
}

static void poom_print_hex_spaced_cli_(const uint8_t* data, size_t len)
{
    if(data == NULL)
    {
        return;
    }

    for(size_t i = 0U; i < len; i++)
    {
        printf("%02X%s", (unsigned)data[i], (i + 1U < len) ? " " : "");
    }
}

static void poom_print_ascii_or_hex_(const uint8_t* data, size_t len, bool printable)
{
    if(data == NULL)
    {
        return;
    }

    if(printable)
    {
        for(size_t i = 0U; i < len; i++)
        {
            printf("%c", (char)data[i]);
        }
        return;
    }

    poom_print_hex_compact_(data, len);
}

static const char* poom_emv_tag_name_(uint32_t tag)
{
    enum
    {
        POOM_CLI_EMV_TAG_FCI_TEMPLATE = 0x6FU,
        POOM_CLI_EMV_TAG_DF_NAME = 0x84U,
        POOM_CLI_EMV_TAG_FCI_PROPRIETARY_TEMPLATE = 0xA5U,
        POOM_CLI_EMV_TAG_FCI_ISSUER_DISCRETIONARY_DATA = 0xBF0CU,
        POOM_CLI_EMV_TAG_DIRECTORY_ENTRY = 0x61U,
        POOM_CLI_EMV_TAG_AID = 0x4FU,
        POOM_CLI_EMV_TAG_APPLICATION_LABEL = 0x50U,
        POOM_CLI_EMV_TAG_APPLICATION_PRIORITY = 0x87U,
    };

    switch(tag)
    {
        case POOM_CLI_EMV_TAG_FCI_TEMPLATE: return "FCI Template";
        case POOM_CLI_EMV_TAG_DF_NAME: return "DF Name";
        case POOM_CLI_EMV_TAG_FCI_PROPRIETARY_TEMPLATE: return "FCI Proprietary Template";
        case POOM_CLI_EMV_TAG_FCI_ISSUER_DISCRETIONARY_DATA: return "FCI Issuer Discretionary Data";
        case POOM_CLI_EMV_TAG_DIRECTORY_ENTRY: return "Directory Entry";
        case POOM_CLI_EMV_TAG_AID: return "Application Identifier";
        case POOM_CLI_EMV_TAG_APPLICATION_LABEL: return "Application Label";
        case POOM_CLI_EMV_TAG_APPLICATION_PRIORITY: return "Application Priority Indicator";
        default: return NULL;
    }
}

static void poom_emv_print_indent_(int depth)
{
    for(int i = 0; i < depth; i++)
    {
        printf("  ");
    }
}

static void poom_emv_print_tlv_tree_(const uint8_t* buf, size_t buf_len, int depth)
{
    enum
    {
        POOM_CLI_EMV_TAG_DF_NAME = 0x84U,
        POOM_CLI_EMV_TAG_APPLICATION_LABEL = 0x50U,
        POOM_CLI_EMV_TAG_APPLICATION_PRIORITY = 0x87U,
    };

    size_t off = 0U;
    poom_tlv_view_t tlv;

    while(poom_tlv_next(buf, buf_len, &off, &tlv))
    {
        char tag_hex[16];
        const char* name = poom_emv_tag_name_(tlv.tag);

        poom_tlv_format_tag(tlv.tag, tag_hex, sizeof(tag_hex));
        poom_emv_print_indent_(depth);
        printf("%s", tag_hex);
        if(name != NULL)
        {
            printf(" - %s", name);
        }
        printf("\r\n");

        if(tlv.constructed)
        {
            poom_emv_print_tlv_tree_(tlv.value, tlv.value_len, depth + 1);
            continue;
        }

        poom_emv_print_indent_(depth + 1);
        if(tlv.tag == POOM_CLI_EMV_TAG_APPLICATION_LABEL || tlv.tag == POOM_CLI_EMV_TAG_DF_NAME)
        {
            poom_print_ascii_or_hex_(tlv.value,
                                     tlv.value_len,
                                     poom_nfc_emv_label_is_printable(tlv.value, tlv.value_len));
        }
        else if(tlv.tag == POOM_CLI_EMV_TAG_APPLICATION_PRIORITY && tlv.value_len >= 1U)
        {
            printf("%u", (unsigned)tlv.value[0]);
        }
        else
        {
            poom_print_hex_compact_(tlv.value, tlv.value_len);
        }
        printf("\r\n");
    }
}

typedef struct
{
    size_t app_index;
} poom_emv_print_ctx_t;

static bool poom_emv_print_app_cb_(const poom_nfc_emv_app_t* app, void* user_ctx)
{
    poom_emv_print_ctx_t* ctx = (poom_emv_print_ctx_t*)user_ctx;

    if(app == NULL || ctx == NULL)
    {
        return false;
    }

    ctx->app_index++;
    printf("\r\nApplication %u\r\n", (unsigned)ctx->app_index);
    printf("  AID      : ");
    poom_print_hex_compact_(app->aid, app->aid_len);
    printf("\r\n");

    if(app->label != NULL && app->label_len > 0U)
    {
        printf("  Label    : ");
        poom_print_ascii_or_hex_(app->label, app->label_len, app->label_printable);
        printf("\r\n");
    }

    if(app->has_priority)
    {
        printf("  Priority : %u\r\n", (unsigned)app->priority);
    }

    return true;
}

static void poom_emv_print_status_(uint8_t sw1, uint8_t sw2)
{
    printf("\r\nStatus\r\n");
    printf("  %02X %02X - %s\r\n",
           (unsigned)sw1,
           (unsigned)sw2,
           poom_iso7816_status_desc(sw1, sw2));
}

static int cmd_nfc_emv_discover(int argc, char** argv)
{
    uint8_t rapdu[260];
    size_t rapdu_len = 0U;
    size_t app_count = 0U;
    poom_iso7816_rapdu_view_t view;
    poom_emv_print_ctx_t print_ctx = {0};
    size_t ppse_len = 0U;
    const uint8_t* ppse_name;
    uint8_t apdu[32];
    size_t apdu_len = 0U;
    bool verbose;

    (void)argc;
    (void)argv;

    if(!poom_nfc_emv_select_ppse(rapdu, sizeof(rapdu), &rapdu_len))
    {
        printf("nfc-emv-discover: ISO-DEP exchange failed. Tip: run nfc-core-start then nfc-card-connect first on an ISO-DEP payment card.\r\n");
        return 1;
    }

    if(!poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view))
    {
        printf("nfc-emv-discover: invalid R-APDU\r\n");
        return 1;
    }

    verbose = poom_reader_is_verbose();
    ppse_name = poom_nfc_emv_ppse_name(&ppse_len);
    apdu_len = poom_iso7816_build_select_df_name(ppse_name, ppse_len, apdu, sizeof(apdu));

    printf("EMV payment environment detected\r\n\r\n");
    printf("PPSE: 2PAY.SYS.DDF01\r\n");

    if(verbose)
    {
        printf("\r\nSELECT PPSE\r\n\r\n");
        printf("C-APDU:\r\n");
        poom_print_hex_spaced_cli_(apdu, apdu_len);
        printf("\r\n\r\nDecoded:\r\n\r\n");
        poom_emv_print_tlv_tree_(view.data, view.data_len, 0);
    }

    if(poom_iso7816_status_is_ok(view.sw1, view.sw2))
    {
        if(!poom_nfc_emv_parse_ppse_apps(rapdu, rapdu_len, NULL, NULL, &app_count))
        {
            printf("nfc-emv-discover: invalid PPSE TLV\r\n");
            poom_emv_print_status_(view.sw1, view.sw2);
            return 1;
        }

        printf("\r\nApplications found: %u\r\n", (unsigned)app_count);
        if(app_count > 0U)
        {
            (void)poom_nfc_emv_parse_ppse_apps(rapdu, rapdu_len, poom_emv_print_app_cb_, &print_ctx, NULL);
        }
    }
    else
    {
        printf("\r\nPPSE SELECT failed\r\n");
    }

    poom_emv_print_status_(view.sw1, view.sw2);
    return poom_iso7816_status_is_ok(view.sw1, view.sw2) ? 0 : 1;
}

static int cmd_nfc_emv_select(int argc, char** argv)
{
    uint8_t aid[32];
    size_t aid_len = 0U;
    uint8_t rapdu[260];
    size_t rapdu_len = 0U;
    poom_iso7816_rapdu_view_t view;
    uint8_t apdu[64];
    size_t apdu_len = 0U;
    bool verbose;
    int nerrors = arg_parse(argc, argv, (void**)&nfc_emv_select_args);

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_emv_select_args.end, argv[0]);
        printf("Uso: nfc-emv-select <AID>\r\n");
        printf("Ej : nfc-emv-select A0000000041010\r\n");
        return 1;
    }

    if(!poom_parse_hex_bytes(nfc_emv_select_args.aid->sval[0], aid, sizeof(aid), &aid_len) || aid_len == 0U)
    {
        printf("nfc-emv-select: invalid AID hex\r\n");
        return 1;
    }

    if(!poom_nfc_emv_select_aid(aid, aid_len, rapdu, sizeof(rapdu), &rapdu_len))
    {
        printf("nfc-emv-select: ISO-DEP exchange failed. Tip: run nfc-core-start then nfc-card-connect first on an ISO-DEP payment card.\r\n");
        return 1;
    }

    if(!poom_iso7816_parse_rapdu(rapdu, rapdu_len, &view))
    {
        printf("nfc-emv-select: invalid R-APDU\r\n");
        return 1;
    }

    verbose = poom_reader_is_verbose();
    apdu_len = poom_iso7816_build_select_df_name(aid, aid_len, apdu, sizeof(apdu));

    printf("EMV application\r\n");
    printf("  AID : ");
    poom_print_hex_compact_(aid, aid_len);
    printf("\r\n");

    if(verbose)
    {
        printf("\r\nSELECT AID\r\n\r\n");
        printf("C-APDU:\r\n");
        poom_print_hex_spaced_cli_(apdu, apdu_len);
        printf("\r\n");

        if(view.data_len > 0U)
        {
            printf("\r\nDecoded:\r\n\r\n");
            poom_emv_print_tlv_tree_(view.data, view.data_len, 0);
        }
    }

    printf("\r\nSELECT %s\r\n",
           poom_iso7816_status_is_ok(view.sw1, view.sw2) ? "OK" : "failed");
    poom_emv_print_status_(view.sw1, view.sw2);
    return poom_iso7816_status_is_ok(view.sw1, view.sw2) ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_send`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_send(int argc, char** argv)
{
    char hex_line[512] = {0};
    size_t used        = 0;

    int nerrors = arg_parse(argc, argv, (void**)&nfc_send_args);
    if(nerrors)
    {
        arg_print_errors(stderr, nfc_send_args.end, argv[0]);
        printf("Uso: nfc-card-send <hex...>\r\n");
        printf("Ej : nfc-card-send 30 04\r\n");
        return 1;
    }

    for(int i = 0; i < nfc_send_args.hex->count; i++)
    {
        const char* part = nfc_send_args.hex->sval[i];
        size_t part_len  = strlen(part);

        if(i > 0)
        {
            if((used + 1) >= sizeof(hex_line))
            {
                printf("nfc-card-send: input too long\r\n");
                return 1;
            }
            hex_line[used++] = ' ';
            hex_line[used]   = '\0';
        }

        if((used + part_len) >= sizeof(hex_line))
        {
            printf("nfc-card-send: input too long\r\n");
            return 1;
        }

        memcpy(&hex_line[used], part, part_len);
        used += part_len;
        hex_line[used] = '\0';
    }

    if(!poom_nfc_controller_send_raw_hex(hex_line))
    {
        printf("nfc-card-send failed. Tip: run nfc-card-connect first.\r\n");
        return 1;
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_clipper_history`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_clipper_history(int argc, char** argv)
{
    (void)argv;

    if(argc != 1)
    {
        printf("Uso: nfc-clipper-history-read\r\n");
        return 1;
    }

    return poom_clipper_print_history();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_stop();
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_tune_auto`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_tune_auto(int argc, char** argv)
{
    poom_nfc_tuning_result_t tr;

    (void)argc;
    (void)argv;

    if(!poom_nfc_controller_tune_auto(&tr))
    {
        return 1;
    }
    print_tune_result("nfc-tune-auto-run results:", &tr);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_tune_get`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_tune_get(int argc, char** argv)
{
    poom_nfc_tuning_result_t tr;

    (void)argc;
    (void)argv;

    if(!poom_nfc_controller_tune_get(&tr))
    {
        return 1;
    }
    print_tune_result("nfc-tune-get results:", &tr);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_tune_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_tune_set(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&nfc_tune_set_args);
    poom_nfc_tuning_result_t tr;

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_tune_set_args.end, argv[0]);
        printf("Uso: nfc-tune-set <aat_a> <aat_b>\r\n");
        return 1;
    }

    if(nfc_tune_set_args.aat_a->ival[0] < 0 ||
       nfc_tune_set_args.aat_a->ival[0] > 255 ||
       nfc_tune_set_args.aat_b->ival[0] < 0 ||
       nfc_tune_set_args.aat_b->ival[0] > 255)
    {
        printf("Invalid values. Use 0..255 for aat_a and aat_b\r\n");
        return 1;
    }

    if(!poom_nfc_controller_tune_set((uint8_t)nfc_tune_set_args.aat_a->ival[0],
                                     (uint8_t)nfc_tune_set_args.aat_b->ival[0],
                                     &tr))
    {
        return 1;
    }

    print_tune_result("nfc-tune-set results:", &tr);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_emul_show`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_show(int argc, char** argv)
{
    poom_nfc_emu_cfg_t cfg;

    (void)argc;
    (void)argv;

    poom_nfc_emulator_get_config(&cfg);

    printf("nfc-emul config:\r\n");
    printf("  running = %s\r\n", poom_nfc_emulator_is_running() ? "yes" : "no");
    printf("  mode = %s\r\n", poom_nfc_emulator_mode_to_str(cfg.mode));
    printf("  uid = ");
    if(cfg.uid_len > 0)
    {
        for(uint8_t i = 0; i < cfg.uid_len; i++)
        {
            printf("%02X", cfg.uid[i]);
        }
        printf(" (%u bytes)\r\n", cfg.uid_len);
    }
    else
    {
        printf("<not set>\r\n");
    }

    if(cfg.sak_set)
    {
        printf("  sak = %02X\r\n", cfg.sak);
    }
    else
    {
        printf("  sak = <not set>\r\n");
    }

    if(cfg.atqa_set)
    {
        printf("  atqa = %02X%02X\r\n", cfg.atqa[0], cfg.atqa[1]);
    }
    else
    {
        printf("  atqa = <auto>\r\n");
    }

    if(cfg.ats_len > 0)
    {
        printf("  ats = ");
        for(uint8_t i = 0; i < cfg.ats_len; i++)
        {
            printf("%02X", cfg.ats[i]);
        }
        printf("\r\n");
    }
    else
    {
        printf("  ats = <default>\r\n");
    }

    if(cfg.mode == POOM_NFC_EMU_MODE_MFUL)
    {
        printf("  uri = <ignored in mful mode>\r\n");
    }
    else
    {
        printf("  uri = %s\r\n", cfg.uri_set ? cfg.uri : "<not set>");
    }
    printf("  image = %s\r\n",
           cfg.image_path_set ? cfg.image_path : "<factory/default>");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_emul_reset`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_reset(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(poom_nfc_emulator_is_running())
    {
        printf("nfc-emul-reset: stop emulation first\r\n");
        return 1;
    }

    poom_nfc_emulator_reset_config();
    printf("nfc-emul: config reset\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_emul_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_set(int argc, char** argv)
{
    const char* field;
    uint8_t buf[64];
    size_t n = 0;
    char joined[160];

    if(argc < 3)
    {
        printf("Uso: nfc-emul-set <field> <value>\r\n");
        printf("Fields: mode uid sak atqa ats uri image\r\n");
        printf("  uri: use '-' to restore default\r\n");
        return 1;
    }
    if(poom_nfc_emulator_is_running())
    {
        printf("nfc-emul-set: stop emulation first\r\n");
        return 1;
    }

    field = argv[1];

    if(strcmp(field, "mode") == 0)
    {
        if(strcmp(argv[2], "3a") == 0)
        {
            return poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_3A) ? 0 : 1;
        }
        if(strcmp(argv[2], "t4t") == 0)
        {
            return poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_T4T) ? 0 : 1;
        }
        if(strcmp(argv[2], "mful") == 0)
        {
            return poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_MFUL) ? 0 : 1;
        }
        printf("Invalid mode. Use: 3a | t4t | mful\r\n");
        return 1;
    }

    if(strcmp(field, "uid") == 0)
    {
        if(!poom_parse_hex_bytes(argv[2], buf, sizeof(buf), &n) ||
           (n != 4 && n != 7))
        {
            printf("Invalid UID. Use 4 or 7 bytes hex.\r\n");
            return 1;
        }
        return poom_nfc_emulator_set_uid(buf, (uint8_t)n) ? 0 : 1;
    }

    if(strcmp(field, "sak") == 0)
    {
        if(!poom_parse_hex_bytes(argv[2], buf, sizeof(buf), &n) || n != 1)
        {
            printf("Invalid SAK. Use 1 byte hex.\r\n");
            return 1;
        }
        return poom_nfc_emulator_set_sak(buf[0]) ? 0 : 1;
    }

    if(strcmp(field, "atqa") == 0)
    {
        if(!poom_parse_hex_bytes(argv[2], buf, sizeof(buf), &n) || n != 2)
        {
            printf("Invalid ATQA. Use 2 bytes hex.\r\n");
            return 1;
        }
        return poom_nfc_emulator_set_atqa(buf) ? 0 : 1;
    }

    if(strcmp(field, "ats") == 0)
    {
        if(!poom_parse_hex_bytes(argv[2], buf, sizeof(buf), &n) || n == 0)
        {
            printf("Invalid ATS. Use ATS bytes in hex.\r\n");
            return 1;
        }
        return poom_nfc_emulator_set_ats(buf, (uint8_t)n) ? 0 : 1;
    }

    if(strcmp(field, "uri") == 0)
    {
        if(!poom_join_argv(joined, sizeof(joined), argc, argv, 2))
        {
            printf("Invalid URI\r\n");
            return 1;
        }
        if(strcmp(joined, "-") == 0)
        {
            return poom_nfc_emulator_clear_uri() ? 0 : 1;
        }
        return poom_nfc_emulator_set_uri(joined) ? 0 : 1;
    }

    if(strcmp(field, "image") == 0)
    {
        if(!poom_join_argv(joined, sizeof(joined), argc, argv, 2))
        {
            printf("Invalid image path\r\n");
            return 1;
        }
        if(!poom_normalize_emul_image_path_(joined, sizeof(joined)))
        {
            printf("Invalid image path\r\n");
            return 1;
        }
        return poom_nfc_emulator_set_mful_image_file(joined) ? 0 : 1;
    }

    printf("Unknown field '%s'. Use: mode uid sak atqa ats uri image\r\n", field);
    return 1;
}

/**
 * @brief Internal helper for `cmd_nfc_cards_list`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_cards_list(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_store_t store;
    esp_err_t st = poom_nfc_store_load(&store);
    if(st != ESP_OK)
    {
        printf("nfc-cards-list: load failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc cards saved: %u/%u\r\n",
           (unsigned)store.count,
           (unsigned)POOM_NFC_STORE_MAX_CARDS);

    for(uint8_t i = 0; i < store.count; i++)
    {
        const poom_nfc_card_id_t* id = &store.cards[i];
        printf("  [%u] %c uid=", (unsigned)i, poom_nfc_type_char(id->type));
        print_uid_hex(id->uid, id->uid_len);
        printf(" (%u)", (unsigned)id->uid_len);

        if((id->flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U)
        {
            printf(" atqa=%02X%02X", id->atqa[0], id->atqa[1]);
        }
        if((id->flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
        {
            printf(" sak=%02X", id->sak);
        }
        printf("\r\n");
    }

    return 0;
}

/**
 * @brief Internal helper for `poom_apply_profile_to_emulator`.
 *
 * @param[in] p Parameter passed to the function.
 * @param[in] reset_first Parameter passed to the function.
 * @return bool
 */
static bool poom_apply_profile_to_emulator_(const poom_nfc_profile_t* p, bool reset_first)
{
    if(p == NULL || p->uid_len == 0U)
    {
        return false;
    }

    if(reset_first)
    {
        poom_nfc_emulator_reset_config();
    }

    if(!poom_nfc_emulator_set_mode(p->mode))
    {
        return false;
    }
    if(!poom_nfc_emulator_set_uid(p->uid, p->uid_len))
    {
        return false;
    }
    if(p->atqa_set)
    {
        (void)poom_nfc_emulator_set_atqa(p->atqa);
    }
    if(p->sak_set)
    {
        (void)poom_nfc_emulator_set_sak(p->sak);
    }
    if(p->ats_len > 0U)
    {
        (void)poom_nfc_emulator_set_ats(p->ats, p->ats_len);
    }
    return true;
}

/**
 * @brief Internal helper for `cmd_nfc_emul_set_from_last_scan`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_set_from_last_scan(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(poom_nfc_emulator_is_running())
    {
        printf("nfc-emul-load-last-connect: stop emulation first\r\n");
        return 1;
    }

    poom_nfc_profile_t p;
    if(!poom_reader_get_last_profile(&p))
    {
        printf("nfc-emul-load-last-connect: no last profile (run nfc-card-connect first)\r\n");
        return 1;
    }

    if(!poom_apply_profile_to_emulator_(&p, true))
    {
        printf("nfc-emul-load-last-connect: failed\r\n");
        return 1;
    }

    printf("nfc-emul: config loaded from last connect (mode=%s uid_len=%u ats_len=%u)\r\n",
           poom_nfc_emulator_mode_to_str(p.mode),
           (unsigned)p.uid_len,
           (unsigned)p.ats_len);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_start_last(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(cmd_nfc_emul_set_from_last_scan(1, argv) != 0)
    {
        return 1;
    }

    if(!poom_nfc_controller_start())
    {
        return 1;
    }

    return poom_nfc_emulator_start() ? 0 : 1;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_card_save_current(int argc, char** argv)
{
    if(argc > 2)
    {
        printf("Uso: nfc-card-save-current [name]\r\n");
        return 1;
    }

    poom_nfc_profile_t p;
    if(!poom_reader_get_last_profile(&p))
    {
        printf("nfc-card-save-current: no last profile (run nfc-card-connect first)\r\n");
        return 1;
    }

    if(argc == 2 && argv[1] != NULL && argv[1][0] != '\0')
    {
        (void)snprintf(p.name, sizeof(p.name), "%s", argv[1]);
        p.name_set = true;
    }

    size_t added = 0U, updated = 0U, no_space = 0U;
    const esp_err_t st = poom_nfc_profile_store_add(&p, &added, &updated, &no_space);
    if(st != ESP_OK)
    {
        printf("nfc-card-save-current: save failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-card-save-current: %s%s%s\r\n",
           (added ? "added" : ""),
           (updated ? "updated" : ""),
           (no_space ? "no_space" : ""));
    return (no_space ? 1 : 0);
}

/**
 * @brief Internal helper for `poom_print_uid`.
 *
 * @param[in] uid Parameter passed to the function.
 * @param[in] uid_len Parameter passed to the function.
 * @return void
 */
static void poom_print_uid_(const uint8_t* uid, uint8_t uid_len)
{
    if(uid == NULL || uid_len == 0U)
    {
        return;
    }
    for(uint8_t i = 0; i < uid_len; i++)
    {
        printf("%02X", uid[i]);
    }
}

/**
 * @brief Internal helper for `poom_print_hex_spaced_stdout`.
 *
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return void
 */
static void poom_print_hex_spaced_stdout_(const uint8_t* data, uint8_t len)
{
    if(data == NULL || len == 0U)
    {
        return;
    }

    for(uint8_t i = 0U; i < len; i++)
    {
        if(i > 0U)
        {
            printf(" ");
        }
        printf("%02X", data[i]);
    }
}

/**
 * @brief Internal helper for `poom_print_ats_summary`.
 *
 * @param[in] ats Parameter passed to the function.
 * @param[in] ats_len Parameter passed to the function.
 * @return void
 */
static void poom_print_ats_summary_(const uint8_t* ats, uint8_t ats_len)
{
    poom_nfc_ats_info_t info;

    if(ats == NULL || ats_len == 0U)
    {
        return;
    }
    if(!poom_nfc_ats_parse(ats, ats_len, &info))
    {
        printf("      ATS summary: invalid\r\n");
        return;
    }

    printf("      ATS: ");
    poom_print_hex_spaced_stdout_(ats, info.ats_len);
    printf("\r\n");

    printf("      ATS summary: TL=%u T0=%02X FSCI=%u FSC=%u",
           (unsigned)info.tl,
           info.t0,
           (unsigned)info.fsci,
           (unsigned)info.fsc);

    if(info.ta_present)
    {
        printf(" TA=%02X", info.ta);
    }
    if(info.tb_present)
    {
        printf(" TB=%02X FWI=%u SFGI=%u",
               info.tb,
               (unsigned)info.fwi,
               (unsigned)info.sfgi);
    }
    if(info.tc_present)
    {
        printf(" TC=%02X DID=%s NAD=%s",
               info.tc,
               info.did_supported ? "yes" : "no",
               info.nad_supported ? "yes" : "no");
    }
    if(info.hb_len > 0U)
    {
        printf(" HB=");
        poom_print_hex_spaced_stdout_(info.hb, info.hb_len);
    }
    printf("\r\n");
}

/**
 * @brief Internal helper for `cmd_nfc_profiles_list`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_profiles_list(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_profile_store_t store;
    const esp_err_t st = poom_nfc_profile_store_load(&store);
    if(st != ESP_OK)
    {
        printf("nfc-profiles-list: load failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc profiles saved: %u/%u\r\n",
           (unsigned)store.count,
           (unsigned)POOM_NFC_PROFILE_STORE_MAX);

    for(uint8_t i = 0U; i < store.count; i++)
    {
        const poom_nfc_profile_t* p = &store.profiles[i];
        printf("  [%u] mode=%s uid=",
               (unsigned)i,
               poom_nfc_emulator_mode_to_str(p->mode));
        poom_print_uid_(p->uid, p->uid_len);
        printf(" (%u)", (unsigned)p->uid_len);

        if(p->name_set && p->name[0] != '\0')
        {
            printf(" name=%s", p->name);
        }
        if(p->atqa_set)
        {
            printf(" atqa=%02X%02X", p->atqa[0], p->atqa[1]);
        }
        if(p->sak_set)
        {
            printf(" sak=%02X", p->sak);
        }
        if(p->ats_len > 0U)
        {
            printf(" ats_len=%u", (unsigned)p->ats_len);
        }
        printf("\r\n");
        if(p->ats_len > 0U)
        {
            poom_print_ats_summary_(p->ats, p->ats_len);
        }
    }

    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_profiles_del`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_profiles_del(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Uso: nfc-profiles-del <index>\r\n");
        printf("Tip: use nfc-profiles-list to see indices\r\n");
        return 1;
    }

    char* end = NULL;
    long idx  = strtol(argv[1], &end, 10);
    if(end == argv[1] || end == NULL || *end != '\0' || idx < 0 || idx > 255)
    {
        printf("nfc-profiles-del: invalid index\r\n");
        return 1;
    }

    bool removed = false;
    const esp_err_t st = poom_nfc_profile_store_remove_index((uint8_t)idx, &removed);
    if(st != ESP_OK)
    {
        printf("nfc-profiles-del: failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-profiles-del: %s\r\n", removed ? "removed" : "not found");
    return removed ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_profiles_clear`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_profiles_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t st = poom_nfc_profile_store_clear();
    if(st != ESP_OK)
    {
        printf("nfc-profiles-clear: failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-profiles-clear: ok\r\n");
    return 0;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @param[in] default_timeout Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t poom_parse_timeout_ms_or_default_(int argc,
                                                 char** argv,
                                                 uint32_t default_timeout)
{
    if(argc < 2)
    {
        return default_timeout;
    }

    char* end = NULL;
    long t    = strtol(argv[1], &end, 10);
    if(end == argv[1] || end == NULL || *end != '\0' || t <= 0 || t > 60000)
    {
        return default_timeout;
    }
    return (uint32_t)t;
}

static bool poom_parse_i32_(const char* s, int32_t* out_val);

/**
 * @brief Internal helper for `cmd_nfc_cards_save`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_cards_save(int argc, char** argv)
{
    const uint32_t timeout_ms = poom_parse_timeout_ms_or_default_(argc, argv, 3000U);

    poom_nfc_card_id_t cards[8];
    size_t found = 0U;

    if(!poom_nfc_controller_scan_found_cards(timeout_ms, cards, 8U, &found) || (found == 0U))
    {
        printf("nfc-cards-save: no cards found\r\n");
        return 1;
    }

    size_t added = 0U;
    size_t already = 0U;
    size_t no_space = 0U;

    const esp_err_t st = poom_nfc_store_add_cards(cards, found, &added, &already, &no_space);
    if(st != ESP_OK)
    {
        printf("nfc-cards-save: store add failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-cards-save: found=%u added=%u already=%u no_space=%u\r\n",
           (unsigned)found,
           (unsigned)added,
           (unsigned)already,
           (unsigned)no_space);
    printf("Tip: use nfc-cards-list then nfc-emul-start <idx>\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_cards_clear`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_cards_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t st = poom_nfc_store_clear();
    if(st != ESP_OK)
    {
        printf("nfc-cards-clear: failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-cards-clear: ok\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_cards_del`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_cards_del(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Uso: nfc-cards-del <index>\r\n");
        printf("Tip: use nfc-cards-list to see indices\r\n");
        return 1;
    }

    char* end = NULL;
    long idx  = strtol(argv[1], &end, 10);
    if(end == argv[1] || end == NULL || *end != '\0' || idx < 0 || idx > 255)
    {
        printf("nfc-cards-del: invalid index\r\n");
        return 1;
    }

    bool removed = false;
    const esp_err_t st = poom_nfc_store_remove_index((uint8_t)idx, &removed);
    if(st != ESP_OK)
    {
        printf("nfc-cards-del: failed err=%d\r\n", (int)st);
        return 1;
    }

    printf("nfc-cards-del: %s\r\n", removed ? "removed" : "not found");
    return removed ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_dump_sd`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_dump_sd(int argc, char** argv)
{
    const uint32_t timeout_ms = poom_parse_timeout_ms_or_default_(argc, argv, 3000U);
    poom_nfc_dump_t dump;
    char rel_path[96];
    bool saved_mful = false;
    poom_nfc_t2t_product_t t2t_prod = POOM_NFC_T2T_PRODUCT_UNKNOWN;
    uint16_t user_bytes = 0U;
    uint16_t total_bytes = 0U;
    bool has_user_bytes = false;
    bool has_total_bytes = false;
    rel_path[0] = '\0';
    (void)memset(&dump, 0, sizeof(dump));

    if(!poom_nfc_controller_capture_dump(timeout_ms, &dump))
    {
        poom_nfc_controller_stop();
        printf("nfc-dump-save-sd: capture failed (timeout/no card)\r\n");
        return 1;
    }
    poom_nfc_controller_stop();

    t2t_prod = poom_nfc_dump_guess_t2t_product(&dump);
    has_user_bytes = poom_nfc_dump_get_t2t_user_bytes(&dump, &user_bytes);
    has_total_bytes = poom_nfc_dump_get_t2t_total_bytes(&dump, &total_bytes);

    esp_err_t err = poom_nfc_dump_save_mful_bin_to_sd(&dump, rel_path, sizeof(rel_path));
    if(err == ESP_OK)
    {
        saved_mful = true;
    }
    else if(err == ESP_ERR_INVALID_STATE)
    {
        rel_path[0] = '\0';
        err = poom_nfc_dump_save_to_sd(&dump, rel_path, sizeof(rel_path));
    }

    if(err == ESP_OK)
    {
        if(saved_mful && has_user_bytes && has_total_bytes)
        {
            printf("nfc-dump-save-sd: saved %s (emu-ready %s %u/%uB)\r\n",
                   (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/",
                   poom_nfc_t2t_product_to_str(t2t_prod),
                   (unsigned)user_bytes,
                   (unsigned)total_bytes);
        }
        else if(saved_mful)
        {
            printf("nfc-dump-save-sd: saved %s (emu-ready MFUL .nfc)\r\n",
                   (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/");
        }
        else
        {
            printf("nfc-dump-save-sd: saved %s\r\n",
                   (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/");
        }
        return 0;
    }

    printf("nfc-dump-save-sd: failed err=%d\r\n", (int)err);
    return 1;
}

/**
 * @brief Internal helper for `cmd_nfc_mful_save`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mful_save(int argc, char** argv)
{
    const uint32_t timeout_ms = poom_parse_timeout_ms_or_default_(argc, argv, 3000U);
    poom_nfc_dump_t dump;
    char rel_path[96];
    const char* mode_str;
    poom_nfc_t2t_product_t t2t_prod = POOM_NFC_T2T_PRODUCT_UNKNOWN;
    uint16_t user_bytes = 0U;
    uint16_t total_bytes = 0U;
    bool has_user_bytes = false;
    bool has_total_bytes = false;
    rel_path[0] = '\0';
    (void)memset(&dump, 0, sizeof(dump));

    if(!poom_nfc_controller_capture_dump(timeout_ms, &dump))
    {
        poom_nfc_controller_stop();
        printf("nfc-mful-save: capture failed (timeout/no card)\r\n");
        return 1;
    }

    mode_str = (dump.read_mode == POOM_NFC_DUMP_READ_FULL) ? "FULL" : "ID_ONLY";
    t2t_prod = poom_nfc_dump_guess_t2t_product(&dump);
    has_user_bytes = poom_nfc_dump_get_t2t_user_bytes(&dump, &user_bytes);
    has_total_bytes = poom_nfc_dump_get_t2t_total_bytes(&dump, &total_bytes);
    const esp_err_t err = poom_nfc_dump_save_mful_bin_to_sd(&dump, rel_path, sizeof(rel_path));
    poom_nfc_controller_stop();

    if(err == ESP_OK)
    {
        if(has_user_bytes && has_total_bytes)
        {
            printf("nfc-mful-save: saved %s (%s %u/%uB)\r\n",
                   (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/",
                   poom_nfc_t2t_product_to_str(t2t_prod),
                   (unsigned)user_bytes,
                   (unsigned)total_bytes);
        }
        else
        {
            printf("nfc-mful-save: saved %s\r\n",
                   (rel_path[0] != '\0') ? rel_path : "/nfc_dumps/");
        }
        return 0;
    }

    if(err == ESP_ERR_INVALID_STATE)
    {
        printf("nfc-mful-save: not full-readable MFUL/T2T\r\n");
        printf("  details: mode=%s read_ok=%u pages_read=%u pages_total=%u type=%c uid_len=%u\r\n",
               mode_str,
               dump.read_ok ? 1U : 0U,
               (unsigned)dump.pages_read,
               (unsigned)dump.pages_total,
               poom_nfc_type_char(dump.id.type),
               (unsigned)dump.id.uid_len);
        if((dump.id.flags & POOM_NFC_CARD_FLAG_ATQA_SET) != 0U)
        {
            printf("  atqa=%02X%02X\r\n", dump.id.atqa[1], dump.id.atqa[0]);
        }
        if((dump.id.flags & POOM_NFC_CARD_FLAG_SAK_SET) != 0U)
        {
            printf("  sak=%02X\r\n", dump.id.sak);
        }
        return 1;
    }

    if(err == ESP_ERR_TIMEOUT)
    {
        printf("nfc-mful-save: timeout (no card)\r\n");
        return 1;
    }

    printf("nfc-mful-save: failed err=%d\r\n", (int)err);
    return 1;
}

/**
 * @brief Internal helper for `cmd_nfc_isodep_dump_sd`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_isodep_dump_sd(int argc, char** argv)
{
    poom_nfc_profile_t p;
    size_t ppse_len = 0U;
    size_t gv_len = 0U;
    uint8_t gv_status = 0x00U;
    bool ppse_ok = false;
    bool gv_ok = false;
    char rel_path[96];
    esp_err_t save_err;

    rel_path[0] = '\0';
    (void)memset(&p, 0, sizeof(p));
    (void)argc;
    (void)argv;

    if(!poom_nfc_controller_start())
    {
        printf("nfc-isodep-dump-save-sd: nfc-core-start failed\r\n");
        return 1;
    }

    if(!poom_nfc_controller_connect())
    {
        poom_nfc_controller_stop();
        printf("nfc-isodep-dump-save-sd: no card/activation failed\r\n");
        return 1;
    }

    if(!poom_reader_get_last_profile(&p))
    {
        poom_nfc_controller_stop();
        printf("nfc-isodep-dump-save-sd: no profile (run nfc-card-connect path)\r\n");
        return 1;
    }

    if(p.ats_len == 0U)
    {
        poom_nfc_controller_stop();
        printf("nfc-isodep-dump-save-sd: active card is not ISO-DEP (ATS missing)\r\n");
        return 1;
    }

    ppse_ok = poom_isodep_send_apdu_capture_(
        "00A404000E325041592E5359532E444446303100",
        poom_isodep_rapdu_ppse_,
        sizeof(poom_isodep_rapdu_ppse_),
        &ppse_len);

    gv_ok = poom_clipper_desfire_cmd_collect(
        0x60,
        NULL,
        0U,
        poom_isodep_desfire_gv_,
        sizeof(poom_isodep_desfire_gv_),
        &gv_len,
        &gv_status);

    poom_nfc_controller_stop();

    save_err = poom_isodep_capture_save_sd_(
        &p,
        ppse_ok,
        poom_isodep_rapdu_ppse_,
        ppse_len,
        gv_ok,
        poom_isodep_desfire_gv_,
        gv_len,
        gv_status,
        rel_path,
        sizeof(rel_path));

    if(save_err != ESP_OK)
    {
        printf("nfc-isodep-dump-save-sd: save failed err=%d (%s)\r\n",
               (int)save_err,
               esp_err_to_name(save_err));
        return 1;
    }

    printf("nfc-isodep-dump-save-sd: saved %s\r\n", rel_path);
    printf("  ppse=%s desfire_get_version=%s\r\n",
           ppse_ok ? "ok" : "fail",
           gv_ok ? "ok" : "fail");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_tech`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_tech(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_reader_verbose`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_reader_verbose(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Uso: nfc-reader-verbose-set <0|1>\r\n");
        return 1;
    }

    int32_t v = 0;
    if(!poom_parse_i32_(argv[1], &v) || ((v != 0) && (v != 1)))
    {
        printf("nfc-reader-verbose-set: invalid value\r\n");
        return 1;
    }

    poom_nfc_debug_reader_set_verbose(v == 1);
    printf("nfc-reader-verbose-set: %s\r\n", (v == 1) ? "on" : "off");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_iso_chunk`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_iso_chunk(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Uso: nfc-isodep-chunk-set <1..250>\r\n");
        return 1;
    }

    int32_t v = 0;
    if(!poom_parse_i32_(argv[1], &v) || (v < 1) || (v > 250))
    {
        printf("nfc-isodep-chunk-set: invalid value\r\n");
        return 1;
    }

    if(!poom_nfc_debug_reader_set_iso_dep_chunk_len((uint8_t)v))
    {
        printf("nfc-isodep-chunk-set: rejected\r\n");
        return 1;
    }

    printf("nfc-isodep-chunk-set: ok (%ld)\r\n", (long)v);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_scan_loop`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_scan_loop(int argc, char** argv)
{
    if((argc < 3) || (argc > 4))
    {
        printf("Uso: nfc-scan-run-loop <period_ms> <count> [timeout_ms]\r\n");
        return 1;
    }

    int32_t period_ms = 0;
    int32_t count = 0;
    int32_t timeout_ms = 3000;

    if(!poom_parse_i32_(argv[1], &period_ms) || (period_ms <= 0) || (period_ms > 60000))
    {
        printf("nfc-scan-run-loop: invalid period_ms\r\n");
        return 1;
    }
    if(!poom_parse_i32_(argv[2], &count) || (count <= 0) || (count > 1000000))
    {
        printf("nfc-scan-run-loop: invalid count\r\n");
        return 1;
    }
    if(argc == 4)
    {
        if(!poom_parse_i32_(argv[3], &timeout_ms) || (timeout_ms <= 0) || (timeout_ms > 60000))
        {
            printf("nfc-scan-run-loop: invalid timeout_ms\r\n");
            return 1;
        }
    }

    printf("nfc-scan-run-loop: period=%ldms count=%ld timeout=%ldms\r\n",
           (long)period_ms, (long)count, (long)timeout_ms);

    return poom_nfc_debug_scan_loop((uint32_t)period_ms, (uint32_t)timeout_ms, (uint32_t)count) ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_rf_on`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_rf_on(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(!poom_nfc_debug_rf_on())
    {
        return 1;
    }
    printf("nfc-rf-turn-on: ok\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_rf_off`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_rf_off(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(!poom_nfc_debug_rf_off())
    {
        return 1;
    }
    printf("nfc-rf-turn-off: ok\r\n");
    return 0;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] s Parameter passed to the function.
 * @param[in] out_val Parameter passed to the function.
 * @return bool
 */
static bool poom_parse_i32_(const char* s, int32_t* out_val)
{
    if((s == NULL) || (out_val == NULL))
    {
        return false;
    }

    char* end = NULL;
    long v    = strtol(s, &end, 0);
    if((end == s) || (end == NULL) || (*end != '\0'))
    {
        return false;
    }
    *out_val = (int32_t)v;
    return true;
}

/**
 * @brief Internal helper for `cmd_nfc_mode_get`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mode_get(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_debug_mode_info_t info;
    if(!poom_nfc_debug_mode_get(&info))
    {
        return 1;
    }

    printf("nfc-mode-get:\r\n");
    printf("  mode = %d\r\n", (int)info.mode);
    printf("  bitrate rc = %d\r\n", (int)info.bitrate_rc);
    printf("  tx_br = %d\r\n", (int)info.tx_br);
    printf("  rx_br = %d\r\n", (int)info.rx_br);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mode_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mode_set(int argc, char** argv)
{
    int32_t mode_i = 0;
    int32_t tx_i   = 0;
    int32_t rx_i   = 0;

    if(argc != 4)
    {
        printf("Uso: nfc-mode-set <mode> <tx_br> <rx_br>\r\n");
        printf("Example: nfc-mode-set 1 0 0  (POLL_NFCA @106)\r\n");
        return 1;
    }

    if(!poom_parse_i32_(argv[1], &mode_i) ||
       !poom_parse_i32_(argv[2], &tx_i) ||
       !poom_parse_i32_(argv[3], &rx_i))
    {
        printf("nfc-mode-set: invalid args\r\n");
        return 1;
    }

    const ReturnCode rc = poom_nfc_debug_mode_set((rfalMode)mode_i,
                                                  (rfalBitRate)tx_i,
                                                  (rfalBitRate)rx_i);
    printf("nfc-mode-set: rc=%d\r\n", (int)rc);
    return (rc == RFAL_ERR_NONE) ? 0 : 1;
}

/**
 * @brief Internal helper for `cmd_nfc_obsv_get`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_obsv_get(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_debug_obsv_info_t info;
    if(!poom_nfc_debug_obsv_get(&info))
    {
        return 1;
    }
    printf("nfc-obsv-get: tx=%u (0x%02X) rx=%u (0x%02X)\r\n",
           (unsigned)info.tx, (unsigned)info.tx, (unsigned)info.rx, (unsigned)info.rx);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_obsv_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_obsv_set(int argc, char** argv)
{
    int32_t on_i = 0;
    int32_t tx_i = 0;
    int32_t rx_i = 0;

    if((argc != 2) && (argc != 4))
    {
        printf("Uso: nfc-obsv-set <0|1> [tx] [rx]\r\n");
        return 1;
    }

    if(!poom_parse_i32_(argv[1], &on_i) || ((on_i != 0) && (on_i != 1)))
    {
        printf("nfc-obsv-set: invalid on value\r\n");
        return 1;
    }

    if(argc == 4)
    {
        if(!poom_parse_i32_(argv[2], &tx_i) ||
           !poom_parse_i32_(argv[3], &rx_i) ||
           (tx_i < 0) || (tx_i > 255) ||
           (rx_i < 0) || (rx_i > 255))
        {
            printf("nfc-obsv-set: invalid tx/rx (0..255)\r\n");
            return 1;
        }
    }

    if(!poom_nfc_debug_obsv_set_enabled(on_i == 1, (uint8_t)tx_i, (uint8_t)rx_i))
    {
        return 1;
    }

    if(on_i == 0)
    {
        printf("nfc-obsv-set: disabled\r\n");
        return 0;
    }

    printf("nfc-obsv-set: enabled tx=%u rx=%u\r\n", (unsigned)tx_i, (unsigned)rx_i);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_reqa`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_reqa(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_debug_atqa_t atqa;
    const ReturnCode rc = poom_nfc_debug_req_a(&atqa);
    if(rc != RFAL_ERR_NONE)
    {
        printf("nfc-reqa-send: err=%d (%s)\r\n",
               (int)rc,
               poom_nfc_debug_return_code_to_str(rc));
        return 1;
    }

    printf("nfc-reqa-send: ATQA=%02X%02X\r\n",
           (unsigned)atqa.atqa[0],
           (unsigned)atqa.atqa[1]);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_wupa`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_wupa(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_nfc_debug_atqa_t atqa;
    const ReturnCode rc = poom_nfc_debug_wup_a(&atqa);
    if(rc != RFAL_ERR_NONE)
    {
        printf("nfc-wupa-send: err=%d (%s)\r\n",
               (int)rc,
               poom_nfc_debug_return_code_to_str(rc));
        return 1;
    }

    printf("nfc-wupa-send: ATQA=%02X%02X\r\n",
           (unsigned)atqa.atqa[0],
           (unsigned)atqa.atqa[1]);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_raw_txrx`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_raw_txrx(int argc, char** argv)
{
    bool crc_auto = true;
    char joined[512];
    uint8_t tx[256];
    size_t tx_len = 0;
    uint8_t rx[300];
    size_t rx_len = 0;
    ReturnCode rc;

    if(argc < 2)
    {
        printf("Uso: nfc-raw-transceive <hex...>\r\n");
        printf("Uso: nfc-raw-transceive --crc-manual <hex...>\r\n");
        return 1;
    }

    int start = 1;
    if(strcmp(argv[1], "--crc-manual") == 0)
    {
        crc_auto = false;
        start = 2;
        if(argc < 3)
        {
            printf("nfc-raw-transceive: missing hex bytes\r\n");
            return 1;
        }
    }

    if(!poom_join_argv(joined, sizeof(joined), argc, argv, start))
    {
        printf("nfc-raw-transceive: input too long\r\n");
        return 1;
    }

    if(!poom_parse_hex_bytes(joined, tx, sizeof(tx), &tx_len) || (tx_len == 0U))
    {
        printf("nfc-raw-transceive: invalid hex input\r\n");
        return 1;
    }

    rc = poom_nfc_debug_raw_txrx(tx, tx_len, crc_auto, rx, sizeof(rx), &rx_len);

    if((rc != RFAL_ERR_NONE) && !(rc == RFAL_ERR_CRC))
    {
        printf("nfc-raw-transceive: err=%d\r\n", (int)rc);
        return 1;
    }

    printf("nfc-raw-transceive: rc=%d rx_len=%u rx=",
           (int)rc,
           (unsigned)rx_len);
    for(size_t i = 0; i < rx_len; i++)
    {
        printf("%02X", rx[i]);
    }
    printf("\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_regdump`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_regdump(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    t_st25r3916Regs dump;
    const ReturnCode rc = poom_nfc_debug_regdump(&dump);
    if(rc != RFAL_ERR_NONE)
    {
        printf("nfc-regs-dump: st25r3916GetRegsDump err=%d\r\n", (int)rc);
        return 1;
    }

    printf("nfc-regs-dump: ST25R3916 regs A (0..0x%02X)\r\n",
           (unsigned)ST25R3916_REG_IC_IDENTITY);
    for(unsigned i = 0; i < (unsigned)(ST25R3916_REG_IC_IDENTITY + 1U); i++)
    {
        printf("  0x%02X: 0x%02X\r\n", i, dump.RsA[i]);
    }

    size_t map_len = 0U;
    const uint8_t* map = poom_nfc_debug_st25_space_b_addr_map(&map_len);

    printf("nfc-regs-dump: ST25R3916 regs B (0..0x%02X)\r\n",
           (unsigned)(ST25R3916_SPACE_B_REG_LEN - 1U));
    for(unsigned i = 0; i < (unsigned)ST25R3916_SPACE_B_REG_LEN; i++)
    {
        const unsigned addr = (map != NULL && i < map_len) ? (unsigned)map[i] : i;
        printf("  0x%02X: 0x%02X\r\n", addr, dump.RsB[i]);
    }

    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_start(int argc, char** argv)
{
    if(poom_nfc_emulator_is_running())
    {
        printf("nfc-emul-start: stop emulation first\r\n");
        return 1;
    }

    if(argc == 2)
    {
        char* end = NULL;
        long idx  = strtol(argv[1], &end, 10);
        if(end == argv[1] || end == NULL || *end != '\0' || idx < 0 ||
           idx > 255)
        {
            printf("Uso: nfc-emul-start [stored_index]\r\n");
            printf("Tip: use nfc-cards-list to see indices\r\n");
            return 1;
        }

        {
            poom_nfc_profile_store_t ps;
            const esp_err_t stp = poom_nfc_profile_store_load(&ps);
            if(stp == ESP_OK && (uint8_t)idx < ps.count)
            {
                const poom_nfc_profile_t* p = &ps.profiles[(uint8_t)idx];
                if(!poom_apply_profile_to_emulator_(p, true))
                {
                    printf("nfc-emul-start: failed to apply profile index %ld\r\n", idx);
                    return 1;
                }
                goto start_emulation;
            }
        }

        poom_nfc_store_t store;
        esp_err_t st = poom_nfc_store_load(&store);
        if(st != ESP_OK)
        {
            printf("nfc-emul-start: load saved cards failed err=%d\r\n",
                   (int)st);
            return 1;
        }
        if((uint8_t)idx >= store.count)
        {
            printf("nfc-emul-start: invalid index %ld (0..%u)\r\n",
                   idx,
                   (store.count > 0) ? (unsigned)(store.count - 1U) : 0U);
            return 1;
        }

        const poom_nfc_card_id_t* id = &store.cards[(uint8_t)idx];
        if(!((id->type == 0U) || (id->type == 10U)) ||
           (id->uid_len != 4 && id->uid_len != 7))
        {
            printf("nfc-emul-start: unsupported saved tag (need NFC-A UID 4/7)\r\n");
            printf("  type=%c uid_len=%u\r\n",
                   poom_nfc_type_char(id->type),
                   (unsigned)id->uid_len);
            return 1;
        }
        if(((id->flags & POOM_NFC_CARD_FLAG_ATQA_SET) == 0U) ||
           ((id->flags & POOM_NFC_CARD_FLAG_SAK_SET) == 0U))
        {
            printf("nfc-emul-start: saved card missing ATQA/SAK (rescan + save)\r\n");
            return 1;
        }

        poom_nfc_emulator_reset_config();
        (void)poom_nfc_emulator_set_mode(POOM_NFC_EMU_MODE_3A);
        if(!poom_nfc_emulator_set_uid(id->uid, id->uid_len))
        {
            printf("nfc-emul-start: failed to set UID\r\n");
            return 1;
        }
        (void)poom_nfc_emulator_set_atqa(id->atqa);
        (void)poom_nfc_emulator_set_sak(id->sak);
    }
    else if(argc != 1)
    {
        printf("Uso: nfc-emul-start [stored_index]\r\n");
        printf("Tip: use nfc-cards-list to see indices\r\n");
        return 1;
    }

start_emulation:
    if(!poom_nfc_controller_start())
    {
        return 1;
    }
    return poom_nfc_emulator_start() ? 0 : 1;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_emul_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_emulator_stop();
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_discover`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_discover(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_discover_args);
    bool try_b;

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_discover_args.end, argv[0]);
        printf("Uso: nfc-mfc-discover [-b]\r\n");
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    try_b = (nfc_mfc_discover_args.try_b->count > 0);

    if(!poom_mifare_classic_discover_default_keys(try_b))
    {
        printf("  mifare discover: no keys found. Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_auth`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @param[in] key_type Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_auth_(int argc, char** argv, poom_mifare_key_type_t key_type)
{
    uint8_t key[6];
    size_t key_len = 0U;
    int block;
    int sector;
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_auth_args);

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_auth_args.end, argv[0]);
        printf("Uso: %s <block> <key>\r\n", argv[0]);
        printf("Ej : %s 4 FFFFFFFFFFFF\r\n", argv[0]);
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    block = nfc_mfc_auth_args.block->ival[0];
    if(block < 0 || block > 255)
    {
        printf("Invalid block. Use 0..255\r\n");
        return 1;
    }

    if(!poom_parse_hex_bytes(nfc_mfc_auth_args.key->sval[0], key, sizeof(key), &key_len) || key_len != 6U)
    {
        printf("Invalid key. Provide exactly 6 bytes (12 hex chars).\r\n");
        return 1;
    }

    if(!poom_mifare_classic_auth((uint8_t)block, key_type, key))
    {
        poom_mifare_auth_status_t st = poom_mifare_classic_get_last_auth_status();
        printf("  mifare auth %c failed (status=%s).\r\n",
               (key_type == POOM_MIFARE_KEY_A) ? 'A' : 'B',
               poom_mifare_auth_status_str_(st));
        if(st == POOM_MIFARE_AUTH_STATUS_PARTIAL)
        {
            printf("  Tip: step1 responded but Crypto1 step2 did not complete.\r\n");
        }
        else
        {
            printf("  Tip: verify the key, then retry. Use nfc-mfc-discover only if you want explicit dictionary probing.\r\n");
        }
        return 1;
    }

    sector = poom_mifare_classic_sector_for_block((uint8_t)block);
    printf("  mifare auth %c OK for block %d", (key_type == POOM_MIFARE_KEY_A) ? 'A' : 'B', block);
    if(sector >= 0)
    {
        printf(" (sector %d)", sector);
    }
    printf("\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_auth_a`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_auth_a(int argc, char** argv)
{
    return cmd_nfc_mfc_auth_(argc, argv, POOM_MIFARE_KEY_A);
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_auth_b`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_auth_b(int argc, char** argv)
{
    return cmd_nfc_mfc_auth_(argc, argv, POOM_MIFARE_KEY_B);
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_read`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_read(int argc, char** argv)
{
    uint8_t data[16];
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_read_args);

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_read_args.end, argv[0]);
        printf("Uso: nfc-mfc-read <block>\r\n");
        printf("Ej : nfc-mfc-read 4\r\n");
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    int block = nfc_mfc_read_args.block->ival[0];
    if(block < 0 || block > 255)
    {
        printf("Invalid block. Use 0..255\r\n");
        return 1;
    }

    if(!poom_mifare_classic_read_block((uint8_t)block, data))
    {
        printf("  mifare read failed. Tip: run nfc-core-start then nfc-card-connect first; then nfc-mfc-auth-a/b or nfc-mfc-discover.\r\n");
        return 1;
    }

    printf("  block %u:", (unsigned)block);
    for(int i = 0; i < 16; i++)
    {
        printf(" %02X", data[i]);
    }
    printf("\r\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_write`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_write(int argc, char** argv)
{
    char hex_line[512] = {0};
    size_t used        = 0;
    uint8_t data[16];
    size_t data_len = 0;

    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_write_args);
    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_write_args.end, argv[0]);
        printf("Uso: nfc-mfc-write <block> <hex...>\r\n");
        printf("Ej : nfc-mfc-write 4 00112233445566778899AABBCCDDEEFF\r\n");
        printf("Ej : nfc-mfc-write 4 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF\r\n");
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    int block = nfc_mfc_write_args.block->ival[0];
    if(block < 0 || block > 255)
    {
        printf("Invalid block. Use 0..255\r\n");
        return 1;
    }

    for(int i = 0; i < nfc_mfc_write_args.hex->count; i++)
    {
        const char* part = nfc_mfc_write_args.hex->sval[i];
        size_t part_len  = strlen(part);

        if(i > 0)
        {
            if((used + 1U) >= sizeof(hex_line))
            {
                printf("nfc-mfc-write: input too long\r\n");
                return 1;
            }
            hex_line[used++] = ' ';
            hex_line[used]   = '\0';
        }

        if((used + part_len) >= sizeof(hex_line))
        {
            printf("nfc-mfc-write: input too long\r\n");
            return 1;
        }

        memcpy(&hex_line[used], part, part_len);
        used += part_len;
        hex_line[used] = '\0';
    }

    if(!poom_parse_hex_bytes(hex_line, data, sizeof(data), &data_len) || data_len != 16U)
    {
        printf("Invalid data. Provide exactly 16 bytes (32 hex chars).\r\n");
        return 1;
    }

    if(!poom_mifare_classic_write_block((uint8_t)block, data))
    {
        printf("  mifare write failed. Tip: run nfc-core-start then nfc-card-connect first; then nfc-mfc-auth-a/b or nfc-mfc-discover.\r\n");
        return 1;
    }

    printf("  block %u written OK\r\n", (unsigned)block);
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_keys`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_keys(int argc, char** argv)
{
    uint8_t key[6];
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_keys_args);
    int block;
    int sector;
    bool has_any = false;

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_keys_args.end, argv[0]);
        printf("Uso: nfc-mfc-keys <block>\r\n");
        printf("Ej : nfc-mfc-keys 4\r\n");
        printf("Nota: MIFARE Classic usa Key A/Key B por sector, no password por bloque.\r\n");
        return 1;
    }

    block = nfc_mfc_keys_args.block->ival[0];
    if(block < 0 || block > 255)
    {
        printf("Invalid block. Use 0..255\r\n");
        return 1;
    }

    sector = poom_mifare_classic_sector_for_block((uint8_t)block);
    if(sector < 0)
    {
        printf("  mifare keys: unknown sector for block %d. Tip: run nfc-core-start then nfc-card-connect first.\r\n", block);
        return 1;
    }

    printf("  block %d belongs to sector %d\r\n", block, sector);

    if(poom_mifare_classic_get_sector_key((uint8_t)sector, POOM_MIFARE_KEY_A, key))
    {
        printf("    keyA=");
        print_key6(key);
        printf("\r\n");
        has_any = true;
    }
    else
    {
        printf("    keyA=<unknown>\r\n");
    }

    if(poom_mifare_classic_get_sector_key((uint8_t)sector, POOM_MIFARE_KEY_B, key))
    {
        printf("    keyB=");
        print_key6(key);
        printf("\r\n");
        has_any = true;
    }
    else
    {
        printf("    keyB=<unknown>\r\n");
    }

    if(!has_any)
    {
        printf("  Tip: use nfc-mfc-auth-a/b or nfc-mfc-discover to cache the sector keys.\r\n");
        return 1;
    }

    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_dump`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_dump(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_dump_args);
    bool try_b;
    char path[160];

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_dump_args.end, argv[0]);
        printf("Uso: nfc-mfc-dump [-b]\r\n");
        printf("Ej : nfc-mfc-dump -b\r\n");
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    try_b   = (nfc_mfc_dump_args.try_b->count > 0);

    path[0] = '\0';
    if(!poom_mifare_classic_dump_to_flipper_file("/nfc", try_b, path, sizeof(path)))
    {
        printf("  mifare dump failed. Tip: ensure SD mounted; run nfc-core-start then nfc-card-connect first; then nfc-mfc-discover.\r\n");
        return 1;
    }

    printf("  dump saved: %s\r\n", (path[0] != '\0') ? path : "<unknown>");
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_mfc_dump_poom`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_mfc_dump_poom(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&nfc_mfc_dump_args);
    bool try_b;
    char path[160];

    if(nerrors)
    {
        arg_print_errors(stderr, nfc_mfc_dump_args.end, argv[0]);
        printf("Uso: nfc-mfc-dump-poom [-b]\r\n");
        printf("Ej : nfc-mfc-dump-poom -b\r\n");
        printf("Tip: run nfc-core-start then nfc-card-connect first.\r\n");
        return 1;
    }

    try_b   = (nfc_mfc_dump_args.try_b->count > 0);

    path[0] = '\0';
    if(!poom_mifare_classic_dump_to_poom_memory_file("/nfc", try_b, path, sizeof(path)))
    {
        printf("  mifare dump failed. Tip: ensure SD mounted; run nfc-core-start then nfc-card-connect first; then nfc-mfc-discover.\r\n");
        return 1;
    }

    printf("  dump saved: %s\r\n", (path[0] != '\0') ? path : "<unknown>");
    return 0;
}

/* tech handlers */

/**
 * @brief Internal helper for `cmd_nfc_all`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_all(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_ALL);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_a`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_a(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_A);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_b`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_b(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_B);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_f`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_f(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_F);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_v`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_v(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_V);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `cmd_nfc_st25tb`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_nfc_st25tb(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    poom_nfc_controller_set_technology(POOM_NFC_CTRL_TECH_ST25TB);
    printf("NFC tech selected: %s\r\n",
           poom_nfc_controller_technology_to_str(
               poom_nfc_controller_get_technology()));
    return 0;
}

/**
 * @brief Internal helper for `register_poom_nfc_cmds`.
 *
 * @return void
 */
static void register_poom_nfc_cmds(void)
{
    const esp_console_cmd_t nfc_start = {
        .command = "nfc-core-start",
        .help    = "Initialize NFC core (I2C + IRQ + RFAL). Call once.",
        .hint    = NULL,
        .func    = &cmd_nfc_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_start));

    const esp_console_cmd_t nfc_scan = {
        .command = "nfc-scan-run",
        .help = "Scan selected technology (NFC-A/B/V/F/ST25TB) and list tags.",
        .hint = NULL,
        .func = &cmd_nfc_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_scan));

    const esp_console_cmd_t nfc_connect = {
        .command = "nfc-card-connect",
        .help    = "Activate a card (A/B/15693).",
        .hint    = NULL,
        .func    = &cmd_nfc_connect,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_connect));

    nfc_send_args.hex =
        arg_strn(NULL, NULL, "<hex>", 1, 64, "hex bytes, e.g. 30 04 or 60");
    nfc_send_args.end = arg_end(1);

    const esp_console_cmd_t nfc_send = {
        .command = "nfc-card-send",
        .help = "Send raw hex bytes to the active tag. Example: nfc-card-send 30 04",
        .hint = NULL,
        .func = &cmd_nfc_send,
        .argtable = &nfc_send_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_send));

    const esp_console_cmd_t nfc_clipper_history = {
        .command = "nfc-clipper-history-read",
        .help    = "Read Clipper DESFire files and print parsed ride history.",
        .hint    = NULL,
        .func    = &cmd_nfc_clipper_history,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_clipper_history));

    nfc_emv_select_args.aid = arg_str1(NULL, NULL, "<AID>", "EMV application AID in hex");
    nfc_emv_select_args.end = arg_end(1);

    const esp_console_cmd_t nfc_emv_discover = {
        .command  = "nfc-emv-discover",
        .help     = "Select PPSE and list advertised EMV payment applications.",
        .hint     = NULL,
        .func     = &cmd_nfc_emv_discover,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emv_discover));

    const esp_console_cmd_t nfc_emv_select = {
        .command  = "nfc-emv-select",
        .help     = "Select one EMV application by AID. Example: nfc-emv-select A0000000041010",
        .hint     = NULL,
        .func     = &cmd_nfc_emv_select,
        .argtable = &nfc_emv_select_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emv_select));

    nfc_mfc_discover_args.try_b = arg_lit0("b", "try-b", "also try Key B per sector");
    nfc_mfc_discover_args.end   = arg_end(1);

    const esp_console_cmd_t nfc_mfc_discover = {
        .command  = "nfc-mfc-discover",
        .help     = "Discover MIFARE Classic sector keys using the built-in dictionary.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_discover,
        .argtable = &nfc_mfc_discover_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_discover));

    nfc_mfc_auth_args.block = arg_int1(NULL, NULL, "<block>", "block number (0..255)");
    nfc_mfc_auth_args.key   = arg_str1(NULL, NULL, "<key>", "6-byte key in hex");
    nfc_mfc_auth_args.end   = arg_end(2);

    const esp_console_cmd_t nfc_mfc_auth_a = {
        .command  = "nfc-mfc-auth-a",
        .help     = "Authenticate a MIFARE Classic sector using Key A for the given block.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_auth_a,
        .argtable = &nfc_mfc_auth_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_auth_a));

    const esp_console_cmd_t nfc_mfc_auth_b = {
        .command  = "nfc-mfc-auth-b",
        .help     = "Authenticate a MIFARE Classic sector using Key B for the given block.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_auth_b,
        .argtable = &nfc_mfc_auth_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_auth_b));

    nfc_mfc_read_args.block = arg_int1(NULL, NULL, "<block>", "block number (0..255)");
    nfc_mfc_read_args.end   = arg_end(1);

    const esp_console_cmd_t nfc_mfc_read = {
        .command  = "nfc-mfc-read",
        .help     = "Read a MIFARE Classic block using cached or discovered sector keys.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_read,
        .argtable = &nfc_mfc_read_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_read));

    nfc_mfc_write_args.block = arg_int1(NULL, NULL, "<block>", "block number (0..255)");
    nfc_mfc_write_args.hex =
        arg_strn(NULL, NULL, "<hex>", 1, 64, "16-byte hex payload (e.g. 00 11 ... or 0011...)");
    nfc_mfc_write_args.end = arg_end(2);

    const esp_console_cmd_t nfc_mfc_write = {
        .command  = "nfc-mfc-write",
        .help     = "Write a MIFARE Classic block using cached or discovered sector keys.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_write,
        .argtable = &nfc_mfc_write_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_write));

    nfc_mfc_keys_args.block = arg_int1(NULL, NULL, "<block>", "block number (0..255)");
    nfc_mfc_keys_args.end   = arg_end(1);

    const esp_console_cmd_t nfc_mfc_keys = {
        .command  = "nfc-mfc-keys",
        .help     = "Show cached Key A/Key B for the sector that owns the given block.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_keys,
        .argtable = &nfc_mfc_keys_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_keys));

    nfc_mfc_dump_args.try_b = arg_lit0("b", "try-b", "also try Key B discovery");
    nfc_mfc_dump_args.end   = arg_end(1);

    const esp_console_cmd_t nfc_mfc_dump = {
        .command  = "nfc-mfc-dump",
        .help     = "Dump MIFARE Classic as a Flipper .nfc file to /nfc on SD.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_dump,
        .argtable = &nfc_mfc_dump_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_dump));

    const esp_console_cmd_t nfc_mfc_dump_poom = {
        .command  = "nfc-mfc-dump-poom",
        .help     = "Dump MIFARE Classic as a POOM memory image (.nfc) to /nfc on SD.",
        .hint     = NULL,
        .func     = &cmd_nfc_mfc_dump_poom,
        .argtable = &nfc_mfc_dump_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mfc_dump_poom));

    const esp_console_cmd_t nfc_stop = {
        .command = "nfc-core-stop",
        .help    = "Deactivate NFC core / RF field (optional).",
        .hint    = NULL,
        .func    = &cmd_nfc_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_stop));

    const esp_console_cmd_t nfc_emul_show = {
        .command = "nfc-emul-show",
        .help    = "Show current NFC emulation configuration.",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_show));

    const esp_console_cmd_t nfc_emul_reset = {
        .command = "nfc-emul-reset",
        .help    = "Reset NFC emulation config to defaults.",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_reset));

    const esp_console_cmd_t nfc_emul_set = {
        .command = "nfc-emul-set",
        .help = "Set emulation field: mode|uid|sak|atqa|ats|uri|image. "
                "For image prefer Flipper .nfc (bin still supported). URI can be '-' to restore default.",
        .hint = NULL,
        .func = &cmd_nfc_emul_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_set));

    const esp_console_cmd_t nfc_emul_set_last = {
        .command = "nfc-emul-load-last-connect",
        .help    = "Copy last nfc-card-connect activation snapshot into emulation config (UID/ATQA/SAK/ATS/mode).",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_set_from_last_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_set_last));

    const esp_console_cmd_t nfc_emul_start_last = {
        .command = "nfc-emul-start-last-connect",
        .help    = "Equivalent to: nfc-emul-load-last-connect + nfc-emul-start",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_start_last,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_start_last));

    const esp_console_cmd_t nfc_cards_list = {
        .command = "nfc-cards-list",
        .help    = "List NFC card IDs saved in NVS (used by menu NFC).",
        .hint    = NULL,
        .func    = &cmd_nfc_cards_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_cards_list));

    const esp_console_cmd_t nfc_card_save_current = {
        .command = "nfc-card-save-current",
        .help    = "Save last nfc-card-connect activation profile to NVS (mode+UID+ATQA+SAK+ATS). Optional name.",
        .hint    = NULL,
        .func    = &cmd_nfc_card_save_current,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_card_save_current));

    const esp_console_cmd_t nfc_profiles_list = {
        .command = "nfc-profiles-list",
        .help    = "List activation profiles saved in NVS (mode+UID+ATQA+SAK+ATS).",
        .hint    = NULL,
        .func    = &cmd_nfc_profiles_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_profiles_list));

    const esp_console_cmd_t nfc_profiles_del = {
        .command = "nfc-profiles-del",
        .help    = "Delete a saved activation profile by index.",
        .hint    = NULL,
        .func    = &cmd_nfc_profiles_del,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_profiles_del));

    const esp_console_cmd_t nfc_profiles_clear = {
        .command = "nfc-profiles-clear",
        .help    = "Clear all saved activation profiles.",
        .hint    = NULL,
        .func    = &cmd_nfc_profiles_clear,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_profiles_clear));

    const esp_console_cmd_t nfc_cards_save = {
        .command = "nfc-cards-save",
        .help = "Scan and save nearby card IDs into NVS. Optional: nfc-cards-save <timeout_ms>",
        .hint = NULL,
        .func = &cmd_nfc_cards_save,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_cards_save));

    const esp_console_cmd_t nfc_cards_del = {
        .command = "nfc-cards-del",
        .help    = "Delete a saved card by index (see nfc-cards-list).",
        .hint    = NULL,
        .func    = &cmd_nfc_cards_del,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_cards_del));

    const esp_console_cmd_t nfc_cards_clear = {
        .command = "nfc-cards-clear",
        .help    = "Clear all saved card IDs from NVS.",
        .hint    = NULL,
        .func    = &cmd_nfc_cards_clear,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_cards_clear));

    const esp_console_cmd_t nfc_dump_sd = {
        .command = "nfc-dump-save-sd",
        .help    = "Scan and save a structured dump to SD. Optional: nfc-dump-save-sd <timeout_ms>",
        .hint    = NULL,
        .func    = &cmd_nfc_dump_sd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_dump_sd));

    const esp_console_cmd_t nfc_mful_save = {
        .command = "nfc-mful-save",
        .help    = "Scan and save MFUL/Type2 structured .nfc to /nfc_dumps. Optional: nfc-mful-save <timeout_ms>",
        .hint    = NULL,
        .func    = &cmd_nfc_mful_save,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mful_save));

    const esp_console_cmd_t nfc_isodep_dump_sd = {
        .command = "nfc-isodep-dump-save-sd",
        .help    = "Connect ISO-DEP card and save APDU capture to SD (/nfc_dumps).",
        .hint    = NULL,
        .func    = &cmd_nfc_isodep_dump_sd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_isodep_dump_sd));

    const esp_console_cmd_t nfc_tech = {
        .command = "nfc-tech-show",
        .help    = "Show currently selected NFC technology filter.",
        .hint    = NULL,
        .func    = &cmd_nfc_tech,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_tech));

    const esp_console_cmd_t nfc_reader_verbose = {
        .command = "nfc-reader-verbose-set",
        .help    = "Enable/disable reader verbose TX/RX prints (ISO-DEP bridge).",
        .hint    = NULL,
        .func    = &cmd_nfc_reader_verbose,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_reader_verbose));

    const esp_console_cmd_t nfc_iso_chunk = {
        .command = "nfc-isodep-chunk-set",
        .help    = "Set ISO-DEP chunk size (1..250) for APDU chaining.",
        .hint    = NULL,
        .func    = &cmd_nfc_iso_chunk,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_iso_chunk));

    const esp_console_cmd_t nfc_scan_loop = {
        .command = "nfc-scan-run-loop",
        .help    = "Run repeated scans: nfc-scan-run-loop <period_ms> <count> [timeout_ms].",
        .hint    = NULL,
        .func    = &cmd_nfc_scan_loop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_scan_loop));

    const esp_console_cmd_t nfc_rf_on = {
        .command = "nfc-rf-turn-on",
        .help    = "Force RF field ON (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_rf_on,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_rf_on));

    const esp_console_cmd_t nfc_rf_off = {
        .command = "nfc-rf-turn-off",
        .help    = "Force RF field OFF (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_rf_off,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_rf_off));

    const esp_console_cmd_t nfc_mode_get = {
        .command = "nfc-mode-get",
        .help    = "Get current RFAL mode and bitrates (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_mode_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mode_get));

    const esp_console_cmd_t nfc_mode_set = {
        .command = "nfc-mode-set",
        .help    = "Set RFAL mode/bitrates (debug). Usage: nfc-mode-set <mode> <tx_br> <rx_br>",
        .hint    = NULL,
        .func    = &cmd_nfc_mode_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_mode_set));

    const esp_console_cmd_t nfc_obsv_get = {
        .command = "nfc-obsv-get",
        .help    = "Get RFAL observation mode mux values (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_obsv_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_obsv_get));

    const esp_console_cmd_t nfc_obsv_set = {
        .command = "nfc-obsv-set",
        .help    = "Enable/disable RFAL observation mode (debug). Usage: nfc-obsv-set <0|1> [tx] [rx]",
        .hint    = NULL,
        .func    = &cmd_nfc_obsv_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_obsv_set));

    const esp_console_cmd_t nfc_reqa = {
        .command = "nfc-reqa-send",
        .help    = "Send ISO14443A REQA and print ATQA (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_reqa,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_reqa));

    const esp_console_cmd_t nfc_wupa = {
        .command = "nfc-wupa-send",
        .help    = "Send ISO14443A WUPA and print ATQA (debug).",
        .hint    = NULL,
        .func    = &cmd_nfc_wupa,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_wupa));

    const esp_console_cmd_t nfc_raw = {
        .command = "nfc-raw-transceive",
        .help    = "Raw transceive (debug). Usage: nfc-raw-transceive [--crc-manual] <hex...>",
        .hint    = NULL,
        .func    = &cmd_nfc_raw_txrx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_raw));

    const esp_console_cmd_t nfc_regdump = {
        .command = "nfc-regs-dump",
        .help    = "Dump ST25R3916 registers (debug; long output).",
        .hint    = NULL,
        .func    = &cmd_nfc_regdump,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_regdump));

    const esp_console_cmd_t nfc_emul_start = {
        .command = "nfc-emul-start",
        .help = "Start NFC card emulation. Optional: nfc-emul-start <stored_index> "
                "(loads UID from saved store and uses mode 3A).",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_start));

    const esp_console_cmd_t nfc_emul_stop = {
        .command = "nfc-emul-stop",
        .help    = "Stop NFC card emulation.",
        .hint    = NULL,
        .func    = &cmd_nfc_emul_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_emul_stop));

    const esp_console_cmd_t nfc_tune_auto = {
        .command = "nfc-tune-auto-run",
        .help    = "Run ST25R3916 auto antenna tuning and print values.",
        .hint    = NULL,
        .func    = &cmd_nfc_tune_auto,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_tune_auto));

    const esp_console_cmd_t nfc_tune_get = {
        .command = "nfc-tune-get",
        .help    = "Read current AAT_A/AAT_B and phase/amplitude.",
        .hint    = NULL,
        .func    = &cmd_nfc_tune_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_tune_get));

    nfc_tune_set_args.aat_a =
        arg_int1(NULL, NULL, "<aat_a>", "AAT_A value (0..255)");
    nfc_tune_set_args.aat_b =
        arg_int1(NULL, NULL, "<aat_b>", "AAT_B value (0..255)");
    nfc_tune_set_args.end = arg_end(2);

    const esp_console_cmd_t nfc_tune_set = {
        .command  = "nfc-tune-set",
        .help     = "Write AAT_A/AAT_B and print resulting phase/amplitude.",
        .hint     = NULL,
        .func     = &cmd_nfc_tune_set,
        .argtable = &nfc_tune_set_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_tune_set));

    const esp_console_cmd_t nfc_all = {
        .command = "nfc-tech-set-all",
        .help    = "Select technology NFC-A/B/V/F",
        .hint    = NULL,
        .func    = &cmd_nfc_all,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_all));

    const esp_console_cmd_t nfc_a = {
        .command = "nfc-tech-set-a",
        .help    = "Select technology NFC-A (ISO14443A)",
        .hint    = NULL,
        .func    = &cmd_nfc_a,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_a));

    const esp_console_cmd_t nfc_b = {
        .command = "nfc-tech-set-b",
        .help    = "Select technology NFC-B (ISO14443B)",
        .hint    = NULL,
        .func    = &cmd_nfc_b,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_b));

    const esp_console_cmd_t nfc_st25tb = {
        .command = "nfc-tech-set-st25tb",
        .help    = "Select technology ST25TB (ISO14443B ST25TB)",
        .hint    = NULL,
        .func    = &cmd_nfc_st25tb,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_st25tb));

    const esp_console_cmd_t nfc_v = {
        .command = "nfc-tech-set-v",
        .help    = "Select technology NFC-V Vicinity (ISO15693)",
        .hint    = NULL,
        .func    = &cmd_nfc_v,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_v));

    const esp_console_cmd_t nfc_f = {
        .command = "nfc-tech-set-f",
        .help    = "Select technology NFC-F (FeliCa)",
        .hint    = NULL,
        .func    = &cmd_nfc_f,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nfc_f));
}

void cli_poom_nfc_register_cmds(void)
{
    static bool s_registered = false;
    if (s_registered)
    {
        return;
    }
    s_registered = true;

    register_poom_nfc_cmds();
}

/* =========================================================
 *  Entry point
 * ========================================================= */

void cli_poom_nfc_option(void)
{
    poom_console_begin(cli_poom_nfc_register_cmds);
}
