// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_gatt_server.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"

/**
 * @file poom_ble_gatt_server.c
 * @brief BLE GATT server helper implementation.
 */

#ifndef POOM_BLE_GATT_SERVER_LOG_ENABLED
#define POOM_BLE_GATT_SERVER_LOG_ENABLED (1)
#endif

#ifndef POOM_BLE_GATT_SERVER_DEBUG_LOG_ENABLED
#define POOM_BLE_GATT_SERVER_DEBUG_LOG_ENABLED (0)
#endif

#if POOM_BLE_GATT_SERVER_LOG_ENABLED
static const char *POOM_BLE_GATT_SERVER_LOG_TAG = "poom_ble_gatt_server";

#define POOM_BLE_GATT_SERVER_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_SERVER_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_BLE_GATT_SERVER_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_SERVER_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_BLE_GATT_SERVER_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_SERVER_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#if POOM_BLE_GATT_SERVER_DEBUG_LOG_ENABLED
#define POOM_BLE_GATT_SERVER_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_SERVER_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define POOM_BLE_GATT_SERVER_PRINTF_D(...) do { } while (0)
#endif
#else
#define POOM_BLE_GATT_SERVER_PRINTF_E(...) do { } while (0)
#define POOM_BLE_GATT_SERVER_PRINTF_W(...) do { } while (0)
#define POOM_BLE_GATT_SERVER_PRINTF_I(...) do { } while (0)
#define POOM_BLE_GATT_SERVER_PRINTF_D(...) do { } while (0)
#endif

#define POOM_BLE_GATT_SERVER_PROFILE_COUNT                  (1U)
#define POOM_BLE_GATT_SERVER_PROFILE_ID                     (0U)
#define POOM_BLE_GATT_SERVER_DEVICE_NAME_DEFAULT            "EC_BLE_SERVER"
#define POOM_BLE_GATT_SERVER_NUM_HANDLES                    (4U)
#define POOM_BLE_GATT_SERVER_APP_ID                         (0U)
#define POOM_BLE_GATT_SERVER_SERVICE_INSTANCE_ID            (0U)
#define POOM_BLE_GATT_SERVER_LOCAL_MTU                      (500U)

#define POOM_BLE_GATT_SERVER_ADV_CONFIG_FLAG               (1U << 0)
#define POOM_BLE_GATT_SERVER_SCAN_RSP_CONFIG_FLAG          (1U << 1)

static uint8_t s_default_manufacturer_data[POOM_BLE_GATT_SERVER_MANUFACTURER_DATA_LEN_DEFAULT] = {
    0x12, 0x23, 0x45, 0x56
};

static const uint8_t s_adv_service_uuid128[32] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xEE, 0x00, 0x00, 0x00,
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

static uint8_t s_default_char_value[] = {0x61, 0x70, 0x70, 0x73, 0x65, 0x63};

/**
 * @brief Internal prepare-write buffer state.
 */
typedef struct
{
    uint8_t *prepare_buf;
    int prepare_len;
} poom_ble_gatt_server_prepare_env_t;

/**
 * @brief Internal GATTS profile state.
 */
typedef struct
{
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
} poom_ble_gatt_server_profile_inst_t;

static void poom_ble_gatt_server_profile_event_handler_(esp_gatts_cb_event_t event,
                                                  esp_gatt_if_t gatts_if,
                                                  esp_ble_gatts_cb_param_t *param);
static void poom_ble_gatt_server_gap_event_handler_(esp_gap_ble_cb_event_t event,
                                              esp_ble_gap_cb_param_t *param);
static void poom_ble_gatt_server_event_dispatcher_(esp_gatts_cb_event_t event,
                                             esp_gatt_if_t gatts_if,
                                             esp_ble_gatts_cb_param_t *param);
static void poom_ble_gatt_server_prepare_write_event_(esp_gatt_if_t gatts_if,
                                                poom_ble_gatt_server_prepare_env_t *prepare_env,
                                                esp_ble_gatts_cb_param_t *param);
static void poom_ble_gatt_server_exec_write_event_(poom_ble_gatt_server_prepare_env_t *prepare_env,
                                             esp_ble_gatts_cb_param_t *param);

static uint8_t s_adv_config_done = 0U;
static esp_gatt_char_prop_t s_char_property = 0U;
static bool s_started = false;
static bool s_adv_params_set = false;

static poom_ble_gatt_server_prepare_env_t s_prepare_write_env = {0};
static poom_ble_gatt_server_event_cb_t s_event_cb = {0};

static esp_attr_value_t s_char_val;
static esp_ble_adv_data_t s_adv_data;
static esp_ble_adv_data_t s_scan_rsp_data;
static esp_ble_adv_params_t s_adv_params;
static poom_ble_gatt_server_props_t s_props = {
    .device_name = POOM_BLE_GATT_SERVER_DEVICE_NAME_DEFAULT,
    .manufacturer_data = s_default_manufacturer_data,
};

static poom_ble_gatt_server_profile_inst_t s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_COUNT] = {
    [POOM_BLE_GATT_SERVER_PROFILE_ID] = {
        .gatts_cb = poom_ble_gatt_server_profile_event_handler_,
        .gatts_if = ESP_GATT_IF_NONE,
    }
};

/**
 * @brief Dumps payload bytes in hex format.
 */
static void poom_ble_gatt_server_log_hex_(const uint8_t *data, size_t len)
{
    char line[16U * 3U + 1U];

    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    for (size_t i = 0; i < len; i += 16U)
    {
        size_t off = 0U;
        size_t chunk = ((len - i) > 16U) ? 16U : (len - i);

        memset(line, 0, sizeof(line));
        for (size_t j = 0; j < chunk; ++j)
        {
            off += (size_t)snprintf(&line[off], sizeof(line) - off, "%02X ", data[i + j]);
            if (off >= sizeof(line))
            {
                break;
            }
        }

        POOM_BLE_GATT_SERVER_PRINTF_D("HEX[%u]: %s", (unsigned)i, line);
    }
}

/**
 * @brief Dumps payload as printable ASCII.
 */
static void poom_ble_gatt_server_log_ascii_(const uint8_t *data, size_t len)
{
    char text[64];

    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    size_t n = (len < (sizeof(text) - 1U)) ? len : (sizeof(text) - 1U);
    for (size_t i = 0U; i < n; ++i)
    {
        text[i] = isprint(data[i]) ? (char)data[i] : '.';
    }
    text[n] = '\0';

    POOM_BLE_GATT_SERVER_PRINTF_D("ASCII: %s", text);
}

/**
 * @brief Applies default advertising/characteristic parameters.
 */
static void poom_ble_gatt_server_set_defaults_(void)
{
    s_char_val = poom_ble_gatt_server_default_char_val();
    s_adv_data = poom_ble_gatt_server_default_adv_data();
    s_scan_rsp_data = poom_ble_gatt_server_default_scan_rsp_data();
    s_adv_params = poom_ble_gatt_server_default_adv_params();
    s_props.device_name = POOM_BLE_GATT_SERVER_DEVICE_NAME_DEFAULT;
    s_props.manufacturer_data = s_default_manufacturer_data;
    s_scan_rsp_data.p_manufacturer_data = (uint8_t *)s_props.manufacturer_data;
    s_scan_rsp_data.manufacturer_len = POOM_BLE_GATT_SERVER_MANUFACTURER_DATA_LEN_DEFAULT;
    s_adv_params_set = true;
}

esp_attr_value_t poom_ble_gatt_server_default_char_val(void)
{
    esp_attr_value_t value = {
        .attr_max_len = POOM_BLE_GATT_SERVER_CHAR_VAL_MAX_LEN_DEFAULT,
        .attr_len = sizeof(s_default_char_value),
        .attr_value = s_default_char_value,
    };

    return value;
}

esp_ble_adv_data_t poom_ble_gatt_server_default_adv_data(void)
{
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(s_adv_service_uuid128),
        .p_service_uuid = (uint8_t *)s_adv_service_uuid128,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    return adv_data;
}

esp_ble_adv_data_t poom_ble_gatt_server_default_scan_rsp_data(void)
{
    esp_ble_adv_data_t scan_rsp = {
        .set_scan_rsp = true,
        .include_name = true,
        .include_txpower = true,
        .appearance = 0x00,
        .manufacturer_len = POOM_BLE_GATT_SERVER_MANUFACTURER_DATA_LEN_DEFAULT,
        .p_manufacturer_data = s_default_manufacturer_data,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(s_adv_service_uuid128),
        .p_service_uuid = (uint8_t *)s_adv_service_uuid128,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    return scan_rsp;
}

esp_ble_adv_params_t poom_ble_gatt_server_default_adv_params(void)
{
    esp_ble_adv_params_t adv_params = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    return adv_params;
}

void poom_ble_gatt_server_set_adv_data_params(const poom_ble_gatt_server_adv_params_t *adv_params)
{
    if (adv_params == NULL)
    {
        poom_ble_gatt_server_set_defaults_();
        return;
    }

    s_adv_data = adv_params->adv_data;
    s_scan_rsp_data = adv_params->scan_rsp_data;
    s_adv_params = adv_params->adv_params;
    s_char_val = adv_params->char_val;
    s_props = adv_params->bt_props;

    if ((s_props.device_name == NULL) || (s_props.device_name[0] == '\0'))
    {
        s_props.device_name = POOM_BLE_GATT_SERVER_DEVICE_NAME_DEFAULT;
    }
    if (s_props.manufacturer_data == NULL)
    {
        s_props.manufacturer_data = s_default_manufacturer_data;
    }

    s_scan_rsp_data.p_manufacturer_data = (uint8_t *)s_props.manufacturer_data;
    if (s_scan_rsp_data.manufacturer_len == 0U)
    {
        s_scan_rsp_data.manufacturer_len = POOM_BLE_GATT_SERVER_MANUFACTURER_DATA_LEN_DEFAULT;
    }

    s_adv_params_set = true;
}

void poom_ble_gatt_server_set_callbacks(poom_ble_gatt_server_event_cb_t event_cb)
{
    s_event_cb = event_cb;
}

void poom_ble_gatt_server_send_data(uint8_t *data, int length)
{
    if ((data == NULL) || (length <= 0))
    {
        return;
    }

    if (!s_started)
    {
        POOM_BLE_GATT_SERVER_PRINTF_W("send ignored: stack not started");
        return;
    }

    (void)esp_ble_gatts_send_indicate(
        s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].gatts_if,
        s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].conn_id,
        s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_handle,
        length,
        data,
        true);
}

/**
 * @brief Handles prepare writes.
 */
static void poom_ble_gatt_server_prepare_write_event_(esp_gatt_if_t gatts_if,
                                                poom_ble_gatt_server_prepare_env_t *prepare_env,
                                                esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_status_t status = ESP_GATT_OK;

    if ((prepare_env == NULL) || (param == NULL))
    {
        return;
    }

    if (param->write.need_rsp)
    {
        if (param->write.is_prep)
        {
            if (param->write.offset > POOM_BLE_GATT_SERVER_PREPARE_BUF_MAX_SIZE_DEFAULT)
            {
                status = ESP_GATT_INVALID_OFFSET;
            }
            else if ((param->write.offset + param->write.len) > POOM_BLE_GATT_SERVER_PREPARE_BUF_MAX_SIZE_DEFAULT)
            {
                status = ESP_GATT_INVALID_ATTR_LEN;
            }

            if ((status == ESP_GATT_OK) && (prepare_env->prepare_buf == NULL))
            {
                prepare_env->prepare_buf = (uint8_t *)malloc(POOM_BLE_GATT_SERVER_PREPARE_BUF_MAX_SIZE_DEFAULT);
                prepare_env->prepare_len = 0;
                if (prepare_env->prepare_buf == NULL)
                {
                    status = ESP_GATT_NO_RESOURCES;
                }
            }

            esp_gatt_rsp_t *rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            if (rsp != NULL)
            {
                rsp->attr_value.len = param->write.len;
                rsp->attr_value.handle = param->write.handle;
                rsp->attr_value.offset = param->write.offset;
                rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
                memcpy(rsp->attr_value.value, param->write.value, param->write.len);

                (void)esp_ble_gatts_send_response(gatts_if,
                                                  param->write.conn_id,
                                                  param->write.trans_id,
                                                  status,
                                                  rsp);
                free(rsp);
            }
            else
            {
                status = ESP_GATT_NO_RESOURCES;
            }

            if (status != ESP_GATT_OK)
            {
                return;
            }

            memcpy(prepare_env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            prepare_env->prepare_len += param->write.len;
        }
        else
        {
            (void)esp_ble_gatts_send_response(gatts_if,
                                              param->write.conn_id,
                                              param->write.trans_id,
                                              status,
                                              NULL);
        }
    }
}

/**
 * @brief Handles execute-write event and clears temporary prepare buffer.
 */
static void poom_ble_gatt_server_exec_write_event_(poom_ble_gatt_server_prepare_env_t *prepare_env,
                                             esp_ble_gatts_cb_param_t *param)
{
    if ((prepare_env == NULL) || (param == NULL))
    {
        return;
    }

    if ((param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) &&
        (prepare_env->prepare_buf != NULL))
    {
        poom_ble_gatt_server_log_hex_(prepare_env->prepare_buf, (size_t)prepare_env->prepare_len);
        poom_ble_gatt_server_log_ascii_(prepare_env->prepare_buf, (size_t)prepare_env->prepare_len);
    }

    free(prepare_env->prepare_buf);
    prepare_env->prepare_buf = NULL;
    prepare_env->prepare_len = 0;
}

/**
 * @brief GAP callback.
 */
static void poom_ble_gatt_server_gap_event_handler_(esp_gap_ble_cb_event_t event,
                                              esp_ble_gap_cb_param_t *param)
{
    if (param == NULL)
    {
        return;
    }

    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~POOM_BLE_GATT_SERVER_ADV_CONFIG_FLAG);
            if (s_adv_config_done == 0U)
            {
                (void)esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~POOM_BLE_GATT_SERVER_SCAN_RSP_CONFIG_FLAG);
            if (s_adv_config_done == 0U)
            {
                (void)esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("advertising start failed status=0x%x", param->adv_start_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("advertising stop failed status=0x%x", param->adv_stop_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            POOM_BLE_GATT_SERVER_PRINTF_D("conn params status=%d min=%d max=%d int=%d lat=%d to=%d",
                                   param->update_conn_params.status,
                                   param->update_conn_params.min_int,
                                   param->update_conn_params.max_int,
                                   param->update_conn_params.conn_int,
                                   param->update_conn_params.latency,
                                   param->update_conn_params.timeout);
            break;

        default:
            break;
    }

    if (s_event_cb.handler_gap_cb != NULL)
    {
        s_event_cb.handler_gap_cb(event, param);
    }
}

/**
 * @brief Per-profile GATTS event handler.
 */
static void poom_ble_gatt_server_profile_event_handler_(esp_gatts_cb_event_t event,
                                                  esp_gatt_if_t gatts_if,
                                                  esp_ble_gatts_cb_param_t *param)
{
    if (param == NULL)
    {
        return;
    }

    switch (event)
    {
        case ESP_GATTS_REG_EVT:
        {
            esp_err_t ret;

            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_id.is_primary = true;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_id.id.inst_id = POOM_BLE_GATT_SERVER_SERVICE_INSTANCE_ID;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_id.id.uuid.uuid.uuid16 = POOM_BLE_GATT_SERVER_SERVICE_UUID_DEFAULT;

            ret = esp_ble_gap_set_device_name(s_props.device_name);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("set device name failed: 0x%x", ret);
            }

            ret = esp_ble_gap_config_adv_data(&s_adv_data);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("config adv data failed: 0x%x", ret);
            }
            s_adv_config_done |= POOM_BLE_GATT_SERVER_ADV_CONFIG_FLAG;

            ret = esp_ble_gap_config_adv_data(&s_scan_rsp_data);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("config scan rsp failed: 0x%x", ret);
            }
            s_adv_config_done |= POOM_BLE_GATT_SERVER_SCAN_RSP_CONFIG_FLAG;

            (void)esp_ble_gatts_create_service(
                gatts_if,
                &s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_id,
                POOM_BLE_GATT_SERVER_NUM_HANDLES);
        }
        break;

        case ESP_GATTS_READ_EVT:
            POOM_BLE_GATT_SERVER_PRINTF_D("read conn=%d handle=%d",
                                   param->read.conn_id,
                                   param->read.handle);
            break;

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep)
            {
                if ((s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_handle == param->write.handle) &&
                    (param->write.len == 2U))
                {
                    uint16_t descr_value = (uint16_t)((param->write.value[1] << 8) | param->write.value[0]);

                    if (descr_value == 0x0002U)
                    {
                        if ((s_char_property & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0U)
                        {
                            uint8_t indicate_data[15];
                            for (size_t i = 0U; i < sizeof(indicate_data); ++i)
                            {
                                indicate_data[i] = (uint8_t)(i & 0xFFU);
                            }

                            (void)esp_ble_gatts_send_indicate(
                                gatts_if,
                                param->write.conn_id,
                                s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_handle,
                                sizeof(indicate_data),
                                indicate_data,
                                true);
                        }
                    }
                }
            }

            poom_ble_gatt_server_prepare_write_event_(gatts_if, &s_prepare_write_env, param);
            break;

        case ESP_GATTS_EXEC_WRITE_EVT:
            (void)esp_ble_gatts_send_response(gatts_if,
                                              param->exec_write.conn_id,
                                              param->exec_write.trans_id,
                                              ESP_GATT_OK,
                                              NULL);
            poom_ble_gatt_server_exec_write_event_(&s_prepare_write_env, param);
            break;

        case ESP_GATTS_MTU_EVT:
            POOM_BLE_GATT_SERVER_PRINTF_I("MTU=%d", param->mtu.mtu);
            break;

        case ESP_GATTS_CREATE_EVT:
        {
            esp_err_t ret;

            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_handle = param->create.service_handle;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_uuid.len = ESP_UUID_LEN_16;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_uuid.uuid.uuid16 = POOM_BLE_GATT_SERVER_CHAR_UUID_DEFAULT;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_uuid.len = ESP_UUID_LEN_16;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_uuid.uuid.uuid16 = POOM_BLE_GATT_SERVER_DESCR_UUID_DEFAULT;

            (void)esp_ble_gatts_start_service(s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_handle);

            s_char_property = ESP_GATT_CHAR_PROP_BIT_READ |
                              ESP_GATT_CHAR_PROP_BIT_WRITE_NR |
                              ESP_GATT_CHAR_PROP_BIT_NOTIFY;

            ret = esp_ble_gatts_add_char(
                s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_handle,
                &s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                s_char_property,
                &s_char_val,
                NULL);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("add char failed: 0x%x", ret);
            }
        }
        break;

        case ESP_GATTS_ADD_CHAR_EVT:
        {
            esp_err_t ret;

            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_handle = param->add_char.attr_handle;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_uuid.len = ESP_UUID_LEN_16;
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

            ret = esp_ble_gatts_add_char_descr(
                s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_handle,
                &s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                NULL,
                NULL);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_E("add char descriptor failed: 0x%x", ret);
            }
        }
        break;

        case ESP_GATTS_ADD_CHAR_DESCR_EVT:
            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_handle = param->add_char_descr.attr_handle;
            break;

        case ESP_GATTS_START_EVT:
            POOM_BLE_GATT_SERVER_PRINTF_I("service started handle=%d", param->start.service_handle);
            break;

        case ESP_GATTS_CONNECT_EVT:
        {
            esp_ble_conn_update_params_t conn_params = {0};

            s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].conn_id = param->connect.conn_id;
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.latency = 0;
            conn_params.max_int = 0x20;
            conn_params.min_int = 0x10;
            conn_params.timeout = 400;

            (void)esp_ble_gap_update_conn_params(&conn_params);
        }
        break;

        case ESP_GATTS_DISCONNECT_EVT:
            POOM_BLE_GATT_SERVER_PRINTF_I("disconnect reason=0x%x", param->disconnect.reason);
            (void)esp_ble_gap_start_advertising(&s_adv_params);
            break;

        case ESP_GATTS_CONF_EVT:
            if (param->conf.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_SERVER_PRINTF_W("confirm status=%d", param->conf.status);
                poom_ble_gatt_server_log_hex_(param->conf.value, param->conf.len);
                poom_ble_gatt_server_log_ascii_(param->conf.value, param->conf.len);
            }
            break;

        default:
            break;
    }

    if (s_event_cb.handler_server_cb != NULL)
    {
        s_event_cb.handler_server_cb(event, param);
    }
}

/**
 * @brief Global GATTS dispatcher.
 */
static void poom_ble_gatt_server_event_dispatcher_(esp_gatts_cb_event_t event,
                                             esp_gatt_if_t gatts_if,
                                             esp_ble_gatts_cb_param_t *param)
{
    if ((event == ESP_GATTS_REG_EVT) && (param != NULL))
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            s_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        }
        else
        {
            POOM_BLE_GATT_SERVER_PRINTF_E("register app failed app_id=%04x status=%d",
                                   param->reg.app_id,
                                   param->reg.status);
            return;
        }
    }

    for (int i = 0; i < (int)POOM_BLE_GATT_SERVER_PROFILE_COUNT; ++i)
    {
        if ((gatts_if == ESP_GATT_IF_NONE) || (gatts_if == s_profile_tab[i].gatts_if))
        {
            if (s_profile_tab[i].gatts_cb != NULL)
            {
                s_profile_tab[i].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

void poom_ble_gatt_server_start(void)
{
    esp_err_t ret;
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;

    if (s_started)
    {
        return;
    }

    if (!s_adv_params_set)
    {
        poom_ble_gatt_server_set_defaults_();
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_SERVER_PRINTF_W("controller mem release classic failed: 0x%x", ret);
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&cfg);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_SERVER_PRINTF_E("controller init failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status != ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_SERVER_PRINTF_E("controller enable failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_SERVER_PRINTF_E("bluedroid init failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status != ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_SERVER_PRINTF_E("bluedroid enable failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_ble_gatts_register_callback(poom_ble_gatt_server_event_dispatcher_);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_SERVER_PRINTF_E("gatts callback register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gap_register_callback(poom_ble_gatt_server_gap_event_handler_);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_SERVER_PRINTF_E("gap callback register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gatts_app_register(POOM_BLE_GATT_SERVER_APP_ID);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_SERVER_PRINTF_E("gatts app register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gatt_set_local_mtu(POOM_BLE_GATT_SERVER_LOCAL_MTU);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_SERVER_PRINTF_E("set local mtu failed: 0x%x", ret);
    }

    s_started = true;
    POOM_BLE_GATT_SERVER_PRINTF_I("started");
}

void poom_ble_gatt_server_stop(void)
{
    esp_err_t ret;
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;

    if (!s_started)
    {
        return;
    }

    free(s_prepare_write_env.prepare_buf);
    s_prepare_write_env.prepare_buf = NULL;
    s_prepare_write_env.prepare_len = 0;

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_SERVER_PRINTF_W("bluedroid disable failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_SERVER_PRINTF_W("bluedroid deinit failed: 0x%x", ret);
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_SERVER_PRINTF_W("controller disable failed: 0x%x", ret);
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_SERVER_PRINTF_W("controller deinit failed: 0x%x", ret);
        }
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_SERVER_PRINTF_W("controller mem release ble failed: 0x%x", ret);
    }

    s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].gatts_if = ESP_GATT_IF_NONE;
    s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].conn_id = 0U;
    s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].service_handle = 0U;
    s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].char_handle = 0U;
    s_profile_tab[POOM_BLE_GATT_SERVER_PROFILE_ID].descr_handle = 0U;
    s_adv_config_done = 0U;
    s_char_property = 0U;
    s_started = false;

    POOM_BLE_GATT_SERVER_PRINTF_I("stopped");
}
