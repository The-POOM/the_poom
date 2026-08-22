// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_gatt_dynamic.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"

#if 0
static const char *POOM_BLE_GATT_DYNAMIC_LOG_TAG = "poom_ble_gatt_dynamic";

#define POOM_BLE_GATT_DYNAMIC_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_DYNAMIC_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_BLE_GATT_DYNAMIC_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_DYNAMIC_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_BLE_GATT_DYNAMIC_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_DYNAMIC_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define POOM_BLE_GATT_DYNAMIC_PRINTF_E(...) do { } while (0)
#define POOM_BLE_GATT_DYNAMIC_PRINTF_W(...) do { } while (0)
#define POOM_BLE_GATT_DYNAMIC_PRINTF_I(...) do { } while (0)
#endif

#define POOM_BLE_GATT_DYNAMIC_APP_ID                (0x46U)
#define POOM_BLE_GATT_DYNAMIC_SVC_INST_ID           (0U)
#define POOM_BLE_GATT_DYNAMIC_LOCAL_MTU             (247U)
#define POOM_BLE_GATT_DYNAMIC_ADV_CONFIG_FLAG       (1U << 0)
#define POOM_BLE_GATT_DYNAMIC_SCAN_RSP_CONFIG_FLAG  (1U << 1)
#define POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN           (31U)
#define POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN          (31U)
#define POOM_BLE_GATT_DYNAMIC_CONN_MIN_INT          (0x0BU)
#define POOM_BLE_GATT_DYNAMIC_CONN_MAX_INT          (0x10U)
#define POOM_BLE_GATT_DYNAMIC_CONN_LATENCY          (0U)
#define POOM_BLE_GATT_DYNAMIC_CONN_TIMEOUT          (400U)
#define POOM_BLE_GATT_DYNAMIC_HANDLE_BASE_COUNT     (1U)
#define POOM_BLE_GATT_DYNAMIC_HANDLE_PER_CHAR       (2U)

static uint8_t s_default_char_value[] = {0x61U, 0x70U, 0x70U, 0x73U, 0x65U, 0x63U};
static const esp_attr_control_t s_auto_rsp = {
    .auto_rsp = ESP_GATT_AUTO_RSP,
};

typedef struct
{
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t char_perm;
    esp_gatt_char_prop_t char_property;
    esp_attr_value_t char_value;
    uint8_t *char_value_buffer;
    uint16_t char_value_capacity;
    uint16_t char_handle;
} poom_ble_gatt_dynamic_char_ctx_t;

typedef struct
{
    char *device_name;
    uint8_t *adv_raw_data;
    uint8_t *scan_rsp_raw_data;
    uint8_t adv_raw_len;
    uint8_t scan_rsp_raw_len;
    esp_ble_adv_params_t adv_params;
    esp_bt_uuid_t service_uuid;
    poom_ble_gatt_dynamic_char_ctx_t *chars;
    size_t char_count;
    size_t next_char_index;
    bool started;
    bool stopping;
    bool connected;
    uint8_t adv_config_done;
    esp_gatt_if_t gatts_if;
    uint16_t conn_id;
    uint16_t service_handle;
} poom_ble_gatt_dynamic_ctx_t;

static poom_ble_gatt_dynamic_ctx_t *s_ctx = NULL;

static poom_ble_gatt_dynamic_ctx_t *poom_ble_gatt_dynamic_ctx_ensure_(void);
static void poom_ble_gatt_dynamic_ctx_free_(void);
static bool poom_ble_gatt_dynamic_reset_defaults_(poom_ble_gatt_dynamic_ctx_t *ctx);
static void poom_ble_gatt_dynamic_build_adv_payloads_(poom_ble_gatt_dynamic_ctx_t *ctx);
static void poom_ble_gatt_dynamic_free_chars_(poom_ble_gatt_dynamic_ctx_t *ctx);
static bool poom_ble_gatt_dynamic_alloc_chars_(poom_ble_gatt_dynamic_ctx_t *ctx, size_t char_count);
static bool poom_ble_gatt_dynamic_apply_char_(poom_ble_gatt_dynamic_char_ctx_t *dst,
                                              const poom_ble_gatt_dynamic_char_config_t *src);
static void poom_ble_gatt_dynamic_add_next_char_(poom_ble_gatt_dynamic_ctx_t *ctx);
static uint16_t poom_ble_gatt_dynamic_service_handle_count_(size_t char_count);
static void poom_ble_gatt_dynamic_format_uuid_(const esp_bt_uuid_t *uuid, char *out, size_t out_len);
static void poom_ble_gatt_dynamic_print_bytes_(const uint8_t *data, uint16_t len);
static void poom_ble_gatt_dynamic_print_addr_(const esp_bd_addr_t addr);
static poom_ble_gatt_dynamic_char_ctx_t *poom_ble_gatt_dynamic_find_char_by_handle_(poom_ble_gatt_dynamic_ctx_t *ctx,
                                                                                    uint16_t handle,
                                                                                    size_t *out_index);

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_uuid_len`.
 *
 * @param[in] uuid Parameter passed to the function.
 * @return size_t
 */
static size_t poom_ble_gatt_dynamic_uuid_len_(const esp_bt_uuid_t *uuid)
{
    if (uuid == NULL)
    {
        return 0U;
    }

    switch (uuid->len)
    {
        case ESP_UUID_LEN_16:
            return 2U;
        case ESP_UUID_LEN_32:
            return 4U;
        case ESP_UUID_LEN_128:
            return 16U;
        default:
            return 0U;
    }
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] uuid Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_format_uuid_(const esp_bt_uuid_t *uuid, char *out, size_t out_len)
{
    if ((uuid == NULL) || (out == NULL) || (out_len == 0U))
    {
        return;
    }

    if (uuid->len == ESP_UUID_LEN_16)
    {
        (void)snprintf(out, out_len, "%04X", (unsigned)uuid->uuid.uuid16);
        return;
    }

    if (uuid->len == ESP_UUID_LEN_32)
    {
        (void)snprintf(out, out_len, "%08" PRIX32, uuid->uuid.uuid32);
        return;
    }

    if (uuid->len == ESP_UUID_LEN_128)
    {
        const uint8_t *u = uuid->uuid.uuid128;
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
 * @brief Internal helper for `poom_ble_gatt_dynamic_print_bytes`.
 *
 * @param[in] data Parameter passed to the function.
 * @param[in] len Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_print_bytes_(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        printf(" <empty>");
        return;
    }

    for (uint16_t i = 0U; i < len; i++)
    {
        printf(" %02X", (unsigned)data[i]);
    }
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_print_addr`.
 *
 * @param[in] addr Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_print_addr_(const esp_bd_addr_t addr)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned)addr[0], (unsigned)addr[1], (unsigned)addr[2],
           (unsigned)addr[3], (unsigned)addr[4], (unsigned)addr[5]);
}

/**
 * @brief Handles the current module action.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] handle Parameter passed to the function.
 * @param[in] out_index Parameter passed to the function.
 * @return poom_ble_gatt_dynamic_char_ctx_t *
 */
static poom_ble_gatt_dynamic_char_ctx_t *poom_ble_gatt_dynamic_find_char_by_handle_(poom_ble_gatt_dynamic_ctx_t *ctx,
                                                                                    uint16_t handle,
                                                                                    size_t *out_index)
{
    if (out_index != NULL)
    {
        *out_index = 0U;
    }

    if (ctx == NULL)
    {
        return NULL;
    }

    for (size_t i = 0U; i < ctx->char_count; i++)
    {
        if (ctx->chars[i].char_handle == handle)
        {
            if (out_index != NULL)
            {
                *out_index = i;
            }
            return &ctx->chars[i];
        }
    }

    return NULL;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_free_chars`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_free_chars_(poom_ble_gatt_dynamic_ctx_t *ctx)
{
    if ((ctx == NULL) || (ctx->chars == NULL))
    {
        return;
    }

    for (size_t i = 0U; i < ctx->char_count; i++)
    {
        free(ctx->chars[i].char_value_buffer);
        ctx->chars[i].char_value_buffer = NULL;
        ctx->chars[i].char_value.attr_value = NULL;
        ctx->chars[i].char_value.attr_len = 0U;
        ctx->chars[i].char_value.attr_max_len = 0U;
    }

    free(ctx->chars);
    ctx->chars = NULL;
    ctx->char_count = 0U;
    ctx->next_char_index = 0U;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_alloc_chars`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] char_count Parameter passed to the function.
 * @return bool
 */
static bool poom_ble_gatt_dynamic_alloc_chars_(poom_ble_gatt_dynamic_ctx_t *ctx, size_t char_count)
{
    if ((ctx == NULL) || (char_count == 0U))
    {
        return false;
    }

    ctx->chars = (poom_ble_gatt_dynamic_char_ctx_t *)calloc(char_count, sizeof(*ctx->chars));
    if (ctx->chars == NULL)
    {
        return false;
    }

    ctx->char_count = char_count;
    ctx->next_char_index = 0U;
    return true;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_apply_char`.
 *
 * @param[in] dst Parameter passed to the function.
 * @param[in] src Parameter passed to the function.
 * @return bool
 */
static bool poom_ble_gatt_dynamic_apply_char_(poom_ble_gatt_dynamic_char_ctx_t *dst,
                                              const poom_ble_gatt_dynamic_char_config_t *src)
{
    uint16_t value_capacity;
    uint16_t value_len = 0U;

    if ((dst == NULL) || (src == NULL))
    {
        return false;
    }

    dst->char_uuid = src->char_uuid;
    if (poom_ble_gatt_dynamic_uuid_len_(&dst->char_uuid) == 0U)
    {
        dst->char_uuid.len = ESP_UUID_LEN_16;
        dst->char_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_CHAR_UUID_DEFAULT;
    }

    dst->char_perm = (src->char_perm != 0U)
                         ? src->char_perm
                         : (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
    dst->char_property = (src->char_property != 0U)
                             ? src->char_property
                             : (ESP_GATT_CHAR_PROP_BIT_READ |
                                ESP_GATT_CHAR_PROP_BIT_WRITE |
                                ESP_GATT_CHAR_PROP_BIT_WRITE_NR);

    value_capacity = src->char_val.attr_max_len;
    if (value_capacity == 0U)
    {
        value_capacity = POOM_BLE_GATT_DYNAMIC_CHAR_VAL_MAX_LEN_DEFAULT;
    }
    if (src->char_val.attr_len > value_capacity)
    {
        value_capacity = src->char_val.attr_len;
    }

    dst->char_value_buffer = (uint8_t *)calloc(value_capacity, sizeof(uint8_t));
    if (dst->char_value_buffer == NULL)
    {
        return false;
    }

    dst->char_value_capacity = value_capacity;
    dst->char_value.attr_max_len = value_capacity;
    dst->char_value.attr_value = dst->char_value_buffer;
    dst->char_handle = 0U;

    if ((src->char_val.attr_value != NULL) && (src->char_val.attr_len > 0U))
    {
        value_len = src->char_val.attr_len;
        if (value_len > value_capacity)
        {
            value_len = value_capacity;
        }
        memcpy(dst->char_value_buffer, src->char_val.attr_value, value_len);
    }
    else
    {
        value_len = sizeof(s_default_char_value);
        if (value_len > value_capacity)
        {
            value_len = value_capacity;
        }
        memcpy(dst->char_value_buffer, s_default_char_value, value_len);
    }

    dst->char_value.attr_len = value_len;
    return true;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_ctx_ensure`.
 *
 * @return poom_ble_gatt_dynamic_ctx_t *
 */
static poom_ble_gatt_dynamic_ctx_t *poom_ble_gatt_dynamic_ctx_ensure_(void)
{
    if (s_ctx != NULL)
    {
        return s_ctx;
    }

    s_ctx = (poom_ble_gatt_dynamic_ctx_t *)calloc(1U, sizeof(*s_ctx));
    if (s_ctx == NULL)
    {
        return NULL;
    }

    s_ctx->device_name = (char *)calloc(POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN + 1U, sizeof(char));
    s_ctx->adv_raw_data = (uint8_t *)calloc(POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN, sizeof(uint8_t));
    s_ctx->scan_rsp_raw_data = (uint8_t *)calloc(POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN, sizeof(uint8_t));
    if ((s_ctx->device_name == NULL) ||
        (s_ctx->adv_raw_data == NULL) ||
        (s_ctx->scan_rsp_raw_data == NULL))
    {
        poom_ble_gatt_dynamic_ctx_free_();
        return NULL;
    }

    s_ctx->gatts_if = ESP_GATT_IF_NONE;
    if (!poom_ble_gatt_dynamic_reset_defaults_(s_ctx))
    {
        poom_ble_gatt_dynamic_ctx_free_();
        return NULL;
    }

    return s_ctx;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_ctx_free`.
 *
 * @return void
 */
static void poom_ble_gatt_dynamic_ctx_free_(void)
{
    if (s_ctx == NULL)
    {
        return;
    }

    poom_ble_gatt_dynamic_free_chars_(s_ctx);
    free(s_ctx->scan_rsp_raw_data);
    free(s_ctx->adv_raw_data);
    free(s_ctx->device_name);
    free(s_ctx);
    s_ctx = NULL;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_uuid_ad_type`.
 *
 * @param[in] uuid Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t poom_ble_gatt_dynamic_uuid_ad_type_(const esp_bt_uuid_t *uuid)
{
    if (uuid == NULL)
    {
        return 0U;
    }

    switch (uuid->len)
    {
        case ESP_UUID_LEN_16:
            return ESP_BLE_AD_TYPE_16SRV_CMPL;
        case ESP_UUID_LEN_32:
            return ESP_BLE_AD_TYPE_32SRV_CMPL;
        case ESP_UUID_LEN_128:
            return ESP_BLE_AD_TYPE_128SRV_CMPL;
        default:
            return 0U;
    }
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_uuid_to_bytes`.
 *
 * @param[in] uuid Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return size_t
 */
static size_t poom_ble_gatt_dynamic_uuid_to_bytes_(const esp_bt_uuid_t *uuid, uint8_t *out)
{
    if ((uuid == NULL) || (out == NULL))
    {
        return 0U;
    }

    switch (uuid->len)
    {
        case ESP_UUID_LEN_16:
            out[0] = (uint8_t)(uuid->uuid.uuid16 & 0xFFU);
            out[1] = (uint8_t)((uuid->uuid.uuid16 >> 8) & 0xFFU);
            return 2U;

        case ESP_UUID_LEN_32:
            out[0] = (uint8_t)(uuid->uuid.uuid32 & 0xFFU);
            out[1] = (uint8_t)((uuid->uuid.uuid32 >> 8) & 0xFFU);
            out[2] = (uint8_t)((uuid->uuid.uuid32 >> 16) & 0xFFU);
            out[3] = (uint8_t)((uuid->uuid.uuid32 >> 24) & 0xFFU);
            return 4U;

        case ESP_UUID_LEN_128:
            memcpy(out, uuid->uuid.uuid128, 16U);
            return 16U;

        default:
            return 0U;
    }
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_append_ad_field`.
 *
 * @param[in] buffer Parameter passed to the function.
 * @param[in] buffer_len Parameter passed to the function.
 * @param[in] ad_type Parameter passed to the function.
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @return bool
 */
static bool poom_ble_gatt_dynamic_append_ad_field_(uint8_t *buffer,
                                                   uint8_t *buffer_len,
                                                   uint8_t ad_type,
                                                   const uint8_t *payload,
                                                   uint8_t payload_len)
{
    if ((buffer == NULL) || (buffer_len == NULL))
    {
        return false;
    }

    if (((uint16_t)(*buffer_len) + (uint16_t)payload_len + 2U) > POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN)
    {
        return false;
    }

    buffer[(*buffer_len)++] = (uint8_t)(payload_len + 1U);
    buffer[(*buffer_len)++] = ad_type;
    if ((payload_len > 0U) && (payload != NULL))
    {
        memcpy(&buffer[*buffer_len], payload, payload_len);
        *buffer_len = (uint8_t)(*buffer_len + payload_len);
    }

    return true;
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_build_adv_payloads`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_build_adv_payloads_(poom_ble_gatt_dynamic_ctx_t *ctx)
{
    uint8_t flags = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    uint8_t uuid_bytes[16];
    uint8_t uuid_len;
    uint8_t uuid_type;
    size_t name_len;

    if (ctx == NULL)
    {
        return;
    }

    memset(ctx->adv_raw_data, 0, POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN);
    memset(ctx->scan_rsp_raw_data, 0, POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN);
    ctx->adv_raw_len = 0U;
    ctx->scan_rsp_raw_len = 0U;

    (void)poom_ble_gatt_dynamic_append_ad_field_(ctx->adv_raw_data,
                                                 &ctx->adv_raw_len,
                                                 ESP_BLE_AD_TYPE_FLAG,
                                                 &flags,
                                                 1U);

    uuid_len = (uint8_t)poom_ble_gatt_dynamic_uuid_to_bytes_(&ctx->service_uuid, uuid_bytes);
    uuid_type = poom_ble_gatt_dynamic_uuid_ad_type_(&ctx->service_uuid);
    if ((uuid_len > 0U) && (uuid_type != 0U))
    {
        (void)poom_ble_gatt_dynamic_append_ad_field_(ctx->adv_raw_data,
                                                     &ctx->adv_raw_len,
                                                     uuid_type,
                                                     uuid_bytes,
                                                     uuid_len);
    }

    name_len = strnlen(ctx->device_name, POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN);
    if (name_len == 0U)
    {
        return;
    }

    if (name_len <= (size_t)(POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN - ctx->adv_raw_len - 2U))
    {
        (void)poom_ble_gatt_dynamic_append_ad_field_(ctx->adv_raw_data,
                                                     &ctx->adv_raw_len,
                                                     ESP_BLE_AD_TYPE_NAME_CMPL,
                                                     (const uint8_t *)ctx->device_name,
                                                     (uint8_t)name_len);
        return;
    }

    if (name_len <= (size_t)(POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN - 2U))
    {
        (void)poom_ble_gatt_dynamic_append_ad_field_(ctx->scan_rsp_raw_data,
                                                     &ctx->scan_rsp_raw_len,
                                                     ESP_BLE_AD_TYPE_NAME_CMPL,
                                                     (const uint8_t *)ctx->device_name,
                                                     (uint8_t)name_len);
        return;
    }

    (void)poom_ble_gatt_dynamic_append_ad_field_(ctx->scan_rsp_raw_data,
                                                 &ctx->scan_rsp_raw_len,
                                                 ESP_BLE_AD_TYPE_NAME_SHORT,
                                                 (const uint8_t *)ctx->device_name,
                                                 (uint8_t)(POOM_BLE_GATT_DYNAMIC_ADV_MAX_LEN - 2U));
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_reset_defaults`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @return bool
 */
static bool poom_ble_gatt_dynamic_reset_defaults_(poom_ble_gatt_dynamic_ctx_t *ctx)
{
    poom_ble_gatt_dynamic_char_config_t default_char = {0};

    if (ctx == NULL)
    {
        return false;
    }

    poom_ble_gatt_dynamic_free_chars_(ctx);

    memset(ctx->device_name, 0, POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN + 1U);
    (void)snprintf(ctx->device_name,
                   POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN + 1U,
                   "%s",
                   POOM_BLE_GATT_DYNAMIC_DEVICE_NAME_DEFAULT);
    ctx->service_uuid.len = ESP_UUID_LEN_16;
    ctx->service_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_SERVICE_UUID_DEFAULT;
    ctx->adv_params = poom_ble_gatt_dynamic_default_adv_params();

    default_char.char_uuid.len = ESP_UUID_LEN_16;
    default_char.char_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_CHAR_UUID_DEFAULT;
    default_char.char_perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
    default_char.char_property = ESP_GATT_CHAR_PROP_BIT_READ |
                                 ESP_GATT_CHAR_PROP_BIT_WRITE |
                                 ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
    default_char.char_val.attr_max_len = POOM_BLE_GATT_DYNAMIC_CHAR_VAL_MAX_LEN_DEFAULT;
    default_char.char_val.attr_len = sizeof(s_default_char_value);
    default_char.char_val.attr_value = s_default_char_value;

    if (!poom_ble_gatt_dynamic_alloc_chars_(ctx, 1U))
    {
        return false;
    }

    if (!poom_ble_gatt_dynamic_apply_char_(&ctx->chars[0], &default_char))
    {
        poom_ble_gatt_dynamic_free_chars_(ctx);
        return false;
    }

    poom_ble_gatt_dynamic_build_adv_payloads_(ctx);
    return true;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return void
 */
static void poom_ble_gatt_dynamic_start_advertising_if_ready_(void)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = s_ctx;

    if ((ctx != NULL) && (ctx->adv_config_done == 0U) && !ctx->stopping)
    {
        esp_err_t ret = esp_ble_gap_start_advertising(&ctx->adv_params);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_E("start advertising failed: 0x%x", ret);
        }
    }
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] event Parameter passed to the function.
 * @param[in] param Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_gap_cb_(esp_gap_ble_cb_event_t event,
                                          esp_ble_gap_cb_param_t *param)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = s_ctx;

    if (param == NULL)
    {
        return;
    }

    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (ctx != NULL)
            {
                ctx->adv_config_done &= (uint8_t)(~POOM_BLE_GATT_DYNAMIC_ADV_CONFIG_FLAG);
            }
            poom_ble_gatt_dynamic_start_advertising_if_ready_();
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            if (ctx != NULL)
            {
                ctx->adv_config_done &= (uint8_t)(~POOM_BLE_GATT_DYNAMIC_SCAN_RSP_CONFIG_FLAG);
            }
            poom_ble_gatt_dynamic_start_advertising_if_ready_();
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("advertising start failed status=0x%x",
                                               param->adv_start_cmpl.status);
            }
            else
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_I("advertising started name=%s",
                                               (ctx != NULL) ? ctx->device_name : "");
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("advertising stop failed status=0x%x",
                                               param->adv_stop_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            POOM_BLE_GATT_DYNAMIC_PRINTF_I("conn params: ci=%d lat=%d to=%d",
                                           param->update_conn_params.conn_int,
                                           param->update_conn_params.latency,
                                           param->update_conn_params.timeout);
            break;

        default:
            break;
    }
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_add_next_char`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_add_next_char_(poom_ble_gatt_dynamic_ctx_t *ctx)
{
    esp_err_t ret;
    poom_ble_gatt_dynamic_char_ctx_t *char_ctx;

    if ((ctx == NULL) || (ctx->next_char_index >= ctx->char_count))
    {
        return;
    }

    char_ctx = &ctx->chars[ctx->next_char_index];
    ret = esp_ble_gatts_add_char(ctx->service_handle,
                                 &char_ctx->char_uuid,
                                 char_ctx->char_perm,
                                 char_ctx->char_property,
                                 &char_ctx->char_value,
                                 (esp_attr_control_t *)&s_auto_rsp);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_DYNAMIC_PRINTF_E("add char %u failed: 0x%x",
                                       (unsigned)ctx->next_char_index,
                                       ret);
        return;
    }

    ctx->next_char_index++;
}

/**
 * @brief Handles the current module action.
 *
 * @param[in] char_count Parameter passed to the function.
 * @return uint16_t
 */
static uint16_t poom_ble_gatt_dynamic_service_handle_count_(size_t char_count)
{
    uint16_t handle_count = (uint16_t)(POOM_BLE_GATT_DYNAMIC_HANDLE_BASE_COUNT +
                                       (char_count * POOM_BLE_GATT_DYNAMIC_HANDLE_PER_CHAR));

    if (handle_count < 4U)
    {
        handle_count = 4U;
    }

    return handle_count;
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] event Parameter passed to the function.
 * @param[in] gatts_if Parameter passed to the function.
 * @param[in] param Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_profile_cb_(esp_gatts_cb_event_t event,
                                              esp_gatt_if_t gatts_if,
                                              esp_ble_gatts_cb_param_t *param)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = s_ctx;
    esp_gatt_srvc_id_t service_id = {0};

    if ((param == NULL) || (ctx == NULL))
    {
        return;
    }

    switch (event)
    {
        case ESP_GATTS_REG_EVT:
        {
            esp_err_t ret;

            ret = esp_ble_gap_set_device_name(ctx->device_name);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("set device name failed: 0x%x", ret);
            }

            ret = esp_ble_gap_config_adv_data_raw(ctx->adv_raw_data, ctx->adv_raw_len);
            if (ret == ESP_OK)
            {
                ctx->adv_config_done |= POOM_BLE_GATT_DYNAMIC_ADV_CONFIG_FLAG;
            }
            else
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("config raw adv failed: 0x%x", ret);
            }

            ret = esp_ble_gap_config_scan_rsp_data_raw((ctx->scan_rsp_raw_len > 0U) ? ctx->scan_rsp_raw_data : NULL,
                                                       ctx->scan_rsp_raw_len);
            if (ret == ESP_OK)
            {
                ctx->adv_config_done |= POOM_BLE_GATT_DYNAMIC_SCAN_RSP_CONFIG_FLAG;
            }
            else
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("config raw scan rsp failed: 0x%x", ret);
            }

            service_id.is_primary = true;
            service_id.id.inst_id = POOM_BLE_GATT_DYNAMIC_SVC_INST_ID;
            service_id.id.uuid = ctx->service_uuid;
            ret = esp_ble_gatts_create_service(gatts_if,
                                               &service_id,
                                               poom_ble_gatt_dynamic_service_handle_count_(ctx->char_count));
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("create service failed: 0x%x", ret);
            }
        }
        break;

        case ESP_GATTS_CREATE_EVT:
        {
            esp_err_t ret;

            ctx->service_handle = param->create.service_handle;
            ctx->next_char_index = 0U;
            ret = esp_ble_gatts_start_service(ctx->service_handle);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_DYNAMIC_PRINTF_E("start service failed: 0x%x", ret);
            }

            poom_ble_gatt_dynamic_add_next_char_(ctx);
        }
        break;

        case ESP_GATTS_ADD_CHAR_EVT:
            if ((ctx->next_char_index > 0U) && (ctx->next_char_index <= ctx->char_count))
            {
                ctx->chars[ctx->next_char_index - 1U].char_handle = param->add_char.attr_handle;
            }
            poom_ble_gatt_dynamic_add_next_char_(ctx);
            break;

        case ESP_GATTS_START_EVT:
            POOM_BLE_GATT_DYNAMIC_PRINTF_I("service started handle=%d", param->start.service_handle);
            break;

        case ESP_GATTS_MTU_EVT:
            POOM_BLE_GATT_DYNAMIC_PRINTF_I("MTU=%d", param->mtu.mtu);
            break;

        case ESP_GATTS_CONNECT_EVT:
        {
            esp_ble_conn_update_params_t conn_params = {0};

            ctx->connected = true;
            ctx->conn_id = param->connect.conn_id;
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.latency = POOM_BLE_GATT_DYNAMIC_CONN_LATENCY;
            conn_params.max_int = POOM_BLE_GATT_DYNAMIC_CONN_MAX_INT;
            conn_params.min_int = POOM_BLE_GATT_DYNAMIC_CONN_MIN_INT;
            conn_params.timeout = POOM_BLE_GATT_DYNAMIC_CONN_TIMEOUT;
            (void)esp_ble_gap_update_conn_params(&conn_params);
        }
        break;

        case ESP_GATTS_DISCONNECT_EVT:
            ctx->connected = false;
            if (!ctx->stopping)
            {
                (void)esp_ble_gap_start_advertising(&ctx->adv_params);
            }
            break;

        case ESP_GATTS_WRITE_EVT:
        {
            size_t char_index = 0U;
            poom_ble_gatt_dynamic_char_ctx_t *char_ctx =
                poom_ble_gatt_dynamic_find_char_by_handle_(ctx, param->write.handle, &char_index);
            char service_uuid[40] = {0};
            char char_uuid[40] = {0};

            if (char_ctx == NULL)
            {
                break;
            }

            if (param->write.is_prep)
            {
                if (param->write.need_rsp)
                {
                    (void)esp_ble_gatts_send_response(gatts_if,
                                                      param->write.conn_id,
                                                      param->write.trans_id,
                                                      ESP_GATT_REQ_NOT_SUPPORTED,
                                                      NULL);
                }
                break;
            }

            if (param->write.len <= char_ctx->char_value_capacity)
            {
                memcpy(char_ctx->char_value_buffer, param->write.value, param->write.len);
                char_ctx->char_value.attr_len = param->write.len;
                (void)esp_ble_gatts_set_attr_value(char_ctx->char_handle,
                                                   char_ctx->char_value.attr_len,
                                                   char_ctx->char_value_buffer);
            }

            if (param->write.need_rsp)
            {
                (void)esp_ble_gatts_send_response(gatts_if,
                                                  param->write.conn_id,
                                                  param->write.trans_id,
                                                  (param->write.len <= char_ctx->char_value_capacity) ? ESP_GATT_OK
                                                                                                     : ESP_GATT_INVALID_ATTR_LEN,
                                                  NULL);
            }

            poom_ble_gatt_dynamic_format_uuid_(&ctx->service_uuid, service_uuid, sizeof(service_uuid));
            poom_ble_gatt_dynamic_format_uuid_(&char_ctx->char_uuid, char_uuid, sizeof(char_uuid));
            printf("ble-gatt write | service=%s | char[%u]=%s | value:",
                   service_uuid,
                   (unsigned)char_index,
                   char_uuid);
            poom_ble_gatt_dynamic_print_bytes_(param->write.value, param->write.len);
            printf("\n");
        }
            break;

        default:
            break;
    }
}

/**
 * @brief Internal helper for `poom_ble_gatt_dynamic_gatts_dispatcher`.
 *
 * @param[in] event Parameter passed to the function.
 * @param[in] gatts_if Parameter passed to the function.
 * @param[in] param Parameter passed to the function.
 * @return void
 */
static void poom_ble_gatt_dynamic_gatts_dispatcher_(esp_gatts_cb_event_t event,
                                                    esp_gatt_if_t gatts_if,
                                                    esp_ble_gatts_cb_param_t *param)
{
    if ((event == ESP_GATTS_REG_EVT) && (param != NULL))
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            if (s_ctx != NULL)
            {
                s_ctx->gatts_if = gatts_if;
            }
        }
        else
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_E("register app failed status=%d", param->reg.status);
            return;
        }
    }

    poom_ble_gatt_dynamic_profile_cb_(event, gatts_if, param);
}

esp_attr_value_t poom_ble_gatt_dynamic_default_char_val(void)
{
    esp_attr_value_t value = {
        .attr_max_len = POOM_BLE_GATT_DYNAMIC_CHAR_VAL_MAX_LEN_DEFAULT,
        .attr_len = sizeof(s_default_char_value),
        .attr_value = s_default_char_value,
    };

    return value;
}

esp_ble_adv_params_t poom_ble_gatt_dynamic_default_adv_params(void)
{
    esp_ble_adv_params_t value = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    return value;
}

void poom_ble_gatt_dynamic_set_config(const poom_ble_gatt_dynamic_config_t *config)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = poom_ble_gatt_dynamic_ctx_ensure_();
    size_t name_len;
    size_t char_count;

    if (ctx == NULL)
    {
        return;
    }

    if (config == NULL)
    {
        (void)poom_ble_gatt_dynamic_reset_defaults_(ctx);
        return;
    }

    ctx->adv_params = config->adv_params;
    if ((ctx->adv_params.adv_int_min == 0U) || (ctx->adv_params.adv_int_max == 0U))
    {
        ctx->adv_params = poom_ble_gatt_dynamic_default_adv_params();
    }

    memset(ctx->device_name, 0, POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN + 1U);
    if ((config->bt_props.device_name != NULL) && (config->bt_props.device_name[0] != '\0'))
    {
        name_len = strnlen(config->bt_props.device_name, POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN);
        memcpy(ctx->device_name, config->bt_props.device_name, name_len);
        ctx->device_name[name_len] = '\0';
    }
    else
    {
        (void)snprintf(ctx->device_name,
                       POOM_BLE_GATT_DYNAMIC_NAME_MAX_LEN + 1U,
                       "%s",
                       POOM_BLE_GATT_DYNAMIC_DEVICE_NAME_DEFAULT);
    }

    ctx->service_uuid = config->profile.service_uuid;
    if (poom_ble_gatt_dynamic_uuid_len_(&ctx->service_uuid) == 0U)
    {
        ctx->service_uuid.len = ESP_UUID_LEN_16;
        ctx->service_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_SERVICE_UUID_DEFAULT;
    }

    poom_ble_gatt_dynamic_free_chars_(ctx);
    char_count = ((config->profile.chars != NULL) && (config->profile.char_count > 0U))
                     ? config->profile.char_count
                     : 1U;
    if (!poom_ble_gatt_dynamic_alloc_chars_(ctx, char_count))
    {
        (void)poom_ble_gatt_dynamic_reset_defaults_(ctx);
        return;
    }

    if ((config->profile.chars != NULL) && (config->profile.char_count > 0U))
    {
        for (size_t i = 0U; i < char_count; i++)
        {
            if (!poom_ble_gatt_dynamic_apply_char_(&ctx->chars[i], &config->profile.chars[i]))
            {
                (void)poom_ble_gatt_dynamic_reset_defaults_(ctx);
                return;
            }
        }
    }
    else
    {
        poom_ble_gatt_dynamic_char_config_t default_char = {0};

        default_char.char_uuid.len = ESP_UUID_LEN_16;
        default_char.char_uuid.uuid.uuid16 = POOM_BLE_GATT_DYNAMIC_CHAR_UUID_DEFAULT;
        default_char.char_perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        default_char.char_property = ESP_GATT_CHAR_PROP_BIT_READ |
                                     ESP_GATT_CHAR_PROP_BIT_WRITE |
                                     ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
        default_char.char_val = poom_ble_gatt_dynamic_default_char_val();
        if (!poom_ble_gatt_dynamic_apply_char_(&ctx->chars[0], &default_char))
        {
            (void)poom_ble_gatt_dynamic_reset_defaults_(ctx);
            return;
        }
    }

    poom_ble_gatt_dynamic_build_adv_payloads_(ctx);
}

esp_err_t poom_ble_gatt_dynamic_start(void)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = poom_ble_gatt_dynamic_ctx_ensure_();
    esp_err_t ret;
    esp_bt_controller_status_t bt_status;
    esp_bluedroid_status_t bluedroid_status;

    if (ctx == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (ctx->started)
    {
        return ESP_OK;
    }

    if ((ctx->device_name[0] == '\0') || (ctx->char_count == 0U))
    {
        if (!poom_ble_gatt_dynamic_reset_defaults_(ctx))
        {
            return ESP_ERR_NO_MEM;
        }
    }

    ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK)
    {
        return ret;
    }

    ctx->stopping = false;
    ctx->adv_config_done = 0U;
    ctx->connected = false;
    ctx->service_handle = 0U;
    ctx->conn_id = 0U;
    ctx->gatts_if = ESP_GATT_IF_NONE;
    ctx->next_char_index = 0U;
    for (size_t i = 0U; i < ctx->char_count; i++)
    {
        ctx->chars[i].char_handle = 0U;
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_DYNAMIC_PRINTF_W("controller mem release classic failed: 0x%x", ret);
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        ret = esp_bluedroid_init();
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    ret = esp_ble_gatts_register_callback(poom_ble_gatt_dynamic_gatts_dispatcher_);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_ble_gap_register_callback(poom_ble_gatt_dynamic_gap_cb_);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_ble_gatts_app_register(POOM_BLE_GATT_DYNAMIC_APP_ID);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_ble_gatt_set_local_mtu(POOM_BLE_GATT_DYNAMIC_LOCAL_MTU);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_DYNAMIC_PRINTF_W("set local mtu failed: 0x%x", ret);
    }

    ctx->started = true;
    POOM_BLE_GATT_DYNAMIC_PRINTF_I("started");
    return ESP_OK;
}

void poom_ble_gatt_dynamic_stop(void)
{
    poom_ble_gatt_dynamic_ctx_t *ctx = s_ctx;
    esp_err_t ret;
    esp_bt_controller_status_t bt_status;
    esp_bluedroid_status_t bluedroid_status;

    if ((ctx == NULL) || !ctx->started)
    {
        return;
    }

    ctx->stopping = true;

    if (ctx->connected && (ctx->gatts_if != ESP_GATT_IF_NONE))
    {
        ret = esp_ble_gatts_close(ctx->gatts_if, ctx->conn_id);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("close connection failed: 0x%x", ret);
        }
    }

    ret = esp_ble_gap_stop_advertising();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_DYNAMIC_PRINTF_W("stop advertising failed: 0x%x", ret);
    }

    if (ctx->gatts_if != ESP_GATT_IF_NONE)
    {
        ret = esp_ble_gatts_app_unregister(ctx->gatts_if);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("app unregister failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("bluedroid disable failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("bluedroid deinit failed: 0x%x", ret);
        }
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("controller disable failed: 0x%x", ret);
        }
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_DYNAMIC_PRINTF_W("controller deinit failed: 0x%x", ret);
        }
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_DYNAMIC_PRINTF_W("controller mem release ble failed: 0x%x", ret);
    }

    POOM_BLE_GATT_DYNAMIC_PRINTF_I("stopped");
    poom_ble_gatt_dynamic_ctx_free_();
}

bool poom_ble_gatt_dynamic_is_started(void)
{
    return (s_ctx != NULL) && s_ctx->started;
}
