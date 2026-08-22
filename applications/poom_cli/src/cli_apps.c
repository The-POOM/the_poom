// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "cli_apps.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp_pong.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_dec.h"
#include "ir_rcv.h"
#include "ir_tx.h"
#include "poom_ble_spam.h"
#include "poom_ble_tracker.h"
#include "poom_ble_gatt_dynamic.h"
#include "poom_lua.h"
#include "poom_secrets_store.h"
#include "poom_scanner_core.h"
#include "poom_sniffer_device.h"
#include "poom_wifi_captive.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_deauth.h"
#include "poom_wifi_deauth_detector.h"
#include "poom_wifi_karma.h"
#include "poom_wifi_scanner.h"
#include "poom_wifi_spam.h"
#include "sd_card.h"

#define CLI_APPS_BLE_NAME_MAX       (31U)
#define CLI_APPS_BLE_GATT_NAME_MAX  (31U)
#define CLI_APPS_BLE_GATT_VALUE_MAX POOM_BLE_GATT_DYNAMIC_CHAR_VAL_MAX_LEN_DEFAULT
#define CLI_APPS_KARMA_LIST_MAX     (16U)
#define CLI_APPS_WIFI_SSID_MAX      (32U)
#define CLI_APPS_WIFI_PASS_MAX      (64U)
#define CLI_APPS_LUA_UPLOAD_PATH_MAX (160U)
#define CLI_APPS_IR_RX_BUFFER_SYMBOLS (256U)
#define CLI_APPS_IR_TIMEOUT_MS      (5000U)
#define CLI_APPS_WIFI_CONNECT_TIMEOUT_MS (15000U)
#define CLI_APPS_CAPTIVE_PORTAL_MAX (24U)

typedef enum
{
    CLI_APPS_IR_PROTO_NONE = IR_PROTOCOL_NONE,
    CLI_APPS_IR_PROTO_NEC = IR_PROTOCOL_NEC,
    CLI_APPS_IR_PROTO_NEC_EXT = IR_PROTOCOL_NEC_EXT,
    CLI_APPS_IR_PROTO_SAMSUNG32 = IR_PROTOCOL_SAMSUNG32,
    CLI_APPS_IR_PROTO_SIRC = IR_PROTOCOL_SIRC,
    CLI_APPS_IR_PROTO_SIRC15 = IR_PROTOCOL_SIRC15,
    CLI_APPS_IR_PROTO_SIRC20 = IR_PROTOCOL_SIRC20,
    CLI_APPS_IR_PROTO_RC5 = IR_PROTOCOL_RC5,
    CLI_APPS_IR_PROTO_RC5X = IR_PROTOCOL_RC5X,
    CLI_APPS_IR_PROTO_RC6 = IR_PROTOCOL_RC6,
    CLI_APPS_IR_PROTO_RCA = IR_PROTOCOL_RCA,
    CLI_APPS_IR_PROTO_PIONEER = IR_PROTOCOL_PIONEER,
    CLI_APPS_IR_PROTO_KASEIKYO = IR_PROTOCOL_KASEIKYO,
    CLI_APPS_IR_PROTO_NEC42 = IR_PROTOCOL_NEC42,
    CLI_APPS_IR_PROTO_NEC42_EXT = IR_PROTOCOL_NEC42_EXT,
} cli_apps_ir_proto_t;

typedef struct
{
    uint8_t protocol;
    uint8_t flags;
    uint16_t reserved;
    uint32_t address;
    uint32_t command;
} cli_apps_ir_code_t;

typedef struct
{
    uint8_t protocol;
    uint16_t address;
    uint8_t command;
    uint8_t reserved;
} cli_apps_ir_code_legacy_t;

typedef struct
{
    uint8_t channel;
    uint32_t count;
    uint8_t pct;
    int16_t rssi_avg_dbm;
    int8_t rssi_max_dbm;
} cli_apps_scanner_entry_t;

typedef struct
{
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t char_perm;
    esp_gatt_char_prop_t char_property;
    uint8_t* value;
    uint16_t value_len;
    uint16_t value_capacity;
} cli_apps_ble_gatt_char_cfg_t;

typedef struct
{
    char name[CLI_APPS_BLE_GATT_NAME_MAX + 1U];
    esp_bt_uuid_t service_uuid;
    cli_apps_ble_gatt_char_cfg_t* chars;
    size_t char_count;
} cli_apps_ble_gatt_cfg_t;

static bool s_cli_apps_ble_spam_running = false;
static bool s_cli_apps_karma_running = false;
static char s_cli_apps_ble_spam_name[CLI_APPS_BLE_NAME_MAX + 1U] = "-";
static cli_apps_ble_gatt_cfg_t s_cli_apps_ble_gatt_cfg = {0};
static poom_ble_tracker_profile_t* s_cli_apps_ble_tracker_profiles = NULL;
static uint16_t s_cli_apps_ble_tracker_profile_count = 0U;
static cli_apps_ir_code_t s_cli_apps_ir_last_code = {0};
static bool s_cli_apps_ir_last_valid = false;
static bool s_cli_apps_captive_running = false;
static poom_scanner_core_mode_t s_cli_apps_scanner_last_mode = POOM_SCANNER_CORE_MODE_NONE;
static char s_cli_apps_captive_clone_ssid[CLI_APPS_WIFI_SSID_MAX + 1U] = "";
static char s_cli_apps_captive_portal_file[CLI_APPS_CAPTIVE_PORTAL_MAX + 1U] = "";
static char s_cli_apps_wifi_last_ssid[CLI_APPS_WIFI_SSID_MAX + 1U] = "";
static char s_cli_apps_wifi_last_pass[CLI_APPS_WIFI_PASS_MAX + 1U] = "";
static bool s_cli_apps_wifi_last_known = false;
static FILE* s_cli_apps_lua_upload_file = NULL;
static bool s_cli_apps_lua_upload_active = false;
static char s_cli_apps_lua_upload_path[CLI_APPS_LUA_UPLOAD_PATH_MAX + 1U] = "";
static size_t s_cli_apps_lua_upload_bytes = 0U;
static size_t s_cli_apps_lua_upload_lines = 0U;

static bool cli_apps_ble_gatt_reset_cfg_(void);
static bool cli_apps_ble_gatt_ensure_cfg_(void);
static bool cli_apps_ble_gatt_char_set_mode_(cli_apps_ble_gatt_char_cfg_t* char_cfg, const char* mode);
static bool cli_apps_ble_gatt_char_add_(const esp_bt_uuid_t* uuid, const char* mode);
static bool cli_apps_ble_gatt_char_del_(size_t index);
static cli_apps_ble_gatt_char_cfg_t* cli_apps_ble_gatt_char_get_(size_t index);
static void cli_apps_ble_gatt_print_char_(size_t index, const cli_apps_ble_gatt_char_cfg_t* char_cfg);

/**
 * @brief Internal helper for `cli_apps_hex_nibble`.
 *
 * @param[in] c Parameter passed to the function.
 * @param[in] ok Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t cli_apps_hex_nibble_(char c, bool* ok)
{
    if((c >= '0') && (c <= '9'))
    {
        *ok = true;
        return (uint8_t)(c - '0');
    }

    c = (char)tolower((unsigned char)c);
    if((c >= 'a') && (c <= 'f'))
    {
        *ok = true;
        return (uint8_t)(10 + (c - 'a'));
    }

    *ok = false;
    return 0U;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_byte Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_parse_hex_byte_(const char* text, uint8_t* out_byte)
{
    bool ok_hi = false;
    bool ok_lo = false;
    const char* p = text;

    if((text == NULL) || (out_byte == NULL))
    {
        return false;
    }

    if((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X')))
    {
        p += 2;
    }

    if((p[0] == '\0') || (p[1] == '\0') || (p[2] != '\0'))
    {
        return false;
    }

    *out_byte = (uint8_t)((cli_apps_hex_nibble_(p[0], &ok_hi) << 4) |
                          cli_apps_hex_nibble_(p[1], &ok_lo));
    return ok_hi && ok_lo;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_free_chars`.
 *
 * @return void
 */
static void cli_apps_ble_gatt_free_chars_(void)
{
    if(s_cli_apps_ble_gatt_cfg.chars == NULL)
    {
        s_cli_apps_ble_gatt_cfg.char_count = 0U;
        return;
    }

    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        free(s_cli_apps_ble_gatt_cfg.chars[i].value);
        s_cli_apps_ble_gatt_cfg.chars[i].value = NULL;
        s_cli_apps_ble_gatt_cfg.chars[i].value_len = 0U;
        s_cli_apps_ble_gatt_cfg.chars[i].value_capacity = 0U;
    }

    free(s_cli_apps_ble_gatt_cfg.chars);
    s_cli_apps_ble_gatt_cfg.chars = NULL;
    s_cli_apps_ble_gatt_cfg.char_count = 0U;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_char_set_mode`.
 *
 * @param[in] char_cfg Parameter passed to the function.
 * @param[in] mode Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ble_gatt_char_set_mode_(cli_apps_ble_gatt_char_cfg_t* char_cfg, const char* mode)
{
    if((char_cfg == NULL) || (mode == NULL))
    {
        return false;
    }

    if(strcasecmp(mode, "read") == 0)
    {
        char_cfg->char_perm = ESP_GATT_PERM_READ;
        char_cfg->char_property = ESP_GATT_CHAR_PROP_BIT_READ;
        return true;
    }

    if(strcasecmp(mode, "write") == 0)
    {
        char_cfg->char_perm = ESP_GATT_PERM_WRITE;
        char_cfg->char_property = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
        return true;
    }

    if(strcasecmp(mode, "readwrite") == 0)
    {
        char_cfg->char_perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        char_cfg->char_property = ESP_GATT_CHAR_PROP_BIT_READ |
                                  ESP_GATT_CHAR_PROP_BIT_WRITE |
                                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
        return true;
    }

    return false;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @param[in] char_cfg Parameter passed to the function.
 * @param[in] uuid Parameter passed to the function.
 * @param[in] mode Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ble_gatt_char_init_(cli_apps_ble_gatt_char_cfg_t* char_cfg,
                                         const esp_bt_uuid_t* uuid,
                                         const char* mode)
{
    if(char_cfg == NULL)
    {
        return false;
    }

    memset(char_cfg, 0, sizeof(*char_cfg));
    char_cfg->value_capacity = CLI_APPS_BLE_GATT_VALUE_MAX;
    char_cfg->value = (uint8_t*)calloc(char_cfg->value_capacity, sizeof(uint8_t));
    if(char_cfg->value == NULL)
    {
        char_cfg->value_capacity = 0U;
        return false;
    }

    if(uuid != NULL)
    {
        char_cfg->char_uuid = *uuid;
    }
    else
    {
        char_cfg->char_uuid.len = ESP_UUID_LEN_16;
        char_cfg->char_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_CHAR_UUID_DEFAULT;
    }

    if(!cli_apps_ble_gatt_char_set_mode_(char_cfg, (mode != NULL) ? mode : "readwrite"))
    {
        free(char_cfg->value);
        memset(char_cfg, 0, sizeof(*char_cfg));
        return false;
    }

    char_cfg->value[0] = 0x61U;
    char_cfg->value[1] = 0x70U;
    char_cfg->value[2] = 0x70U;
    char_cfg->value[3] = 0x73U;
    char_cfg->value[4] = 0x65U;
    char_cfg->value[5] = 0x63U;
    char_cfg->value_len = 6U;
    return true;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_char_add`.
 *
 * @param[in] uuid Parameter passed to the function.
 * @param[in] mode Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ble_gatt_char_add_(const esp_bt_uuid_t* uuid, const char* mode)
{
    cli_apps_ble_gatt_char_cfg_t* new_chars;

    new_chars = (cli_apps_ble_gatt_char_cfg_t*)calloc(
        s_cli_apps_ble_gatt_cfg.char_count + 1U,
        sizeof(*new_chars));
    if(new_chars == NULL)
    {
        return false;
    }

    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        new_chars[i] = s_cli_apps_ble_gatt_cfg.chars[i];
    }

    if(!cli_apps_ble_gatt_char_init_(
           &new_chars[s_cli_apps_ble_gatt_cfg.char_count],
           uuid,
           mode))
    {
        free(new_chars);
        return false;
    }

    free(s_cli_apps_ble_gatt_cfg.chars);
    s_cli_apps_ble_gatt_cfg.chars = new_chars;
    s_cli_apps_ble_gatt_cfg.char_count++;
    return true;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_char_del`.
 *
 * @param[in] index Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ble_gatt_char_del_(size_t index)
{
    cli_apps_ble_gatt_char_cfg_t* new_chars = NULL;
    size_t new_count;
    size_t dst = 0U;

    if(index >= s_cli_apps_ble_gatt_cfg.char_count)
    {
        return false;
    }

    if(s_cli_apps_ble_gatt_cfg.char_count <= 1U)
    {
        return false;
    }

    new_count = s_cli_apps_ble_gatt_cfg.char_count - 1U;
    new_chars = (cli_apps_ble_gatt_char_cfg_t*)calloc(new_count, sizeof(*new_chars));
    if(new_chars == NULL)
    {
        return false;
    }

    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        if(i == index)
        {
            free(s_cli_apps_ble_gatt_cfg.chars[i].value);
            s_cli_apps_ble_gatt_cfg.chars[i].value = NULL;
            continue;
        }

        new_chars[dst++] = s_cli_apps_ble_gatt_cfg.chars[i];
    }

    free(s_cli_apps_ble_gatt_cfg.chars);
    s_cli_apps_ble_gatt_cfg.chars = new_chars;
    s_cli_apps_ble_gatt_cfg.char_count = new_count;
    return true;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_char_get`.
 *
 * @param[in] index Parameter passed to the function.
 * @return cli_apps_ble_gatt_char_cfg_t*
 */
static cli_apps_ble_gatt_char_cfg_t* cli_apps_ble_gatt_char_get_(size_t index)
{
    if(index >= s_cli_apps_ble_gatt_cfg.char_count)
    {
        return NULL;
    }

    return &s_cli_apps_ble_gatt_cfg.chars[index];
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_reset_cfg`.
 *
 * @return bool
 */
static bool cli_apps_ble_gatt_reset_cfg_(void)
{
    cli_apps_ble_gatt_free_chars_();
    memset(&s_cli_apps_ble_gatt_cfg, 0, sizeof(s_cli_apps_ble_gatt_cfg));
    (void)snprintf(s_cli_apps_ble_gatt_cfg.name,
                   sizeof(s_cli_apps_ble_gatt_cfg.name),
                   "%s",
                   POOM_BLE_GATT_DYNAMIC_DEVICE_NAME_DEFAULT);
    s_cli_apps_ble_gatt_cfg.service_uuid.len = ESP_UUID_LEN_16;
    s_cli_apps_ble_gatt_cfg.service_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_SERVICE_UUID_DEFAULT;
    return cli_apps_ble_gatt_char_add_(NULL, "readwrite");
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_uuid Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_parse_uuid_(const char* text, esp_bt_uuid_t* out_uuid)
{
    size_t digits = 0U;
    char hex_only[32 + 1U] = {0};

    if((text == NULL) || (out_uuid == NULL))
    {
        return false;
    }

    for(size_t i = 0U; text[i] != '\0'; i++)
    {
        if(text[i] == '-')
        {
            continue;
        }
        if(!isxdigit((unsigned char)text[i]) || (digits >= 32U))
        {
            return false;
        }
        hex_only[digits++] = text[i];
    }

    if(digits == 4U)
    {
        out_uuid->len = ESP_UUID_LEN_16;
        out_uuid->uuid.uuid16 = (uint16_t)strtoul(hex_only, NULL, 16);
        return true;
    }

    if(digits != 32U)
    {
        return false;
    }

    out_uuid->len = ESP_UUID_LEN_128;
    for(size_t i = 0U; i < 16U; i++)
    {
        char byte_text[3] = {0};
        const size_t src = i * 2U;
        const size_t dst = 15U - i;
        uint8_t byte = 0U;

        byte_text[0] = hex_only[src];
        byte_text[1] = hex_only[src + 1U];

        if(!cli_apps_parse_hex_byte_(byte_text, &byte))
        {
            return false;
        }
        out_uuid->uuid.uuid128[dst] = byte;
    }

    return true;
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] uuid Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return void
 */
static void cli_apps_format_uuid_(const esp_bt_uuid_t* uuid, char* out, size_t out_len)
{
    if((uuid == NULL) || (out == NULL) || (out_len == 0U))
    {
        return;
    }

    if(uuid->len == ESP_UUID_LEN_16)
    {
        (void)snprintf(out, out_len, "%04X", (unsigned)uuid->uuid.uuid16);
        return;
    }

    if(uuid->len == ESP_UUID_LEN_32)
    {
        (void)snprintf(out, out_len, "%08" PRIX32, uuid->uuid.uuid32);
        return;
    }

    if(uuid->len == ESP_UUID_LEN_128)
    {
        const uint8_t* u = uuid->uuid.uuid128;
        (void)snprintf(out,
                       out_len,
                       "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                       (unsigned)u[15], (unsigned)u[14], (unsigned)u[13], (unsigned)u[12],
                       (unsigned)u[11], (unsigned)u[10],
                       (unsigned)u[9], (unsigned)u[8],
                       (unsigned)u[7], (unsigned)u[6],
                       (unsigned)u[5], (unsigned)u[4], (unsigned)u[3], (unsigned)u[2],
                       (unsigned)u[1], (unsigned)u[0]);
        return;
    }

    (void)snprintf(out, out_len, "UNKNOWN");
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_props_name`.
 *
 * @param[in] props Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_ble_gatt_props_name_(esp_gatt_char_prop_t props)
{
    const bool can_read = (props & ESP_GATT_CHAR_PROP_BIT_READ) != 0U;
    const bool can_write = (props & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) != 0U;

    if(can_read && can_write)
    {
        return "readwrite";
    }
    if(can_read)
    {
        return "read";
    }
    if(can_write)
    {
        return "write";
    }
    return "none";
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_ble_gatt_(void)
{
    poom_ble_gatt_dynamic_stop();
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_ensure_cfg`.
 *
 * @return bool
 */
static bool cli_apps_ble_gatt_ensure_cfg_(void)
{
    if(s_cli_apps_ble_gatt_cfg.name[0] == '\0')
    {
        return cli_apps_ble_gatt_reset_cfg_();
    }

    if(s_cli_apps_ble_gatt_cfg.char_count == 0U)
    {
        return cli_apps_ble_gatt_char_add_(NULL, "readwrite");
    }

    return true;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_value Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_parse_size_t_(const char* text, size_t* out_value)
{
    char* end = NULL;
    unsigned long parsed;

    if((text == NULL) || (out_value == NULL) || (text[0] == '\0'))
    {
        return false;
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if((errno != 0) || (end == NULL) || (*end != '\0'))
    {
        return false;
    }

    *out_value = (size_t)parsed;
    return true;
}

/**
 * @brief Internal helper for `cli_apps_ble_gatt_print_char`.
 *
 * @param[in] index Parameter passed to the function.
 * @param[in] char_cfg Parameter passed to the function.
 * @return void
 */
static void cli_apps_ble_gatt_print_char_(size_t index, const cli_apps_ble_gatt_char_cfg_t* char_cfg)
{
    char char_uuid[40];

    if(char_cfg == NULL)
    {
        return;
    }

    cli_apps_format_uuid_(&char_cfg->char_uuid, char_uuid, sizeof(char_uuid));
    printf("ble-gatt char[%u]: uuid=%s | props=%s | value_len=%u\n",
           (unsigned)index,
           char_uuid,
           cli_apps_ble_gatt_props_name_(char_cfg->char_property),
           (unsigned)char_cfg->value_len);

    printf("ble-gatt char[%u] value:", (unsigned)index);
    for(uint16_t i = 0U; i < char_cfg->value_len; i++)
    {
        printf(" %02X", (unsigned)char_cfg->value[i]);
    }
    printf("\n");
}

/**
 * @brief Internal helper for `cli_apps_ble_tracker_reset_cache`.
 *
 * @return void
 */
static void cli_apps_ble_tracker_reset_cache_(void)
{
    free(s_cli_apps_ble_tracker_profiles);
    s_cli_apps_ble_tracker_profiles = NULL;
    s_cli_apps_ble_tracker_profile_count = 0U;
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] name Parameter passed to the function.
 * @return void
 */
static void cli_apps_ble_spam_name_cb_(const char* name)
{
    (void)snprintf(
        s_cli_apps_ble_spam_name,
        sizeof(s_cli_apps_ble_spam_name),
        "%s",
        ((name != NULL) && (name[0] != '\0')) ? name : "-");
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] record Parameter passed to the function.
 * @return void
 */
static void cli_apps_ble_tracker_cb_(poom_ble_tracker_profile_t record)
{
    int idx;

    if(!record.is_tracker)
    {
        return;
    }

    idx = poom_ble_tracker_find_profile_by_mac(
        s_cli_apps_ble_tracker_profiles,
        s_cli_apps_ble_tracker_profile_count,
        record.mac_address);
    if(idx >= 0)
    {
        s_cli_apps_ble_tracker_profiles[idx] = record;
        return;
    }

    poom_ble_tracker_add_profile(
        &s_cli_apps_ble_tracker_profiles,
        &s_cli_apps_ble_tracker_profile_count,
        record);
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_ble_tracker_(void)
{
    poom_ble_tracker_stop();
    poom_ble_tracker_register_scan_callback(NULL);
    cli_apps_ble_tracker_reset_cache_();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_ble_spam_(void)
{
    poom_ble_spam_register_cb(NULL);
    poom_ble_spam_app_stop();
    s_cli_apps_ble_spam_running = false;
    (void)snprintf(s_cli_apps_ble_spam_name, sizeof(s_cli_apps_ble_spam_name), "-");
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_wifi_spam_(void)
{
    (void)poom_wifi_spam_stop();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_karma_(void)
{
    (void)poom_wifi_karma_stop();
    s_cli_apps_karma_running = false;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_deauth_(void)
{
    (void)poom_wifi_deauth_stop();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_deauth_detector_(void)
{
    (void)poom_wifi_deauth_detector_stop();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_captive_(void)
{
    poom_wifi_captive_stop();
    poom_wifi_captive_set_ap_clone(NULL, false);
    s_cli_apps_captive_running = false;
    s_cli_apps_captive_clone_ssid[0] = '\0';
    s_cli_apps_captive_portal_file[0] = '\0';
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_scanner_(void)
{
    (void)poom_scanner_core_stop();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_sniffer_device_(void)
{
    (void)poom_sniffer_device_stop();
}

/**
 * @brief Loads internal data used by this module.
 *
 * @return void
 */
static void cli_apps_lua_upload_reset_state_(void)
{
    if(s_cli_apps_lua_upload_file != NULL)
    {
        (void)fclose(s_cli_apps_lua_upload_file);
        s_cli_apps_lua_upload_file = NULL;
    }

    s_cli_apps_lua_upload_active = false;
    s_cli_apps_lua_upload_path[0] = '\0';
    s_cli_apps_lua_upload_bytes = 0U;
    s_cli_apps_lua_upload_lines = 0U;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] remove_partial Parameter passed to the function.
 * @return void
 */
static void cli_apps_lua_upload_abort_(bool remove_partial)
{
    char path[CLI_APPS_LUA_UPLOAD_PATH_MAX + 1U] = "";

    if(s_cli_apps_lua_upload_path[0] != '\0')
    {
        (void)snprintf(path, sizeof(path), "%s", s_cli_apps_lua_upload_path);
    }

    cli_apps_lua_upload_reset_state_();

    if(remove_partial && (path[0] != '\0'))
    {
        (void)unlink(path);
    }
}

/**
 * @brief Internal helper for `cli_apps_sd_ensure_mounted`.
 *
 * @return esp_err_t
 */
static esp_err_t cli_apps_sd_ensure_mounted_(void)
{
    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        return sd_card_mount();
    }

    return ESP_OK;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] path Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_lua_upload_path_valid_(const char* path)
{
    if((path == NULL) || (path[0] == '\0'))
    {
        return false;
    }

    return strncmp(path, SD_CARD_PATH "/", strlen(SD_CARD_PATH) + 1U) == 0;
}

/**
 * @brief Internal helper for `cli_apps_ensure_parent_dirs`.
 *
 * @param[in] abs_path Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t cli_apps_ensure_parent_dirs_(const char* abs_path)
{
    char tmp[CLI_APPS_LUA_UPLOAD_PATH_MAX + 1U] = "";
    const size_t prefix_len = strlen(SD_CARD_PATH);

    if((abs_path == NULL) || !cli_apps_lua_upload_path_valid_(abs_path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(strlen(abs_path) > CLI_APPS_LUA_UPLOAD_PATH_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)snprintf(tmp, sizeof(tmp), "%s", abs_path);

    for(char* p = tmp + prefix_len + 1U; *p != '\0'; p++)
    {
        if(*p != '/')
        {
            continue;
        }

        *p = '\0';
        if((mkdir(tmp, 0775) != 0) && (errno != EEXIST))
        {
            return ESP_FAIL;
        }
        *p = '/';
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `cli_apps_remember_wifi_credentials`.
 *
 * @param[in] ssid Parameter passed to the function.
 * @param[in] password Parameter passed to the function.
 * @return void
 */
static void cli_apps_remember_wifi_credentials_(const char* ssid, const char* password)
{
    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        return;
    }

    (void)snprintf(s_cli_apps_wifi_last_ssid, sizeof(s_cli_apps_wifi_last_ssid), "%s", ssid);
    (void)snprintf(s_cli_apps_wifi_last_pass, sizeof(s_cli_apps_wifi_last_pass), "%s", (password != NULL) ? password : "");
    s_cli_apps_wifi_last_known = true;
}

/**
 * @brief Internal helper for `cli_apps_wifi_connection_active`.
 *
 * @return bool
 */
static bool cli_apps_wifi_connection_active_(void)
{
    return poom_wifi_ctrl_sta_is_connected() || poom_wifi_ctrl_sta_has_ip();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] reason Parameter passed to the function.
 * @return void
 */
static void cli_apps_stop_wifi_sta_(const char* reason)
{
    if(!cli_apps_wifi_connection_active_())
    {
        return;
    }

    if((reason != NULL) && (reason[0] != '\0'))
    {
        printf("%s\n", reason);
    }

    (void)poom_wifi_ctrl_sta_disconnect();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_other_wifi_apps_(void)
{
    bool wifi_spam_running = false;

    if((poom_wifi_spam_get_running(&wifi_spam_running) == ESP_OK) && wifi_spam_running)
    {
        printf("Stopping wifi-spam due to Wi-Fi app conflict\n");
        cli_apps_stop_wifi_spam_();
    }

    if(s_cli_apps_karma_running)
    {
        printf("Stopping karma due to Wi-Fi app conflict\n");
        cli_apps_stop_karma_();
    }

    if(poom_wifi_deauth_is_running())
    {
        printf("Stopping deauth due to Wi-Fi app conflict\n");
        cli_apps_stop_deauth_();
    }

    if(poom_wifi_deauth_detector_is_running())
    {
        printf("Stopping deauth-detector due to Wi-Fi app conflict\n");
        cli_apps_stop_deauth_detector_();
    }

    if(s_cli_apps_captive_running)
    {
        printf("Stopping captive due to Wi-Fi app conflict\n");
        cli_apps_stop_captive_();
    }

    if(poom_scanner_core_get_mode() != POOM_SCANNER_CORE_MODE_NONE)
    {
        printf("Stopping scanner due to Wi-Fi app conflict\n");
        cli_apps_stop_scanner_();
    }

    {
        bool sniffer_running = false;
        if((poom_sniffer_device_get_running(&sniffer_running) == ESP_OK) && sniffer_running)
        {
            printf("Stopping sniffer due to Wi-Fi app conflict\n");
            cli_apps_stop_sniffer_device_();
        }
    }
}

/**
 * @brief Internal helper for `cli_apps_wifi_runtime_active`.
 *
 * @return bool
 */
static bool cli_apps_wifi_runtime_active_(void)
{
    bool wifi_spam_running = false;
    bool sniffer_running = false;

    if((poom_wifi_spam_get_running(&wifi_spam_running) == ESP_OK) && wifi_spam_running)
    {
        return true;
    }

    if((poom_sniffer_device_get_running(&sniffer_running) == ESP_OK) && sniffer_running)
    {
        return true;
    }

    return cli_apps_wifi_connection_active_() ||
           s_cli_apps_karma_running ||
           poom_wifi_deauth_is_running() ||
           poom_wifi_deauth_detector_is_running() ||
           s_cli_apps_captive_running ||
           (poom_scanner_core_get_mode() != POOM_SCANNER_CORE_MODE_NONE);
}

/**
 * @brief Internal helper for `cli_apps_ble_runtime_active`.
 *
 * @return bool
 */
static bool cli_apps_ble_runtime_active_(void)
{
    return s_cli_apps_ble_spam_running || poom_ble_tracker_is_active();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_ble_runtime_for_wifi_(void)
{
    if(s_cli_apps_ble_spam_running)
    {
        printf("Stopping ble-spam due to BLE/Wi-Fi radio conflict\n");
        cli_apps_stop_ble_spam_();
    }

    if(poom_ble_tracker_is_active())
    {
        printf("Stopping ble-tracker due to BLE/Wi-Fi radio conflict\n");
        cli_apps_stop_ble_tracker_();
    }
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void cli_apps_stop_wifi_runtime_for_ble_(void)
{
    cli_apps_stop_other_wifi_apps_();
    cli_apps_stop_wifi_sta_("Stopping Wi-Fi STA due to BLE/Wi-Fi radio conflict");
}

/**
 * @brief Internal helper for `cli_apps_prepare_wifi_base`.
 *
 * @return void
 */
static void cli_apps_prepare_wifi_base_(void)
{
    if(cli_apps_ble_runtime_active_())
    {
        cli_apps_stop_ble_runtime_for_wifi_();
    }

    cli_apps_stop_other_wifi_apps_();
}

/**
 * @brief Internal helper for `cli_apps_prepare_wifi_attack`.
 *
 * @return void
 */
static void cli_apps_prepare_wifi_attack_(void)
{
    cli_apps_prepare_wifi_base_();
    cli_apps_stop_wifi_sta_("Stopping Wi-Fi STA due to Wi-Fi attack/app conflict");
}

/**
 * @brief Internal helper for `cli_apps_wifi_auth_name`.
 *
 * @param[in] authmode Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_wifi_auth_name_(wifi_auth_mode_t authmode)
{
    switch(authmode)
    {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Internal helper for `cli_apps_wifi_scan_print`.
 *
 * @return void
 */
static void cli_apps_wifi_scan_print_(void)
{
    poom_wifi_scanner_ap_records_t* records = poom_wifi_scanner_get_ap_records();

    if((records == NULL) || (records->count == 0U))
    {
        printf("wifi-scan: no cached APs\n");
        return;
    }

    for(uint16_t i = 0U; i < records->count; i++)
    {
        const wifi_ap_record_t* ap = &records->records[i];
        printf("%u) SSID=%s | RSSI=%d | CH=%u | AUTH=%s | BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
               (unsigned)i,
               (const char*)ap->ssid,
               ap->rssi,
               (unsigned)ap->primary,
               cli_apps_wifi_auth_name_(ap->authmode),
               (unsigned)ap->bssid[0],
               (unsigned)ap->bssid[1],
               (unsigned)ap->bssid[2],
               (unsigned)ap->bssid[3],
               (unsigned)ap->bssid[4],
               (unsigned)ap->bssid[5]);
    }
}

/**
 * @brief Internal helper for `cli_apps_wifi_get_ip`.
 *
 * @param[in] out_ip Parameter passed to the function.
 * @param[in] out_ip_len Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_wifi_get_ip_(char* out_ip, size_t out_ip_len)
{
    esp_netif_t* netif = NULL;
    esp_netif_ip_info_t info = {0};

    if((out_ip == NULL) || (out_ip_len == 0U))
    {
        return false;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if((netif == NULL) || (esp_netif_get_ip_info(netif, &info) != ESP_OK))
    {
        return false;
    }

    (void)snprintf(out_ip, out_ip_len, IPSTR, IP2STR(&info.ip));
    return true;
}

/**
 * @brief Internal helper for `cli_apps_wifi_get_connected_ssid`.
 *
 * @param[in] out_ssid Parameter passed to the function.
 * @param[in] out_ssid_len Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_wifi_get_connected_ssid_(char* out_ssid, size_t out_ssid_len)
{
    wifi_ap_record_t ap_info = {0};

    if((out_ssid == NULL) || (out_ssid_len == 0U))
    {
        return false;
    }

    if(esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK)
    {
        return false;
    }

    (void)snprintf(out_ssid, out_ssid_len, "%s", (const char*)ap_info.ssid);
    return true;
}

/**
 * @brief Internal helper for `cli_apps_wifi_wait_ip`.
 *
 * @param[in] timeout_ms Parameter passed to the function.
 * @param[in] out_ip Parameter passed to the function.
 * @param[in] out_ip_len Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_wifi_wait_ip_(uint32_t timeout_ms, char* out_ip, size_t out_ip_len)
{
    const uint32_t wait_step_ms = 250U;
    uint32_t waited_ms = 0U;

    while(waited_ms < timeout_ms)
    {
        if(poom_wifi_ctrl_sta_has_ip())
        {
            if((out_ip != NULL) && (out_ip_len > 0U))
            {
                (void)cli_apps_wifi_get_ip_(out_ip, out_ip_len);
            }
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_step_ms));
        waited_ms += wait_step_ms;
    }

    return false;
}

/**
 * @brief Internal helper for `cli_apps_scanner_mode_name`.
 *
 * @param[in] mode Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_scanner_mode_name_(poom_scanner_core_mode_t mode)
{
    switch(mode)
    {
        case POOM_SCANNER_CORE_MODE_WIFI: return "WIFI";
        case POOM_SCANNER_CORE_MODE_IEEE802154: return "IEEE802154";
        default: return "NONE";
    }
}

/**
 * @brief Internal helper for `cli_apps_scanner_report_mode`.
 *
 * @return poom_scanner_core_mode_t
 */
static poom_scanner_core_mode_t cli_apps_scanner_report_mode_(void)
{
    const poom_scanner_core_mode_t current_mode = poom_scanner_core_get_mode();
    if(current_mode != POOM_SCANNER_CORE_MODE_NONE)
    {
        return current_mode;
    }

    return s_cli_apps_scanner_last_mode;
}

/**
 * @brief Sorts internal items for this module.
 *
 * @param[in] entries Parameter passed to the function.
 * @param[in] count Parameter passed to the function.
 * @return void
 */
static void cli_apps_scanner_sort_entries_desc_(cli_apps_scanner_entry_t* entries, size_t count)
{
    if((entries == NULL) || (count <= 1U))
    {
        return;
    }

    for(size_t i = 0U; i + 1U < count; i++)
    {
        size_t best = i;

        for(size_t j = i + 1U; j < count; j++)
        {
            const bool better = (entries[j].count > entries[best].count) ||
                                ((entries[j].count == entries[best].count) &&
                                 (entries[j].channel < entries[best].channel));
            if(better)
            {
                best = j;
            }
        }

        if(best != i)
        {
            const cli_apps_scanner_entry_t tmp = entries[i];
            entries[i] = entries[best];
            entries[best] = tmp;
        }
    }
}

/**
 * @brief Internal helper for `cli_apps_scanner_print_wifi_channels`.
 *
 * @return int
 */
static int cli_apps_scanner_print_wifi_channels_(void)
{
    poom_scanner_core_wifi_stats_t st = {0};
    cli_apps_scanner_entry_t entries[POOM_SCANNER_CORE_WIFI_CH_COUNT] = {{0}};
    uint32_t total = 0U;
    size_t count = 0U;

    if(!poom_scanner_core_get_wifi_stats(&st))
    {
        printf("scanner-channels: cannot read Wi-Fi stats\n");
        return 1;
    }

    count = st.channel_count;
    if(count > POOM_SCANNER_CORE_WIFI_CH_COUNT)
    {
        count = POOM_SCANNER_CORE_WIFI_CH_COUNT;
    }

    for(size_t i = 0U; i < count; i++)
    {
        entries[i].channel = st.channels[i];
        entries[i].count = st.packet_count[i];
        entries[i].rssi_avg_dbm = st.rssi_avg_dbm[i];
        entries[i].rssi_max_dbm = st.rssi_max_dbm[i];
        total += entries[i].count;
    }

    for(size_t i = 0U; i < count; i++)
    {
        entries[i].pct = (total > 0U) ? (uint8_t)(((uint64_t)entries[i].count * 100ULL) / (uint64_t)total) : 0U;
    }

    cli_apps_scanner_sort_entries_desc_(entries, count);
    printf("scanner wifi channels: total=%lu current=%u hop_ms=%lu\n",
           (unsigned long)total,
           (unsigned)st.current_channel,
           (unsigned long)st.hop_ms);

    for(size_t i = 0U; i < count; i++)
    {
        printf("%u) ch=%u count=%lu pct=%u%% rssi_avg=%ddBm rssi_max=%ddBm\n",
               (unsigned)i,
               (unsigned)entries[i].channel,
               (unsigned long)entries[i].count,
               (unsigned)entries[i].pct,
               (int)entries[i].rssi_avg_dbm,
               (int)entries[i].rssi_max_dbm);
    }

    return 0;
}

/**
 * @brief Internal helper for `cli_apps_scanner_print_ieee_channels`.
 *
 * @return int
 */
static int cli_apps_scanner_print_ieee_channels_(void)
{
    poom_scanner_core_ieee802154_stats_t st = {0};
    cli_apps_scanner_entry_t entries[POOM_SCANNER_CORE_IEEE802154_CH_COUNT] = {{0}};
    uint32_t total = 0U;
    size_t count = 0U;

    if(!poom_scanner_core_get_ieee802154_stats(&st))
    {
        printf("scanner-channels: cannot read IEEE 802.15.4 stats\n");
        return 1;
    }

    count = (size_t)(st.channel_max - st.channel_min + 1U);
    if(count > POOM_SCANNER_CORE_IEEE802154_CH_COUNT)
    {
        count = POOM_SCANNER_CORE_IEEE802154_CH_COUNT;
    }

    for(size_t i = 0U; i < count; i++)
    {
        entries[i].channel = (uint8_t)(st.channel_min + (uint8_t)i);
        entries[i].count = st.packet_count[i];
        entries[i].rssi_avg_dbm = st.rssi_avg_dbm[i];
        entries[i].rssi_max_dbm = st.rssi_max_dbm[i];
        total += entries[i].count;
    }

    for(size_t i = 0U; i < count; i++)
    {
        entries[i].pct = (total > 0U) ? (uint8_t)(((uint64_t)entries[i].count * 100ULL) / (uint64_t)total) : 0U;
    }

    cli_apps_scanner_sort_entries_desc_(entries, count);
    printf("scanner ieee802154 channels: total=%lu current=%u hop_ms=%lu\n",
           (unsigned long)total,
           (unsigned)st.current_channel,
           (unsigned long)st.hop_ms);

    for(size_t i = 0U; i < count; i++)
    {
        printf("%u) ch=%u count=%lu pct=%u%% rssi_avg=%ddBm rssi_max=%ddBm\n",
               (unsigned)i,
               (unsigned)entries[i].channel,
               (unsigned long)entries[i].count,
               (unsigned)entries[i].pct,
               (int)entries[i].rssi_avg_dbm,
               (int)entries[i].rssi_max_dbm);
    }

    return 0;
}

/**
 * @brief Internal helper for `cli_apps_ir_proto_name`.
 *
 * @param[in] protocol Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_ir_proto_name_(uint8_t protocol)
{
    return ir_protocol_name((ir_protocol_t)protocol);
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] max_value Parameter passed to the function.
 * @param[in] out_value Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_parse_u32_(const char* text, uint32_t max_value, uint32_t* out_value)
{
    char* end_ptr = NULL;
    unsigned long value = 0UL;

    if((text == NULL) || (out_value == NULL) || (text[0] == '\0'))
    {
        return false;
    }

    errno = 0;
    value = strtoul(text, &end_ptr, 0);
    if((errno != 0) || (end_ptr == text) || ((end_ptr != NULL) && (end_ptr[0] != '\0')) || (value > max_value))
    {
        return false;
    }

    *out_value = (uint32_t)value;
    return true;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_filter Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_parse_sniffer_filter_(const char* text, poom_sniffer_device_filter_t* out_filter)
{
    if((text == NULL) || (out_filter == NULL))
    {
        return false;
    }

    if(strcasecmp(text, "probe") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_PROBE;
        return true;
    }
    if(strcasecmp(text, "beacon") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_BEACON;
        return true;
    }
    if(strcasecmp(text, "mgmt") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_MGMT;
        return true;
    }
    if(strcasecmp(text, "data") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_DATA;
        return true;
    }
    if(strcasecmp(text, "ctrl") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_CTRL;
        return true;
    }
    if(strcasecmp(text, "all") == 0)
    {
        *out_filter = POOM_SNIFFER_DEVICE_FILTER_ALL;
        return true;
    }

    return false;
}

/**
 * @brief Internal helper for `cli_apps_sniffer_filter_name`.
 *
 * @param[in] filter Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_sniffer_filter_name_(poom_sniffer_device_filter_t filter)
{
    switch(filter)
    {
        case POOM_SNIFFER_DEVICE_FILTER_BEACON: return "beacon";
        case POOM_SNIFFER_DEVICE_FILTER_MGMT: return "mgmt";
        case POOM_SNIFFER_DEVICE_FILTER_DATA: return "data";
        case POOM_SNIFFER_DEVICE_FILTER_CTRL: return "ctrl";
        case POOM_SNIFFER_DEVICE_FILTER_ALL: return "all";
        case POOM_SNIFFER_DEVICE_FILTER_PROBE:
        default:
            return "probe";
    }
}

/**
 * @brief Internal helper for `cli_apps_ir_slot_key`.
 *
 * @param[in] slot Parameter passed to the function.
 * @return const char*
 */
static const char* cli_apps_ir_slot_key_(const char* slot)
{
    if(slot == NULL)
    {
        return NULL;
    }

    if((strcasecmp(slot, "a") == 0) || (strcasecmp(slot, "btn_a") == 0))
    {
        return "ir_a";
    }
    if((strcasecmp(slot, "b") == 0) || (strcasecmp(slot, "btn_b") == 0))
    {
        return "ir_b";
    }
    if((strcasecmp(slot, "up") == 0) || (strcasecmp(slot, "u") == 0))
    {
        return "ir_u";
    }
    if((strcasecmp(slot, "down") == 0) || (strcasecmp(slot, "d") == 0))
    {
        return "ir_d";
    }
    if((strcasecmp(slot, "left") == 0) || (strcasecmp(slot, "l") == 0))
    {
        return "ir_l";
    }
    if((strcasecmp(slot, "right") == 0) || (strcasecmp(slot, "r") == 0))
    {
        return "ir_r";
    }

    return NULL;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] key Parameter passed to the function.
 * @param[in] out_code Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ir_load_code_(const char* key, cli_apps_ir_code_t* out_code)
{
    esp_err_t err;
    size_t blob_len = 0U;
    cli_apps_ir_code_legacy_t legacy = {0};
    uint8_t raw[4] = {0};

    if((key == NULL) || (out_code == NULL))
    {
        return false;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return false;
    }

    err = poom_secrets_get_blob(key, NULL, &blob_len);
    if(err != ESP_OK)
    {
        return false;
    }

    if(blob_len == sizeof(*out_code))
    {
        err = poom_secrets_get_blob(key, out_code, &blob_len);
        if((err != ESP_OK) || (blob_len != sizeof(*out_code)))
        {
            return false;
        }
    }
    else if(blob_len == sizeof(legacy))
    {
        size_t tmp_len = sizeof(legacy);
        err = poom_secrets_get_blob(key, &legacy, &tmp_len);
        if((err != ESP_OK) || (tmp_len != sizeof(legacy)))
        {
            return false;
        }

        out_code->protocol = legacy.protocol;
        out_code->flags = 0U;
        out_code->reserved = legacy.reserved;
        out_code->address = legacy.address;
        out_code->command = legacy.command;
    }
    else if(blob_len == sizeof(raw))
    {
        size_t tmp_len = sizeof(raw);
        err = poom_secrets_get_blob(key, raw, &tmp_len);
        if((err != ESP_OK) || (tmp_len != sizeof(raw)))
        {
            return false;
        }

        out_code->protocol = raw[0];
        out_code->flags = 0U;
        out_code->reserved = raw[3];
        out_code->address = raw[1];
        out_code->command = raw[2];
    }
    else
    {
        return false;
    }

    return ir_protocol_is_supported((ir_protocol_t)out_code->protocol);
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] key Parameter passed to the function.
 * @param[in] code Parameter passed to the function.
 * @return bool
 */
static bool cli_apps_ir_save_code_(const char* key, const cli_apps_ir_code_t* code)
{
    esp_err_t err;

    if((key == NULL) || (code == NULL))
    {
        return false;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return false;
    }

    return poom_secrets_set_blob(key, code, sizeof(*code)) == ESP_OK;
}

/**
 * @brief Internal helper for `cli_apps_ir_send_code`.
 *
 * @param[in] code Parameter passed to the function.
 * @return int
 */
static int cli_apps_ir_send_code_(const cli_apps_ir_code_t* code)
{
#if !defined(PIN_NUM_IR_TX)
    (void)code;
    printf("IR TX not supported on this board\n");
    return 1;
#else
    esp_err_t err;
    ir_tx_handle_t transmitter = {0};
    ir_tx_config_t cfg = ir_tx_default_config();

    if(code == NULL)
    {
        return 1;
    }

    cfg.gpio = PIN_NUM_IR_TX;
    cfg.clk_hz = 1000000U;
    cfg.carrier_hz = 38000U;
    cfg.duty_cycle = 0.33f;

    err = ir_tx_init(&transmitter, &cfg, "cli_ir_tx");
    if(err != ESP_OK)
    {
        printf("ir tx init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = ir_tx_send(&transmitter, (ir_protocol_t)code->protocol, code->address, code->command);

    (void)ir_tx_deinit(&transmitter);

    if(err != ESP_OK)
    {
        printf("ir send failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("ir sent: proto=%s addr=0x%08" PRIX32 " cmd=0x%08" PRIX32 "\n",
           cli_apps_ir_proto_name_(code->protocol),
           code->address,
           code->command);
    return 0;
#endif
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return int
 */
static int cli_apps_stop_all_(void)
{
    bool lua_running = false;

    cli_apps_stop_ble_gatt_();
    cli_apps_stop_wifi_spam_();
    cli_apps_stop_karma_();
    cli_apps_stop_deauth_();
    cli_apps_stop_deauth_detector_();
    cli_apps_stop_captive_();
    cli_apps_stop_scanner_();
    cli_apps_stop_sniffer_device_();
    cli_apps_lua_upload_abort_(true);
    cli_apps_stop_wifi_sta_(NULL);
    cli_apps_stop_ble_spam_();
    cli_apps_stop_ble_tracker_();

    lua_running = poom_lua_is_running();
    if(lua_running)
    {
        (void)poom_lua_request_stop();
    }

    printf("Stopped active CLI apps");
    if(lua_running)
    {
        printf(" + requested Lua stop");
    }
    printf("\n");
    return 0;
}

void cli_poom_apps_stop_all(void)
{
    (void)cli_apps_stop_all_();
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cli_stop_all(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return cli_apps_stop_all_();
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_start(int argc, char** argv)
{
    esp_err_t err;
    bool running = false;

    (void)argc;
    (void)argv;

    if(poom_wifi_spam_get_running(&running) == ESP_OK && running)
    {
        printf("wifi-spam already running\n");
        return 0;
    }

    cli_apps_prepare_wifi_attack_();

    err = poom_wifi_spam_start();
    if(err != ESP_OK)
    {
        printf("wifi-spam-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi-spam running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_stop(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_spam_stop();
    if(err != ESP_OK)
    {
        printf("wifi-spam-stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi-spam stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_spam_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_status(int argc, char** argv)
{
    esp_err_t err;
    bool running = false;
    poom_wifi_spam_ssid_list_t list = {0};

    (void)argc;
    (void)argv;

    err = poom_wifi_spam_get_running(&running);
    if(err != ESP_OK)
    {
        printf("wifi-spam-status failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_wifi_spam_ssids_get(&list);
    if(err != ESP_OK)
    {
        printf("wifi-spam-status list failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi-spam: %s | ssids=%u\n", running ? "RUNNING" : "STOPPED", (unsigned)list.count);
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_reset`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_reset(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_ble_gatt_();
    if(!cli_apps_ble_gatt_reset_cfg_())
    {
        printf("ble-gatt reset failed: no memory\n");
        return 1;
    }
    printf("ble-gatt reset\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_name_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_name_set(int argc, char** argv)
{
    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-name-set failed: no memory\n");
        return 1;
    }

    if(argc != 2)
    {
        printf("Usage: ble-gatt-name-set <name>\n");
        return 1;
    }

    (void)snprintf(s_cli_apps_ble_gatt_cfg.name, sizeof(s_cli_apps_ble_gatt_cfg.name), "%s", argv[1]);
    printf("ble-gatt name=%s%s\n",
           s_cli_apps_ble_gatt_cfg.name,
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_service_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_service_set(int argc, char** argv)
{
    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-service-set failed: no memory\n");
        return 1;
    }

    if((argc != 2) || !cli_apps_parse_uuid_(argv[1], &s_cli_apps_ble_gatt_cfg.service_uuid))
    {
        printf("Usage: ble-gatt-service-set <uuid16|uuid128>\n");
        return 1;
    }

    printf("ble-gatt service updated%s\n",
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_set(int argc, char** argv)
{
    cli_apps_ble_gatt_char_cfg_t* char_cfg;
    esp_bt_uuid_t uuid = {0};
    size_t char_index = 0U;
    const char* uuid_text;
    const char* mode_text;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-set failed: no memory\n");
        return 1;
    }

    if(argc == 3)
    {
        uuid_text = argv[1];
        mode_text = argv[2];
    }
    else if(argc == 4)
    {
        if(!cli_apps_parse_size_t_(argv[1], &char_index))
        {
            printf("Usage: ble-gatt-char-set [index] <uuid16|uuid128> <read|write|readwrite>\n");
            return 1;
        }
        uuid_text = argv[2];
        mode_text = argv[3];
    }
    else
    {
        printf("Usage: ble-gatt-char-set [index] <uuid16|uuid128> <read|write|readwrite>\n");
        return 1;
    }

    char_cfg = cli_apps_ble_gatt_char_get_(char_index);
    if((char_cfg == NULL) || !cli_apps_parse_uuid_(uuid_text, &uuid) || !cli_apps_ble_gatt_char_set_mode_(char_cfg, mode_text))
    {
        printf("Usage: ble-gatt-char-set [index] <uuid16|uuid128> <read|write|readwrite>\n");
        return 1;
    }

    char_cfg->char_uuid = uuid;

    printf("ble-gatt characteristic updated%s\n",
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_value`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_value(int argc, char** argv)
{
    cli_apps_ble_gatt_char_cfg_t* char_cfg;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-value failed: no memory\n");
        return 1;
    }

    char_cfg = cli_apps_ble_gatt_char_get_(0U);
    if(char_cfg == NULL)
    {
        printf("ble-gatt-char-value failed: missing characteristic 0\n");
        return 1;
    }

    if((argc < 2) || ((argc - 1) > (int)char_cfg->value_capacity))
    {
        printf("Usage: ble-gatt-char-value <hex...>\n");
        return 1;
    }

    for(int i = 1; i < argc; i++)
    {
        if(!cli_apps_parse_hex_byte_(argv[i], &char_cfg->value[i - 1]))
        {
            printf("Invalid hex byte: %s\n", argv[i]);
            return 1;
        }
    }

    char_cfg->value_len = (uint16_t)(argc - 1);
    printf("ble-gatt value-bytes=%u%s\n",
           (unsigned)char_cfg->value_len,
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_add`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_add(int argc, char** argv)
{
    esp_bt_uuid_t uuid = {0};

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-add failed: no memory\n");
        return 1;
    }

    if((argc != 3) || !cli_apps_parse_uuid_(argv[1], &uuid) || !cli_apps_ble_gatt_char_add_(&uuid, argv[2]))
    {
        printf("Usage: ble-gatt-char-add <uuid16|uuid128> <read|write|readwrite>\n");
        return 1;
    }

    printf("ble-gatt characteristic added index=%u%s\n",
           (unsigned)(s_cli_apps_ble_gatt_cfg.char_count - 1U),
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_del`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_del(int argc, char** argv)
{
    size_t char_index = 0U;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-del failed: no memory\n");
        return 1;
    }

    if((argc != 2) || !cli_apps_parse_size_t_(argv[1], &char_index))
    {
        printf("Usage: ble-gatt-char-del <index>\n");
        return 1;
    }

    if(s_cli_apps_ble_gatt_cfg.char_count <= 1U)
    {
        printf("ble-gatt-char-del needs at least one characteristic\n");
        return 1;
    }

    if(!cli_apps_ble_gatt_char_del_(char_index))
    {
        printf("ble-gatt-char-del invalid index=%u\n", (unsigned)char_index);
        return 1;
    }

    printf("ble-gatt characteristic removed index=%u%s\n",
           (unsigned)char_index,
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_list`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_list(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-list failed: no memory\n");
        return 1;
    }

    printf("ble-gatt chars=%u\n", (unsigned)s_cli_apps_ble_gatt_cfg.char_count);
    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        cli_apps_ble_gatt_print_char_(i, &s_cli_apps_ble_gatt_cfg.chars[i]);
    }

    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_char_value_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_char_value_set(int argc, char** argv)
{
    cli_apps_ble_gatt_char_cfg_t* char_cfg;
    size_t char_index = 0U;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-char-value-set failed: no memory\n");
        return 1;
    }

    if((argc < 3) || !cli_apps_parse_size_t_(argv[1], &char_index))
    {
        printf("Usage: ble-gatt-char-value-set <index> <hex...>\n");
        return 1;
    }

    char_cfg = cli_apps_ble_gatt_char_get_(char_index);
    if(char_cfg == NULL)
    {
        printf("ble-gatt-char-value-set invalid index=%u\n", (unsigned)char_index);
        return 1;
    }

    if((argc - 2) > (int)char_cfg->value_capacity)
    {
        printf("ble-gatt-char-value-set max-bytes=%u\n", (unsigned)char_cfg->value_capacity);
        return 1;
    }

    for(int i = 2; i < argc; i++)
    {
        if(!cli_apps_parse_hex_byte_(argv[i], &char_cfg->value[i - 2]))
        {
            printf("Invalid hex byte: %s\n", argv[i]);
            return 1;
        }
    }

    char_cfg->value_len = (uint16_t)(argc - 2);
    printf("ble-gatt char[%u] value-bytes=%u%s\n",
           (unsigned)char_index,
           (unsigned)char_cfg->value_len,
           poom_ble_gatt_dynamic_is_started() ? " (restart to apply)" : "");
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_start(int argc, char** argv)
{
    poom_ble_gatt_dynamic_config_t params = {0};
    poom_ble_gatt_dynamic_char_config_t* chars = NULL;
    esp_err_t err;

    (void)argc;
    (void)argv;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-start failed: no memory\n");
        return 1;
    }

    if(poom_ble_gatt_dynamic_is_started())
    {
        printf("ble-gatt already running\n");
        return 0;
    }

    if(s_cli_apps_ble_spam_running)
    {
        printf("Stopping ble-spam due to BLE radio conflict\n");
        cli_apps_stop_ble_spam_();
    }

    if(poom_ble_tracker_is_active())
    {
        printf("Stopping ble-tracker due to BLE radio conflict\n");
        cli_apps_stop_ble_tracker_();
    }

    params.adv_params = poom_ble_gatt_dynamic_default_adv_params();
    params.bt_props.device_name = s_cli_apps_ble_gatt_cfg.name;
    params.profile.service_uuid = s_cli_apps_ble_gatt_cfg.service_uuid;
    params.profile.char_count = s_cli_apps_ble_gatt_cfg.char_count;

    chars = (poom_ble_gatt_dynamic_char_config_t*)calloc(
        s_cli_apps_ble_gatt_cfg.char_count,
        sizeof(*chars));
    if(chars == NULL)
    {
        printf("ble-gatt-start failed: no memory\n");
        return 1;
    }

    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        chars[i].char_uuid = s_cli_apps_ble_gatt_cfg.chars[i].char_uuid;
        chars[i].char_perm = s_cli_apps_ble_gatt_cfg.chars[i].char_perm;
        chars[i].char_property = s_cli_apps_ble_gatt_cfg.chars[i].char_property;
        chars[i].char_val.attr_max_len = s_cli_apps_ble_gatt_cfg.chars[i].value_capacity;
        chars[i].char_val.attr_len = s_cli_apps_ble_gatt_cfg.chars[i].value_len;
        chars[i].char_val.attr_value = s_cli_apps_ble_gatt_cfg.chars[i].value;
    }
    params.profile.chars = chars;

    poom_ble_gatt_dynamic_set_config(&params);
    free(chars);
    err = poom_ble_gatt_dynamic_start();
    if(err != ESP_OK)
    {
        printf("ble-gatt-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("ble-gatt running\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_gatt_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_status(int argc, char** argv)
{
    char service_uuid[40];

    (void)argc;
    (void)argv;

    if(!cli_apps_ble_gatt_ensure_cfg_())
    {
        printf("ble-gatt-status failed: no memory\n");
        return 1;
    }
    cli_apps_format_uuid_(&s_cli_apps_ble_gatt_cfg.service_uuid, service_uuid, sizeof(service_uuid));

    printf("ble-gatt: %s | name=%s | service=%s | chars=%u\n",
           poom_ble_gatt_dynamic_is_started() ? "RUNNING" : "STOPPED",
           s_cli_apps_ble_gatt_cfg.name,
           service_uuid,
           (unsigned)s_cli_apps_ble_gatt_cfg.char_count);

    for(size_t i = 0U; i < s_cli_apps_ble_gatt_cfg.char_count; i++)
    {
        cli_apps_ble_gatt_print_char_(i, &s_cli_apps_ble_gatt_cfg.chars[i]);
    }
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_gatt_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_ble_gatt_();
    printf("ble-gatt stopped\n");
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_spam_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(s_cli_apps_ble_spam_running)
    {
        printf("ble-spam already running\n");
        return 0;
    }

    if(poom_ble_gatt_dynamic_is_started())
    {
        printf("Stopping ble-gatt due to BLE radio conflict\n");
        cli_apps_stop_ble_gatt_();
    }

    if(poom_ble_tracker_is_active())
    {
        printf("Stopping ble-tracker due to BLE radio conflict\n");
        cli_apps_stop_ble_tracker_();
    }

    if(cli_apps_wifi_runtime_active_())
    {
        cli_apps_stop_wifi_runtime_for_ble_();
    }

    poom_ble_spam_register_cb(cli_apps_ble_spam_name_cb_);
    poom_ble_spam_start();
    s_cli_apps_ble_spam_running = true;
    printf("ble-spam running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_spam_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_ble_spam_();
    printf("ble-spam stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_spam_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_spam_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("ble-spam: %s | current=%s\n",
           s_cli_apps_ble_spam_running ? "RUNNING" : "STOPPED",
           s_cli_apps_ble_spam_name);
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_spam_current`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_spam_current(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("ble-spam current: %s\n", s_cli_apps_ble_spam_name);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(poom_ble_tracker_is_active())
    {
        printf("ble-tracker already running\n");
        return 0;
    }

    if(poom_ble_gatt_dynamic_is_started())
    {
        printf("Stopping ble-gatt due to BLE radio conflict\n");
        cli_apps_stop_ble_gatt_();
    }

    if(s_cli_apps_ble_spam_running)
    {
        printf("Stopping ble-spam due to BLE radio conflict\n");
        cli_apps_stop_ble_spam_();
    }

    if(cli_apps_wifi_runtime_active_())
    {
        cli_apps_stop_wifi_runtime_for_ble_();
    }

    cli_apps_ble_tracker_reset_cache_();
    poom_ble_tracker_register_scan_callback(cli_apps_ble_tracker_cb_);
    poom_ble_tracker_start();
    printf("ble-tracker running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_ble_tracker_();
    printf("ble-tracker stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_tracker_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("ble-tracker: %s | cached=%u\n",
           poom_ble_tracker_is_active() ? "RUNNING" : "STOPPED",
           (unsigned)s_cli_apps_ble_tracker_profile_count);
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_tracker_list`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_list(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(s_cli_apps_ble_tracker_profile_count == 0U)
    {
        printf("ble-tracker: no tracker records\n");
        return 0;
    }

    for(uint16_t i = 0U; i < s_cli_apps_ble_tracker_profile_count; i++)
    {
        const poom_ble_tracker_profile_t* p = &s_cli_apps_ble_tracker_profiles[i];
        printf("%u) %s | %s | %02X:%02X:%02X:%02X:%02X:%02X | %ddBm | %.2fm\n",
               (unsigned)i,
               (p->name != NULL) ? p->name : "(unknown)",
               (p->vendor != NULL) ? p->vendor : "-",
               (unsigned)p->mac_address[0],
               (unsigned)p->mac_address[1],
               (unsigned)p->mac_address[2],
               (unsigned)p->mac_address[3],
               (unsigned)p->mac_address[4],
               (unsigned)p->mac_address[5],
               p->rssi,
               (double)p->distance_m);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_tracker_clear`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_ble_tracker_reset_cache_();
    printf("ble-tracker cache cleared\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_ble_tracker_scan`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ble_tracker_scan(int argc, char** argv)
{
    return cmd_ble_tracker_start(argc, argv);
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_start(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    if(s_cli_apps_karma_running)
    {
        printf("karma already running\n");
        return 0;
    }

    cli_apps_prepare_wifi_attack_();

    err = poom_wifi_karma_start();
    if(err != ESP_OK)
    {
        printf("karma-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    s_cli_apps_karma_running = true;
    printf("karma running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_karma_();
    printf("karma stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_karma_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_status(int argc, char** argv)
{
    char active_ssid[33] = {0};
    char ssids[CLI_APPS_KARMA_LIST_MAX][33] = {{0}};
    const int count = poom_wifi_karma_get_discovered_ssids(ssids, CLI_APPS_KARMA_LIST_MAX);

    (void)argc;
    (void)argv;

    if(!poom_wifi_karma_get_active_ssid(active_ssid, sizeof(active_ssid)))
    {
        (void)snprintf(active_ssid, sizeof(active_ssid), "-");
    }

    printf("karma: %s | discovered=%d | active=%s\n",
           s_cli_apps_karma_running ? "RUNNING" : "STOPPED",
           count,
           active_ssid);
    return 0;
}

/**
 * @brief Internal helper for `cmd_karma_ssids_list`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_ssids_list(int argc, char** argv)
{
    char ssids[CLI_APPS_KARMA_LIST_MAX][33] = {{0}};
    const int count = poom_wifi_karma_get_discovered_ssids(ssids, CLI_APPS_KARMA_LIST_MAX);

    (void)argc;
    (void)argv;

    if(count <= 0)
    {
        printf("karma: no discovered SSIDs\n");
        return 0;
    }

    for(int i = 0; i < count; i++)
    {
        printf("%d) %s\n", i, ssids[i]);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_karma_active_ssid`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_active_ssid(int argc, char** argv)
{
    char active_ssid[33] = {0};

    (void)argc;
    (void)argv;

    if(!poom_wifi_karma_get_active_ssid(active_ssid, sizeof(active_ssid)))
    {
        printf("karma active ssid: -\n");
        return 0;
    }

    printf("karma active ssid: %s\n", active_ssid);
    return 0;
}

/**
 * @brief Internal helper for `cmd_karma_clear`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_karma_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_karma_();
    printf("karma state cleared via stop\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_lua_run`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_run(int argc, char** argv)
{
    const char* path = "/sdcard/main.lua";
    esp_err_t err;

    if(argc > 2)
    {
        printf("Usage: lua-run [abs_path]\n");
        return 1;
    }
    if(argc == 2)
    {
        path = argv[1];
    }

    if(s_cli_apps_lua_upload_active)
    {
        printf("lua-run failed: upload session still active\n");
        return 1;
    }

    err = poom_lua_run_file_async(path, NULL, NULL);
    if(err != ESP_OK)
    {
        printf("lua-run failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("lua running: %s\n", path);
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_stop(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_lua_request_stop();
    if(err != ESP_OK)
    {
        printf("lua-stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("lua stop requested\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_lua_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("lua: %s\n", poom_lua_is_running() ? "RUNNING" : "STOPPED");
    if(s_cli_apps_lua_upload_active)
    {
        printf("lua-upload: ACTIVE | path=%s | lines=%u | bytes=%u\n",
               s_cli_apps_lua_upload_path,
               (unsigned)s_cli_apps_lua_upload_lines,
               (unsigned)s_cli_apps_lua_upload_bytes);
    }
    else
    {
        printf("lua-upload: IDLE\n");
    }
    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_upload_begin(int argc, char** argv)
{
    esp_err_t err;

    if(argc != 2)
    {
        printf("Usage: lua-upload-begin </sdcard/file.lua>\n");
        return 1;
    }

    if(poom_lua_is_running())
    {
        printf("lua-upload-begin failed: stop Lua first\n");
        return 1;
    }

    if(s_cli_apps_lua_upload_active)
    {
        printf("lua-upload-begin failed: upload already active for %s\n", s_cli_apps_lua_upload_path);
        return 1;
    }

    if(!cli_apps_lua_upload_path_valid_(argv[1]))
    {
        printf("lua-upload-begin failed: path must be absolute under /sdcard\n");
        return 1;
    }

    err = cli_apps_sd_ensure_mounted_();
    if(err != ESP_OK)
    {
        printf("lua-upload-begin sd mount failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = cli_apps_ensure_parent_dirs_(argv[1]);
    if(err != ESP_OK)
    {
        printf("lua-upload-begin mkdir failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    s_cli_apps_lua_upload_file = fopen(argv[1], "wb");
    if(s_cli_apps_lua_upload_file == NULL)
    {
        printf("lua-upload-begin fopen failed: errno=%d\n", errno);
        cli_apps_lua_upload_reset_state_();
        return 1;
    }

    s_cli_apps_lua_upload_active = true;
    (void)snprintf(s_cli_apps_lua_upload_path, sizeof(s_cli_apps_lua_upload_path), "%s", argv[1]);
    s_cli_apps_lua_upload_bytes = 0U;
    s_cli_apps_lua_upload_lines = 0U;

    printf("lua-upload active: %s\n", s_cli_apps_lua_upload_path);
    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_upload_line(int argc, char** argv)
{
    if(!s_cli_apps_lua_upload_active || (s_cli_apps_lua_upload_file == NULL))
    {
        printf("lua-upload-line failed: no active upload\n");
        return 1;
    }

    if(argc < 1)
    {
        printf("Usage: lua-upload-line [text...]\n");
        return 1;
    }

    for(int i = 1; i < argc; i++)
    {
        if((i > 1) && (fputc(' ', s_cli_apps_lua_upload_file) == EOF))
        {
            printf("lua-upload-line failed: fwrite error\n");
            return 1;
        }

        if(fputs(argv[i], s_cli_apps_lua_upload_file) == EOF)
        {
            printf("lua-upload-line failed: fwrite error\n");
            return 1;
        }
    }

    if(fputc('\n', s_cli_apps_lua_upload_file) == EOF)
    {
        printf("lua-upload-line failed: fwrite error\n");
        return 1;
    }

    s_cli_apps_lua_upload_bytes += (argc > 1) ? (strlen(argv[1])) : 0U;
    for(int i = 2; i < argc; i++)
    {
        s_cli_apps_lua_upload_bytes += 1U + strlen(argv[i]);
    }
    s_cli_apps_lua_upload_bytes += 1U;
    s_cli_apps_lua_upload_lines++;

    printf("lua-upload line ok: line=%u total_bytes=%u\n",
           (unsigned)s_cli_apps_lua_upload_lines,
           (unsigned)s_cli_apps_lua_upload_bytes);
    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_upload_end(int argc, char** argv)
{
    char path[CLI_APPS_LUA_UPLOAD_PATH_MAX + 1U] = "";
    size_t bytes = 0U;
    size_t lines = 0U;

    (void)argv;

    if(argc != 1)
    {
        printf("Usage: lua-upload-end\n");
        return 1;
    }

    if(!s_cli_apps_lua_upload_active || (s_cli_apps_lua_upload_file == NULL))
    {
        printf("lua-upload-end failed: no active upload\n");
        return 1;
    }

    (void)snprintf(path, sizeof(path), "%s", s_cli_apps_lua_upload_path);
    bytes = s_cli_apps_lua_upload_bytes;
    lines = s_cli_apps_lua_upload_lines;

    if(fflush(s_cli_apps_lua_upload_file) != 0)
    {
        printf("lua-upload-end fflush failed: errno=%d\n", errno);
        cli_apps_lua_upload_abort_(true);
        return 1;
    }

    cli_apps_lua_upload_reset_state_();
    printf("lua-upload saved: %s | lines=%u | bytes=%u\n",
           path,
           (unsigned)lines,
           (unsigned)bytes);
    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_lua_upload_abort(int argc, char** argv)
{
    char path[CLI_APPS_LUA_UPLOAD_PATH_MAX + 1U] = "";

    (void)argv;

    if(argc != 1)
    {
        printf("Usage: lua-upload-abort\n");
        return 1;
    }

    if(!s_cli_apps_lua_upload_active)
    {
        printf("lua-upload-abort: no active upload\n");
        return 0;
    }

    (void)snprintf(path, sizeof(path), "%s", s_cli_apps_lua_upload_path);
    cli_apps_lua_upload_abort_(true);
    printf("lua-upload aborted: removed partial file %s\n", path);
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_scan`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_scan(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    cli_apps_prepare_wifi_base_();
    err = poom_wifi_scanner_clear_ap_records();
    if(err != ESP_OK)
    {
        printf("wifi-scan clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_wifi_scanner_scan();
    if(err != ESP_OK)
    {
        printf("wifi-scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    cli_apps_wifi_scan_print_();
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_scan_show`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_scan_show(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    cli_apps_wifi_scan_print_();
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_connect`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_connect(int argc, char** argv)
{
    uint32_t index = 0U;
    wifi_ap_record_t* ap = NULL;
    const char* password = NULL;
    esp_err_t err;
    char ip_buf[16] = {0};

    if((argc != 2) && (argc != 3))
    {
        printf("Usage: wifi-connect <index> [password]\n");
        return 1;
    }
    if(!cli_apps_parse_u32_(argv[1], POOM_WIFI_SCANNER_MAX_AP - 1U, &index))
    {
        printf("Invalid Wi-Fi scan index\n");
        return 1;
    }

    ap = poom_wifi_scanner_get_ap_record((unsigned)index);
    if(ap == NULL)
    {
        printf("wifi-connect: AP index not found, run wifi-scan first\n");
        return 1;
    }

    password = (argc == 3) ? argv[2] : NULL;
    cli_apps_prepare_wifi_base_();
    err = poom_wifi_ctrl_sta_connect((const char*)ap->ssid, ((password != NULL) && (password[0] != '\0')) ? password : NULL);
    if(err != ESP_OK)
    {
        printf("wifi-connect failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    cli_apps_remember_wifi_credentials_((const char*)ap->ssid, password);
    if(cli_apps_wifi_wait_ip_(CLI_APPS_WIFI_CONNECT_TIMEOUT_MS, ip_buf, sizeof(ip_buf)))
    {
        printf("wifi connected: ssid=%s ip=%s\n", (const char*)ap->ssid, ip_buf);
    }
    else
    {
        printf("wifi connect started: ssid=%s | waiting for IP\n", (const char*)ap->ssid);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_connect_ssid`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_connect_ssid(int argc, char** argv)
{
    const char* ssid = NULL;
    const char* password = NULL;
    esp_err_t err;
    char ip_buf[16] = {0};

    if((argc != 2) && (argc != 3))
    {
        printf("Usage: wifi-connect-ssid <ssid> [password]\n");
        return 1;
    }

    ssid = argv[1];
    password = (argc == 3) ? argv[2] : NULL;

    cli_apps_prepare_wifi_base_();
    err = poom_wifi_ctrl_sta_connect(ssid, ((password != NULL) && (password[0] != '\0')) ? password : NULL);
    if(err != ESP_OK)
    {
        printf("wifi-connect-ssid failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    cli_apps_remember_wifi_credentials_(ssid, password);
    if(cli_apps_wifi_wait_ip_(CLI_APPS_WIFI_CONNECT_TIMEOUT_MS, ip_buf, sizeof(ip_buf)))
    {
        printf("wifi connected: ssid=%s ip=%s\n", ssid, ip_buf);
    }
    else
    {
        printf("wifi connect started: ssid=%s | waiting for IP\n", ssid);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_disconnect`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_disconnect(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_ctrl_sta_disconnect();
    if(err != ESP_OK)
    {
        printf("wifi-disconnect failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi disconnected\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_status(int argc, char** argv)
{
    char ssid[33] = {0};
    char ip_buf[16] = {0};

    (void)argc;
    (void)argv;

    if(!cli_apps_wifi_connection_active_())
    {
        printf("wifi: STOPPED | ssid=- | ip=-\n");
        return 0;
    }

    if(!cli_apps_wifi_get_connected_ssid_(ssid, sizeof(ssid)))
    {
        (void)snprintf(ssid, sizeof(ssid), "%s", s_cli_apps_wifi_last_known ? s_cli_apps_wifi_last_ssid : "-");
    }
    if(!cli_apps_wifi_get_ip_(ip_buf, sizeof(ip_buf)))
    {
        (void)snprintf(ip_buf, sizeof(ip_buf), "-");
    }

    printf("wifi: %s | ip=%s | ssid=%s\n",
           poom_wifi_ctrl_sta_has_ip() ? "CONNECTED" : "CONNECTING",
           ip_buf,
           ssid);
    return 0;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_save_current(int argc, char** argv)
{
    char ssid[33] = {0};
    esp_err_t err;

    (void)argc;
    (void)argv;

    if(!cli_apps_wifi_connection_active_())
    {
        printf("wifi-save-current: not connected\n");
        return 1;
    }

    if(!cli_apps_wifi_get_connected_ssid_(ssid, sizeof(ssid)))
    {
        printf("wifi-save-current: cannot read current SSID\n");
        return 1;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("wifi-save-current init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_wifi_ssid(ssid);
    if(err != ESP_OK)
    {
        printf("wifi-save-current ssid failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if(!s_cli_apps_wifi_last_known || (strcmp(ssid, s_cli_apps_wifi_last_ssid) != 0))
    {
        err = poom_secrets_set_wifi_pass("");
        if(err != ESP_OK)
        {
            printf("wifi-save-current pass failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("wifi saved: ssid=%s | password not known, saved empty pass\n", ssid);
        return 0;
    }

    err = poom_secrets_set_wifi_pass(s_cli_apps_wifi_last_pass);
    if(err != ESP_OK)
    {
        printf("wifi-save-current pass failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi saved: ssid=%s\n", ssid);
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_scan`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_scan(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    cli_apps_prepare_wifi_base_();
    err = poom_wifi_deauth_scan_and_list();
    if(err != ESP_OK)
    {
        printf("deauth-scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_attack`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_attack(int argc, char** argv)
{
    uint32_t index = 0U;
    esp_err_t err;

    if(argc != 2)
    {
        printf("Usage: deauth-attack <index>\n");
        return 1;
    }
    if(!cli_apps_parse_u32_(argv[1], POOM_WIFI_SCANNER_MAX_AP - 1U, &index))
    {
        printf("Invalid deauth AP index\n");
        return 1;
    }

    cli_apps_prepare_wifi_attack_();
    err = poom_wifi_deauth_attack((int)index);
    if(err != ESP_OK)
    {
        printf("deauth-attack failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth attack launched for index=%u\n", (unsigned)index);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_start(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    cli_apps_prepare_wifi_attack_();
    err = poom_wifi_deauth_start();
    if(err != ESP_OK)
    {
        printf("deauth-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_stop(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_deauth_stop();
    if(err != ESP_OK)
    {
        printf("deauth-stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("deauth: %s\n", poom_wifi_deauth_is_running() ? "RUNNING" : "STOPPED");
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_start(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    cli_apps_prepare_wifi_attack_();
    err = poom_wifi_deauth_detector_start();
    if(err != ESP_OK)
    {
        printf("deauth-det-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector running\n");
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_stop(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_deauth_detector_stop();
    if(err != ESP_OK)
    {
        printf("deauth-det-stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_det_stats`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_stats(int argc, char** argv)
{
    poom_wifi_deauth_detector_stats_t stats = {0};
    poom_wifi_deauth_detector_report_t report = {0};
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_deauth_detector_get_stats(&stats);
    if(err != ESP_OK)
    {
        printf("deauth-det-stats stats failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_wifi_deauth_detector_get_report(&report);
    if(err != ESP_OK)
    {
        printf("deauth-det-stats report failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector: %s | ch=%u | hop=%s | deauth=%u | disassoc=%u | pps=%u/%u | alert=%u\n",
           poom_wifi_deauth_detector_is_running() ? "RUNNING" : "STOPPED",
           (unsigned)stats.current_channel,
           stats.channel_hopping ? "ON" : "OFF",
           (unsigned)stats.deauth_total,
           (unsigned)stats.disassoc_total,
           (unsigned)report.deauth_pps,
           (unsigned)report.disassoc_pps,
           (unsigned)report.alert);

    if(report.top_valid)
    {
        printf("top: src=%02X:%02X:%02X:%02X:%02X:%02X bssid=%02X:%02X:%02X:%02X:%02X:%02X ch=%u pps=%u victims=%u reason=%u count=%u\n",
               (unsigned)report.top_src[0],
               (unsigned)report.top_src[1],
               (unsigned)report.top_src[2],
               (unsigned)report.top_src[3],
               (unsigned)report.top_src[4],
               (unsigned)report.top_src[5],
               (unsigned)report.top_bssid[0],
               (unsigned)report.top_bssid[1],
               (unsigned)report.top_bssid[2],
               (unsigned)report.top_bssid[3],
               (unsigned)report.top_bssid[4],
               (unsigned)report.top_bssid[5],
               (unsigned)report.top_channel,
               (unsigned)report.top_pps,
               (unsigned)report.top_unique_victims,
               (unsigned)report.top_reason,
               (unsigned)report.top_reason_count);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_det_reset`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_reset(int argc, char** argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_wifi_deauth_detector_reset_stats();
    if(err != ESP_OK)
    {
        printf("deauth-det-reset failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector stats reset\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_det_hop`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_hop(int argc, char** argv)
{
    uint32_t enabled = 0U;
    esp_err_t err;

    if(argc != 2)
    {
        printf("Usage: deauth-det-hop <0|1>\n");
        return 1;
    }
    if(!cli_apps_parse_u32_(argv[1], 1U, &enabled))
    {
        printf("Invalid deauth-det-hop value\n");
        return 1;
    }

    err = poom_wifi_deauth_detector_set_channel_hopping(enabled != 0U);
    if(err != ESP_OK)
    {
        printf("deauth-det-hop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector hopping: %s\n", (enabled != 0U) ? "ON" : "OFF");
    return 0;
}

/**
 * @brief Internal helper for `cmd_deauth_det_ch`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_deauth_det_ch(int argc, char** argv)
{
    uint32_t channel = 0U;
    esp_err_t err;

    if(argc != 2)
    {
        printf("Usage: deauth-det-ch <channel>\n");
        return 1;
    }
    if(!cli_apps_parse_u32_(argv[1], POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT, &channel) || (channel == 0U))
    {
        printf("Invalid deauth-det channel\n");
        return 1;
    }

    err = poom_wifi_deauth_detector_set_fixed_channel((uint8_t)channel);
    if(err != ESP_OK)
    {
        printf("deauth-det-ch failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("deauth-detector fixed channel=%u\n", (unsigned)channel);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_captive_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(argc > 2)
    {
        printf("Usage: captive-start [portal_file]\n");
        return 1;
    }

    cli_apps_prepare_wifi_attack_();
    poom_wifi_captive_set_ap_clone(NULL, false);
    if(argc == 2)
    {
        poom_wifi_captive_set_portal_file(argv[1]);
        (void)snprintf(s_cli_apps_captive_portal_file, sizeof(s_cli_apps_captive_portal_file), "%s", argv[1]);
    }
    else
    {
        s_cli_apps_captive_portal_file[0] = '\0';
    }

    poom_wifi_captive_start();
    s_cli_apps_captive_running = true;
    s_cli_apps_captive_clone_ssid[0] = '\0';
    printf("captive running\n");
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_captive_start_from_scan(int argc, char** argv)
{
    uint32_t index = 0U;
    wifi_ap_record_t* ap = NULL;

    if((argc != 2) && (argc != 3))
    {
        printf("Usage: captive-start-from-scan <index> [portal_file]\n");
        return 1;
    }
    if(!cli_apps_parse_u32_(argv[1], POOM_WIFI_SCANNER_MAX_AP - 1U, &index))
    {
        printf("Invalid captive AP index\n");
        return 1;
    }

    ap = poom_wifi_scanner_get_ap_record((unsigned)index);
    if(ap == NULL)
    {
        printf("captive-start-from-scan: AP index not found, run wifi-scan first\n");
        return 1;
    }

    cli_apps_prepare_wifi_attack_();
    poom_wifi_captive_set_ap_clone((const char*)ap->ssid, true);
    (void)snprintf(s_cli_apps_captive_clone_ssid, sizeof(s_cli_apps_captive_clone_ssid), "%s", (const char*)ap->ssid);
    if(argc == 3)
    {
        poom_wifi_captive_set_portal_file(argv[2]);
        (void)snprintf(s_cli_apps_captive_portal_file, sizeof(s_cli_apps_captive_portal_file), "%s", argv[2]);
    }
    else
    {
        s_cli_apps_captive_portal_file[0] = '\0';
    }

    poom_wifi_captive_start();
    s_cli_apps_captive_running = true;
    printf("captive running clone_ssid=%s\n", s_cli_apps_captive_clone_ssid);
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_captive_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_captive_();
    printf("captive stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_captive_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_captive_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("captive: %s | clone_ssid=%s | portal=%s\n",
           s_cli_apps_captive_running ? "RUNNING" : "STOPPED",
           (s_cli_apps_captive_clone_ssid[0] != '\0') ? s_cli_apps_captive_clone_ssid : "-",
           (s_cli_apps_captive_portal_file[0] != '\0') ? s_cli_apps_captive_portal_file : "default");
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_sniffer_start(int argc, char** argv)
{
    poom_sniffer_device_config_t cfg = {0};
    uint32_t time_s = 0U;
    bool got_filter = false;
    bool got_channel = false;
    bool got_time = false;
    esp_err_t err;

    if((argc != 7) || ((argc % 2) == 0))
    {
        printf("Usage: sniffer-start --filter <probe|beacon|mgmt|data|ctrl|all> --channel <hop|channel> --time <sec>\n");
        return 1;
    }

    err = poom_sniffer_device_config_default(&cfg);
    if(err != ESP_OK)
    {
        printf("sniffer-start init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    cfg.save_to_sd = false;
    cfg.dedupe_enabled = false;
    cfg.attempt_time_sync = false;
    cfg.print_packet_summary = true;

    for(int i = 1; i < argc; i += 2)
    {
        const char* key = argv[i];
        const char* value = argv[i + 1];

        if((strcmp(key, "--filter") == 0) || (strcmp(key, "-f") == 0))
        {
            if(!cli_apps_parse_sniffer_filter_(value, &cfg.filter))
            {
                printf("Invalid sniffer filter\n");
                return 1;
            }
            got_filter = true;
        }
        else if((strcmp(key, "--channel") == 0) || (strcmp(key, "-c") == 0))
        {
            uint32_t channel = 0U;

            if(strcasecmp(value, "hop") == 0)
            {
                cfg.channel = 0U;
            }
            else if(cli_apps_parse_u32_(value, 165U, &channel) && (channel > 0U))
            {
                cfg.channel = (uint8_t)channel;
            }
            else
            {
                printf("Invalid sniffer channel\n");
                return 1;
            }
            got_channel = true;
        }
        else if((strcmp(key, "--time") == 0) || (strcmp(key, "-t") == 0))
        {
            if(!cli_apps_parse_u32_(value, 600U, &time_s) || (time_s == 0U))
            {
                printf("Invalid sniffer time\n");
                return 1;
            }
            got_time = true;
        }
        else
        {
            printf("Unknown sniffer-start option: %s\n", key);
            return 1;
        }
    }

    if(!got_filter || !got_channel || !got_time)
    {
        printf("Usage: sniffer-start --filter <probe|beacon|mgmt|data|ctrl|all> --channel <hop|channel> --time <sec>\n");
        return 1;
    }

    cli_apps_prepare_wifi_attack_();
    if(cfg.channel == 0U)
    {
        printf("sniffer start: filter=%s channel=hop time=%lus\n",
               cli_apps_sniffer_filter_name_(cfg.filter),
               (unsigned long)time_s);
    }
    else
    {
        printf("sniffer start: filter=%s channel=%u time=%lus\n",
               cli_apps_sniffer_filter_name_(cfg.filter),
               (unsigned)cfg.channel,
               (unsigned long)time_s);
    }

    err = poom_sniffer_device_start_ex(&cfg);
    if(err != ESP_OK)
    {
        printf("sniffer-start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    for(uint32_t waited_ms = 0U; waited_ms < (time_s * 1000U); waited_ms += 100U)
    {
        bool running = false;
        vTaskDelay(pdMS_TO_TICKS(100U));
        if((poom_sniffer_device_get_running(&running) != ESP_OK) || !running)
        {
            break;
        }
    }

    (void)poom_sniffer_device_stop();
    printf("sniffer done: filter=%s channel=%s time=%lus\n",
           cli_apps_sniffer_filter_name_(cfg.filter),
           (cfg.channel == 0U) ? "hop" : "fixed",
           (unsigned long)time_s);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_start(int argc, char** argv)
{
    const char* mode = NULL;
    uint32_t hop_ms = 200U;
    esp_err_t err;

    if((argc != 2) && (argc != 3))
    {
        printf("Usage: scanner-start <wifi|154> [hop_ms]\n");
        return 1;
    }

    mode = argv[1];
    if((argc == 3) && (!cli_apps_parse_u32_(argv[2], 60000U, &hop_ms)))
    {
        printf("Invalid scanner hop_ms\n");
        return 1;
    }

    if(poom_scanner_core_get_mode() != POOM_SCANNER_CORE_MODE_NONE)
    {
        printf("Stopping running scanner before restart\n");
        cli_apps_stop_scanner_();
    }

    cli_apps_prepare_wifi_attack_();

    if((strcasecmp(mode, "wifi") == 0) || (strcasecmp(mode, "wifi2g") == 0))
    {
        err = poom_scanner_core_start_wifi(hop_ms);
        if(err != ESP_OK)
        {
            printf("scanner-start wifi failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        s_cli_apps_scanner_last_mode = POOM_SCANNER_CORE_MODE_WIFI;
    }
    else if((strcasecmp(mode, "154") == 0) || (strcasecmp(mode, "ieee802154") == 0) ||
            (strcasecmp(mode, "zigbee") == 0))
    {
        err = poom_scanner_core_start_ieee802154(hop_ms);
        if(err != ESP_OK)
        {
            printf("scanner-start 154 failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        s_cli_apps_scanner_last_mode = POOM_SCANNER_CORE_MODE_IEEE802154;
    }
    else
    {
        printf("Invalid scanner mode. Use wifi or 154\n");
        return 1;
    }

    printf("scanner running: mode=%s hop_ms=%lu\n",
           cli_apps_scanner_mode_name_(poom_scanner_core_get_mode()),
           (unsigned long)hop_ms);
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    cli_apps_stop_scanner_();
    printf("scanner stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_scanner_reset`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_reset(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_scanner_core_reset_stats();
    printf("scanner stats reset\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_scanner_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_status(int argc, char** argv)
{
    const poom_scanner_core_mode_t current_mode = poom_scanner_core_get_mode();
    const poom_scanner_core_mode_t report_mode = cli_apps_scanner_report_mode_();

    (void)argc;
    (void)argv;

    if(report_mode == POOM_SCANNER_CORE_MODE_NONE)
    {
        printf("scanner: STOPPED | mode=NONE\n");
        return 0;
    }

    if(report_mode == POOM_SCANNER_CORE_MODE_WIFI)
    {
        poom_scanner_core_wifi_stats_t st = {0};
        uint32_t total = 0U;

        if(!poom_scanner_core_get_wifi_stats(&st))
        {
            printf("scanner-status: cannot read Wi-Fi stats\n");
            return 1;
        }

        for(size_t i = 0U; i < st.channel_count && i < POOM_SCANNER_CORE_WIFI_CH_COUNT; i++)
        {
            total += st.packet_count[i];
        }

        printf("scanner: %s | mode=WIFI | current_ch=%u | hop_ms=%lu | total=%lu | channels=%u\n",
               (current_mode == POOM_SCANNER_CORE_MODE_NONE) ? "STOPPED" : "RUNNING",
               (unsigned)st.current_channel,
               (unsigned long)st.hop_ms,
               (unsigned long)total,
               (unsigned)st.channel_count);
        return 0;
    }

    if(report_mode == POOM_SCANNER_CORE_MODE_IEEE802154)
    {
        poom_scanner_core_ieee802154_stats_t st = {0};
        uint32_t total = 0U;

        if(!poom_scanner_core_get_ieee802154_stats(&st))
        {
            printf("scanner-status: cannot read IEEE 802.15.4 stats\n");
            return 1;
        }

        for(size_t i = 0U; i < POOM_SCANNER_CORE_IEEE802154_CH_COUNT; i++)
        {
            total += st.packet_count[i];
        }

        printf("scanner: %s | mode=IEEE802154 | current_ch=%u | hop_ms=%lu | total=%lu | channels=%u\n",
               (current_mode == POOM_SCANNER_CORE_MODE_NONE) ? "STOPPED" : "RUNNING",
               (unsigned)st.current_channel,
               (unsigned long)st.hop_ms,
               (unsigned long)total,
               (unsigned)POOM_SCANNER_CORE_IEEE802154_CH_COUNT);
        return 0;
    }

    printf("scanner: STOPPED | mode=NONE\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_scanner_top`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_top(int argc, char** argv)
{
    poom_scanner_core_top_channel_t top[POOM_SCANNER_CORE_WIFI_CH_COUNT] = {{0}};
    size_t written = 0U;
    uint32_t requested = 5U;
    const poom_scanner_core_mode_t report_mode = cli_apps_scanner_report_mode_();

    if(argc > 2)
    {
        printf("Usage: scanner-top [count]\n");
        return 1;
    }
    if((argc == 2) && (!cli_apps_parse_u32_(argv[1], POOM_SCANNER_CORE_WIFI_CH_COUNT, &requested)))
    {
        printf("Invalid scanner-top count\n");
        return 1;
    }

    if(report_mode == POOM_SCANNER_CORE_MODE_NONE)
    {
        printf("scanner-top: no scanner mode selected yet\n");
        return 1;
    }

    if(report_mode == POOM_SCANNER_CORE_MODE_WIFI)
    {
        written = poom_scanner_core_get_wifi_top_channels(top, requested);
        printf("scanner-top wifi:\n");
    }
    else
    {
        if(requested > POOM_SCANNER_CORE_IEEE802154_CH_COUNT)
        {
            requested = POOM_SCANNER_CORE_IEEE802154_CH_COUNT;
        }
        written = poom_scanner_core_get_ieee802154_top_channels(top, requested);
        printf("scanner-top ieee802154:\n");
    }

    if(written == 0U)
    {
        printf("no channel activity yet\n");
        return 0;
    }

    for(size_t i = 0U; i < written; i++)
    {
        printf("%u) ch=%u count=%lu pct=%u%% rssi_avg=%ddBm rssi_max=%ddBm\n",
               (unsigned)i,
               (unsigned)top[i].channel,
               (unsigned long)top[i].packet_count,
               (unsigned)top[i].packet_pct,
               (int)top[i].rssi_avg_dbm,
               (int)top[i].rssi_max_dbm);
    }

    return 0;
}

/**
 * @brief Internal helper for `cmd_scanner_channels`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_scanner_channels(int argc, char** argv)
{
    const poom_scanner_core_mode_t report_mode = cli_apps_scanner_report_mode_();

    (void)argc;
    (void)argv;

    if(report_mode == POOM_SCANNER_CORE_MODE_NONE)
    {
        printf("scanner-channels: no scanner mode selected yet\n");
        return 1;
    }

    if(report_mode == POOM_SCANNER_CORE_MODE_WIFI)
    {
        return cli_apps_scanner_print_wifi_channels_();
    }

    return cli_apps_scanner_print_ieee_channels_();
}

/**
 * @brief Internal helper for `cmd_ir_nec_send`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_nec_send(int argc, char** argv)
{
    uint32_t address = 0U;
    uint32_t command = 0U;
    cli_apps_ir_code_t code = {
        .protocol = CLI_APPS_IR_PROTO_NEC,
    };

    if(argc != 3)
    {
        printf("Usage: ir-nec-send <addr_8bit> <cmd_8bit>\n");
        return 1;
    }

    if((!cli_apps_parse_u32_(argv[1], 0xFFU, &address)) || (!cli_apps_parse_u32_(argv[2], 0xFFU, &command)))
    {
        printf("Invalid NEC address/command\n");
        return 1;
    }

    code.address = (uint16_t)address;
    code.command = (uint8_t)command;
    return cli_apps_ir_send_code_(&code);
}

/**
 * @brief Internal helper for `cmd_ir_nec_ext_send`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_nec_ext_send(int argc, char** argv)
{
    uint32_t address = 0U;
    uint32_t command = 0U;
    cli_apps_ir_code_t code = {
        .protocol = CLI_APPS_IR_PROTO_NEC_EXT,
    };

    if(argc != 3)
    {
        printf("Usage: ir-nec-ext-send <addr_16bit> <cmd_8bit>\n");
        return 1;
    }

    if((!cli_apps_parse_u32_(argv[1], 0xFFFFU, &address)) || (!cli_apps_parse_u32_(argv[2], 0xFFU, &command)))
    {
        printf("Invalid NEC_EXT address/command\n");
        return 1;
    }

    code.address = (uint16_t)address;
    code.command = (uint8_t)command;
    return cli_apps_ir_send_code_(&code);
}

/**
 * @brief Internal helper for `cmd_ir_samsung32_send`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_samsung32_send(int argc, char** argv)
{
    uint32_t address = 0U;
    uint32_t command = 0U;
    cli_apps_ir_code_t code = {
        .protocol = CLI_APPS_IR_PROTO_SAMSUNG32,
    };

    if(argc != 3)
    {
        printf("Usage: ir-samsung32-send <addr_16bit> <cmd_8bit>\n");
        return 1;
    }

    if((!cli_apps_parse_u32_(argv[1], 0xFFFFU, &address)) || (!cli_apps_parse_u32_(argv[2], 0xFFU, &command)))
    {
        printf("Invalid Samsung32 address/command\n");
        return 1;
    }

    code.address = (uint16_t)address;
    code.command = (uint8_t)command;
    return cli_apps_ir_send_code_(&code);
}

/**
 * @brief Internal helper for `cmd_ir_rx_once`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_rx_once(int argc, char** argv)
{
#if !defined(PIN_NUM_IR_RX)
    (void)argc;
    (void)argv;
    printf("IR RX not supported on this board\n");
    return 1;
#else
    uint32_t timeout_ms = CLI_APPS_IR_TIMEOUT_MS;
    ir_rcv_handle_t receiver = {0};
    ir_decoder_context_t decoder_ctx = {0};
    ir_rcv_config_t cfg = ir_rcv_default_config();
    rmt_rx_done_event_data_t rx = {0};
    ir_decoded_frame_t frame = {0};
    bool got_frame = false;

    if(argc > 2)
    {
        printf("Usage: ir-rx-once [timeout_ms]\n");
        return 1;
    }
    if((argc == 2) && (!cli_apps_parse_u32_(argv[1], 60000U, &timeout_ms)))
    {
        printf("Invalid timeout\n");
        return 1;
    }

    cfg.gpio = PIN_NUM_IR_RX;
    cfg.clk_hz = 1000000U;
    cfg.buffer_symbols = CLI_APPS_IR_RX_BUFFER_SYMBOLS;
    ir_decoder_context_reset(&decoder_ctx);

    if(ir_rcv_init(&receiver, &cfg, "cli_ir_rx") != ESP_OK)
    {
        printf("ir rx init failed\n");
        return 1;
    }
    if(ir_rcv_start(&receiver, &cfg) != ESP_OK)
    {
        (void)ir_rcv_deinit(&receiver);
        printf("ir rx start failed\n");
        return 1;
    }
    if(!ir_rcv_wait(&receiver, &rx, timeout_ms))
    {
        (void)ir_rcv_deinit(&receiver);
        printf("ir rx timeout\n");
        return 1;
    }

    if(ir_decode_any_ex(rx.received_symbols, rx.num_symbols, cfg.clk_hz, &decoder_ctx, &frame))
    {
        s_cli_apps_ir_last_code.protocol = (uint8_t)frame.protocol;
        s_cli_apps_ir_last_code.flags = frame.repeat ? 1U : 0U;
        s_cli_apps_ir_last_code.reserved = 0U;
        s_cli_apps_ir_last_code.address = frame.address;
        s_cli_apps_ir_last_code.command = frame.command;
        got_frame = true;
    }

    (void)ir_rcv_deinit(&receiver);

    if(!got_frame)
    {
        printf("ir rx: protocol not recognized\n");
        return 1;
    }

    s_cli_apps_ir_last_valid = true;

    printf("ir rx: proto=%s addr=0x%08" PRIX32 " cmd=0x%08" PRIX32 " repeat=%u symbols=%u\n",
           cli_apps_ir_proto_name_(s_cli_apps_ir_last_code.protocol),
           s_cli_apps_ir_last_code.address,
           s_cli_apps_ir_last_code.command,
           (unsigned)s_cli_apps_ir_last_code.flags,
           (unsigned)rx.num_symbols);
    return 0;
#endif
}

/**
 * @brief Internal helper for `cmd_ir_last`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_last(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if(!s_cli_apps_ir_last_valid)
    {
        printf("ir last: empty\n");
        return 0;
    }

    printf("ir last: proto=%s addr=0x%08" PRIX32 " cmd=0x%08" PRIX32 " repeat=%u\n",
           cli_apps_ir_proto_name_(s_cli_apps_ir_last_code.protocol),
           s_cli_apps_ir_last_code.address,
           s_cli_apps_ir_last_code.command,
           (unsigned)s_cli_apps_ir_last_code.flags);
    return 0;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_save_last(int argc, char** argv)
{
    const char* key = NULL;

    if(argc != 2)
    {
        printf("Usage: ir-save-last <a|b|up|down|left|right>\n");
        return 1;
    }
    if(!s_cli_apps_ir_last_valid)
    {
        printf("ir-save-last: no captured code\n");
        return 1;
    }

    key = cli_apps_ir_slot_key_(argv[1]);
    if(key == NULL)
    {
        printf("Invalid IR slot\n");
        return 1;
    }

    if(!cli_apps_ir_save_code_(key, &s_cli_apps_ir_last_code))
    {
        printf("ir-save-last failed\n");
        return 1;
    }

    printf("ir saved slot=%s proto=%s addr=0x%08" PRIX32 " cmd=0x%08" PRIX32 "\n",
           argv[1],
           cli_apps_ir_proto_name_(s_cli_apps_ir_last_code.protocol),
           s_cli_apps_ir_last_code.address,
           s_cli_apps_ir_last_code.command);
    return 0;
}

/**
 * @brief Internal helper for `cmd_ir_slot_show`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_slot_show(int argc, char** argv)
{
    const char* key = NULL;
    cli_apps_ir_code_t code = {0};

    if(argc != 2)
    {
        printf("Usage: ir-slot-show <a|b|up|down|left|right>\n");
        return 1;
    }

    key = cli_apps_ir_slot_key_(argv[1]);
    if((key == NULL) || (!cli_apps_ir_load_code_(key, &code)))
    {
        printf("ir slot empty/invalid\n");
        return 1;
    }

    printf("ir slot %s: proto=%s addr=0x%08" PRIX32 " cmd=0x%08" PRIX32 "\n",
           argv[1],
           cli_apps_ir_proto_name_(code.protocol),
           code.address,
           code.command);
    return 0;
}

/**
 * @brief Internal helper for `cmd_ir_slot_send`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_ir_slot_send(int argc, char** argv)
{
    const char* key = NULL;
    cli_apps_ir_code_t code = {0};

    if(argc != 2)
    {
        printf("Usage: ir-slot-send <a|b|up|down|left|right>\n");
        return 1;
    }

    key = cli_apps_ir_slot_key_(argv[1]);
    if((key == NULL) || (!cli_apps_ir_load_code_(key, &code)))
    {
        printf("ir slot empty/invalid\n");
        return 1;
    }

    return cli_apps_ir_send_code_(&code);
}

void cli_poom_apps_register_cmds(void)
{
    const esp_console_cmd_t cli_stop_all_cmd = {
        .command = "cli-stop-all",
        .help = "Stop CLI-launched apps and release radios/tasks where possible",
        .hint = NULL,
        .func = &cmd_cli_stop_all,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cli_stop_all_cmd));

    const esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi-scan",
        .help = "Run Wi-Fi scan and print AP list",
        .hint = NULL,
        .func = &cmd_wifi_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_scan_cmd));

    const esp_console_cmd_t wifi_scan_show_cmd = {
        .command = "wifi-scan-show",
        .help = "Show cached Wi-Fi scan results",
        .hint = NULL,
        .func = &cmd_wifi_scan_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_scan_show_cmd));

    const esp_console_cmd_t wifi_connect_cmd = {
        .command = "wifi-connect",
        .help = "Connect to AP by scan index. Usage: wifi-connect <index> [password]",
        .hint = NULL,
        .func = &cmd_wifi_connect,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_connect_cmd));

    const esp_console_cmd_t wifi_connect_ssid_cmd = {
        .command = "wifi-connect-ssid",
        .help = "Connect to AP by SSID. Usage: wifi-connect-ssid <ssid> [password]",
        .hint = NULL,
        .func = &cmd_wifi_connect_ssid,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_connect_ssid_cmd));

    const esp_console_cmd_t wifi_disconnect_cmd = {
        .command = "wifi-disconnect",
        .help = "Disconnect Wi-Fi STA",
        .hint = NULL,
        .func = &cmd_wifi_disconnect,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_disconnect_cmd));

    const esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi-status",
        .help = "Show Wi-Fi STA status",
        .hint = NULL,
        .func = &cmd_wifi_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_status_cmd));

    const esp_console_cmd_t wifi_save_current_cmd = {
        .command = "wifi-save-current",
        .help = "Save current Wi-Fi SSID/password from last CLI connect",
        .hint = NULL,
        .func = &cmd_wifi_save_current,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_save_current_cmd));

    const esp_console_cmd_t wifi_spam_start_cmd = {
        .command = "wifi-spam-start",
        .help = "Start Wi-Fi spam runtime",
        .hint = NULL,
        .func = &cmd_wifi_spam_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_start_cmd));

    const esp_console_cmd_t wifi_spam_stop_cmd = {
        .command = "wifi-spam-stop",
        .help = "Stop Wi-Fi spam runtime",
        .hint = NULL,
        .func = &cmd_wifi_spam_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_stop_cmd));

    const esp_console_cmd_t wifi_spam_status_cmd = {
        .command = "wifi-spam-status",
        .help = "Show Wi-Fi spam runtime status",
        .hint = NULL,
        .func = &cmd_wifi_spam_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_status_cmd));

    const esp_console_cmd_t ble_spam_start_cmd = {
        .command = "ble-spam-start",
        .help = "Start BLE spam runtime",
        .hint = NULL,
        .func = &cmd_ble_spam_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_spam_start_cmd));

    const esp_console_cmd_t ble_spam_stop_cmd = {
        .command = "ble-spam-stop",
        .help = "Stop BLE spam runtime",
        .hint = NULL,
        .func = &cmd_ble_spam_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_spam_stop_cmd));

    const esp_console_cmd_t ble_spam_status_cmd = {
        .command = "ble-spam-status",
        .help = "Show BLE spam runtime status",
        .hint = NULL,
        .func = &cmd_ble_spam_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_spam_status_cmd));

    const esp_console_cmd_t ble_spam_current_cmd = {
        .command = "ble-spam-current",
        .help = "Show current BLE spam label",
        .hint = NULL,
        .func = &cmd_ble_spam_current,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_spam_current_cmd));

    const esp_console_cmd_t ble_tracker_start_cmd = {
        .command = "ble-tracker-start",
        .help = "Start BLE tracker scan",
        .hint = NULL,
        .func = &cmd_ble_tracker_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_start_cmd));

    const esp_console_cmd_t ble_tracker_stop_cmd = {
        .command = "ble-tracker-stop",
        .help = "Stop BLE tracker scan and free cached results",
        .hint = NULL,
        .func = &cmd_ble_tracker_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_stop_cmd));

    const esp_console_cmd_t ble_tracker_status_cmd = {
        .command = "ble-tracker-status",
        .help = "Show BLE tracker status and cached count",
        .hint = NULL,
        .func = &cmd_ble_tracker_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_status_cmd));

    const esp_console_cmd_t ble_tracker_list_cmd = {
        .command = "ble-tracker-list",
        .help = "List cached BLE tracker records",
        .hint = NULL,
        .func = &cmd_ble_tracker_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_list_cmd));

    const esp_console_cmd_t ble_tracker_clear_cmd = {
        .command = "ble-tracker-clear",
        .help = "Clear cached BLE tracker records",
        .hint = NULL,
        .func = &cmd_ble_tracker_clear,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_clear_cmd));

    const esp_console_cmd_t ble_tracker_scan_cmd = {
        .command = "ble-tracker-scan",
        .help = "Start a fresh BLE tracker scan",
        .hint = NULL,
        .func = &cmd_ble_tracker_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_tracker_scan_cmd));

    const esp_console_cmd_t ble_gatt_reset_cmd = {
        .command = "ble-gatt-reset",
        .help = "Reset BLE GATT lab config and stop runtime",
        .hint = NULL,
        .func = &cmd_ble_gatt_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_reset_cmd));

    const esp_console_cmd_t ble_gatt_name_set_cmd = {
        .command = "ble-gatt-name-set",
        .help = "Set BLE GATT device name. Usage: ble-gatt-name-set <name>",
        .hint = NULL,
        .func = &cmd_ble_gatt_name_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_name_set_cmd));

    const esp_console_cmd_t ble_gatt_service_set_cmd = {
        .command = "ble-gatt-service-set",
        .help = "Set BLE GATT service UUID. Usage: ble-gatt-service-set <uuid16|uuid128>",
        .hint = NULL,
        .func = &cmd_ble_gatt_service_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_service_set_cmd));

    const esp_console_cmd_t ble_gatt_char_set_cmd = {
        .command = "ble-gatt-char-set",
        .help = "Set BLE GATT characteristic UUID and props. Usage: ble-gatt-char-set [index] <uuid16|uuid128> <read|write|readwrite>",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_set_cmd));

    const esp_console_cmd_t ble_gatt_char_add_cmd = {
        .command = "ble-gatt-char-add",
        .help = "Add BLE GATT characteristic. Usage: ble-gatt-char-add <uuid16|uuid128> <read|write|readwrite>",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_add,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_add_cmd));

    const esp_console_cmd_t ble_gatt_char_del_cmd = {
        .command = "ble-gatt-char-del",
        .help = "Delete BLE GATT characteristic by index. Usage: ble-gatt-char-del <index>",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_del,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_del_cmd));

    const esp_console_cmd_t ble_gatt_char_list_cmd = {
        .command = "ble-gatt-char-list",
        .help = "List BLE GATT characteristics",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_list_cmd));

    const esp_console_cmd_t ble_gatt_char_value_cmd = {
        .command = "ble-gatt-char-value",
        .help = "Set BLE GATT characteristic 0 value bytes. Usage: ble-gatt-char-value <hex...>",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_value,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_value_cmd));

    const esp_console_cmd_t ble_gatt_char_value_set_cmd = {
        .command = "ble-gatt-char-value-set",
        .help = "Set BLE GATT characteristic value bytes. Usage: ble-gatt-char-value-set <index> <hex...>",
        .hint = NULL,
        .func = &cmd_ble_gatt_char_value_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_char_value_set_cmd));

    const esp_console_cmd_t ble_gatt_start_cmd = {
        .command = "ble-gatt-start",
        .help = "Start BLE GATT lab runtime",
        .hint = NULL,
        .func = &cmd_ble_gatt_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_start_cmd));

    const esp_console_cmd_t ble_gatt_status_cmd = {
        .command = "ble-gatt-status",
        .help = "Show BLE GATT lab status and current config",
        .hint = NULL,
        .func = &cmd_ble_gatt_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_status_cmd));

    const esp_console_cmd_t ble_gatt_stop_cmd = {
        .command = "ble-gatt-stop",
        .help = "Stop BLE GATT lab runtime",
        .hint = NULL,
        .func = &cmd_ble_gatt_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_gatt_stop_cmd));

    const esp_console_cmd_t karma_start_cmd = {
        .command = "karma-start",
        .help = "Start Wi-Fi karma runtime",
        .hint = NULL,
        .func = &cmd_karma_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_start_cmd));

    const esp_console_cmd_t karma_stop_cmd = {
        .command = "karma-stop",
        .help = "Stop Wi-Fi karma runtime",
        .hint = NULL,
        .func = &cmd_karma_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_stop_cmd));

    const esp_console_cmd_t karma_status_cmd = {
        .command = "karma-status",
        .help = "Show Wi-Fi karma status",
        .hint = NULL,
        .func = &cmd_karma_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_status_cmd));

    const esp_console_cmd_t karma_ssids_list_cmd = {
        .command = "karma-ssids-list",
        .help = "List discovered Karma SSIDs",
        .hint = NULL,
        .func = &cmd_karma_ssids_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_ssids_list_cmd));

    const esp_console_cmd_t karma_active_ssid_cmd = {
        .command = "karma-active-ssid",
        .help = "Show active Karma spoofed SSID",
        .hint = NULL,
        .func = &cmd_karma_active_ssid,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_active_ssid_cmd));

    const esp_console_cmd_t karma_clear_cmd = {
        .command = "karma-clear",
        .help = "Clear Karma runtime by stopping it",
        .hint = NULL,
        .func = &cmd_karma_clear,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&karma_clear_cmd));

    const esp_console_cmd_t deauth_scan_cmd = {
        .command = "deauth-scan",
        .help = "Scan nearby APs for deauth target selection",
        .hint = NULL,
        .func = &cmd_deauth_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_scan_cmd));

    const esp_console_cmd_t deauth_attack_cmd = {
        .command = "deauth-attack",
        .help = "Launch one deauth attack against cached AP index",
        .hint = NULL,
        .func = &cmd_deauth_attack,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_attack_cmd));

    const esp_console_cmd_t deauth_start_cmd = {
        .command = "deauth-start",
        .help = "Start continuous deauth scan/attack loop",
        .hint = NULL,
        .func = &cmd_deauth_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_start_cmd));

    const esp_console_cmd_t deauth_stop_cmd = {
        .command = "deauth-stop",
        .help = "Stop deauth runtime",
        .hint = NULL,
        .func = &cmd_deauth_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_stop_cmd));

    const esp_console_cmd_t deauth_status_cmd = {
        .command = "deauth-status",
        .help = "Show deauth runtime status",
        .hint = NULL,
        .func = &cmd_deauth_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_status_cmd));

    const esp_console_cmd_t deauth_det_start_cmd = {
        .command = "deauth-det-start",
        .help = "Start deauth detector runtime",
        .hint = NULL,
        .func = &cmd_deauth_det_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_start_cmd));

    const esp_console_cmd_t deauth_det_stop_cmd = {
        .command = "deauth-det-stop",
        .help = "Stop deauth detector runtime",
        .hint = NULL,
        .func = &cmd_deauth_det_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_stop_cmd));

    const esp_console_cmd_t deauth_det_stats_cmd = {
        .command = "deauth-det-stats",
        .help = "Show deauth detector counters and top offender",
        .hint = NULL,
        .func = &cmd_deauth_det_stats,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_stats_cmd));

    const esp_console_cmd_t deauth_det_reset_cmd = {
        .command = "deauth-det-reset",
        .help = "Reset deauth detector counters",
        .hint = NULL,
        .func = &cmd_deauth_det_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_reset_cmd));

    const esp_console_cmd_t deauth_det_hop_cmd = {
        .command = "deauth-det-hop",
        .help = "Enable or disable deauth detector channel hopping",
        .hint = NULL,
        .func = &cmd_deauth_det_hop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_hop_cmd));

    const esp_console_cmd_t deauth_det_ch_cmd = {
        .command = "deauth-det-ch",
        .help = "Set fixed channel for deauth detector",
        .hint = NULL,
        .func = &cmd_deauth_det_ch,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deauth_det_ch_cmd));

    const esp_console_cmd_t captive_start_cmd = {
        .command = "captive-start",
        .help = "Start captive portal. Usage: captive-start [portal_file]",
        .hint = NULL,
        .func = &cmd_captive_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&captive_start_cmd));

    const esp_console_cmd_t captive_start_from_scan_cmd = {
        .command = "captive-start-from-scan",
        .help = "Start captive using cloned SSID from cached scan index",
        .hint = NULL,
        .func = &cmd_captive_start_from_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&captive_start_from_scan_cmd));

    const esp_console_cmd_t captive_stop_cmd = {
        .command = "captive-stop",
        .help = "Stop captive portal runtime",
        .hint = NULL,
        .func = &cmd_captive_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&captive_stop_cmd));

    const esp_console_cmd_t captive_status_cmd = {
        .command = "captive-status",
        .help = "Show captive portal status",
        .hint = NULL,
        .func = &cmd_captive_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&captive_status_cmd));

    const esp_console_cmd_t sniffer_start_cmd = {
        .command = "sniffer-start",
        .help = "Run Wi-Fi sniffer in foreground. Usage: sniffer-start --filter <probe|beacon|mgmt|data|ctrl|all> --channel <hop|channel> --time <sec>",
        .hint = NULL,
        .func = &cmd_sniffer_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sniffer_start_cmd));

    const esp_console_cmd_t scanner_start_cmd = {
        .command = "scanner-start",
        .help = "Start channel scanner. Usage: scanner-start <wifi|154> [hop_ms]",
        .hint = NULL,
        .func = &cmd_scanner_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_start_cmd));

    const esp_console_cmd_t scanner_stop_cmd = {
        .command = "scanner-stop",
        .help = "Stop channel scanner",
        .hint = NULL,
        .func = &cmd_scanner_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_stop_cmd));

    const esp_console_cmd_t scanner_status_cmd = {
        .command = "scanner-status",
        .help = "Show scanner mode, channel and packet totals",
        .hint = NULL,
        .func = &cmd_scanner_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_status_cmd));

    const esp_console_cmd_t scanner_top_cmd = {
        .command = "scanner-top",
        .help = "Show top busy channels. Usage: scanner-top [count]",
        .hint = NULL,
        .func = &cmd_scanner_top,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_top_cmd));

    const esp_console_cmd_t scanner_channels_cmd = {
        .command = "scanner-channels",
        .help = "Show all cached scanner channels sorted by activity",
        .hint = NULL,
        .func = &cmd_scanner_channels,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_channels_cmd));

    const esp_console_cmd_t scanner_reset_cmd = {
        .command = "scanner-reset",
        .help = "Reset scanner counters",
        .hint = NULL,
        .func = &cmd_scanner_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scanner_reset_cmd));

    const esp_console_cmd_t lua_run_cmd = {
        .command = "lua-run",
        .help = "Run Lua file from SD. Usage: lua-run [abs_path]",
        .hint = NULL,
        .func = &cmd_lua_run,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_run_cmd));

    const esp_console_cmd_t lua_stop_cmd = {
        .command = "lua-stop",
        .help = "Request stop of running Lua script",
        .hint = NULL,
        .func = &cmd_lua_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_stop_cmd));

    const esp_console_cmd_t lua_status_cmd = {
        .command = "lua-status",
        .help = "Show Lua runtime status",
        .hint = NULL,
        .func = &cmd_lua_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_status_cmd));

    const esp_console_cmd_t lua_upload_begin_cmd = {
        .command = "lua-upload-begin",
        .help = "Begin Lua upload to SD. Usage: lua-upload-begin </sdcard/file.lua>",
        .hint = NULL,
        .func = &cmd_lua_upload_begin,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_upload_begin_cmd));

    const esp_console_cmd_t lua_upload_line_cmd = {
        .command = "lua-upload-line",
        .help = "Append one Lua source line to active upload",
        .hint = NULL,
        .func = &cmd_lua_upload_line,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_upload_line_cmd));

    const esp_console_cmd_t lua_upload_end_cmd = {
        .command = "lua-upload-end",
        .help = "Finish Lua upload and close saved file",
        .hint = NULL,
        .func = &cmd_lua_upload_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_upload_end_cmd));

    const esp_console_cmd_t lua_upload_abort_cmd = {
        .command = "lua-upload-abort",
        .help = "Abort Lua upload and remove partial file",
        .hint = NULL,
        .func = &cmd_lua_upload_abort,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lua_upload_abort_cmd));

    const esp_console_cmd_t ir_nec_send_cmd = {
        .command = "ir-nec-send",
        .help = "Send NEC IR frame. Usage: ir-nec-send <addr_8bit> <cmd_8bit>",
        .hint = NULL,
        .func = &cmd_ir_nec_send,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_nec_send_cmd));

    const esp_console_cmd_t ir_nec_ext_send_cmd = {
        .command = "ir-nec-ext-send",
        .help = "Send NEC_EXT IR frame. Usage: ir-nec-ext-send <addr_16bit> <cmd_8bit>",
        .hint = NULL,
        .func = &cmd_ir_nec_ext_send,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_nec_ext_send_cmd));

    const esp_console_cmd_t ir_samsung32_send_cmd = {
        .command = "ir-samsung32-send",
        .help = "Send Samsung32 IR frame. Usage: ir-samsung32-send <addr_16bit> <cmd_8bit>",
        .hint = NULL,
        .func = &cmd_ir_samsung32_send,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_samsung32_send_cmd));

    const esp_console_cmd_t ir_rx_once_cmd = {
        .command = "ir-rx-once",
        .help = "Capture one IR frame and print detected protocol",
        .hint = NULL,
        .func = &cmd_ir_rx_once,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_rx_once_cmd));

    const esp_console_cmd_t ir_last_cmd = {
        .command = "ir-last",
        .help = "Show last IR frame captured via ir-rx-once",
        .hint = NULL,
        .func = &cmd_ir_last,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_last_cmd));

    const esp_console_cmd_t ir_save_last_cmd = {
        .command = "ir-save-last",
        .help = "Save last captured IR frame into slot a/b/up/down/left/right",
        .hint = NULL,
        .func = &cmd_ir_save_last,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_save_last_cmd));

    const esp_console_cmd_t ir_slot_show_cmd = {
        .command = "ir-slot-show",
        .help = "Show saved IR slot a/b/up/down/left/right",
        .hint = NULL,
        .func = &cmd_ir_slot_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_slot_show_cmd));

    const esp_console_cmd_t ir_slot_send_cmd = {
        .command = "ir-slot-send",
        .help = "Send saved IR slot a/b/up/down/left/right",
        .hint = NULL,
        .func = &cmd_ir_slot_send,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ir_slot_send_cmd));
}
