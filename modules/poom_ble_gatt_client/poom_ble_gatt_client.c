// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_gatt_client.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"

/**
 * @file poom_ble_gatt_client.c
 * @brief BLE GATT client helper implementation.
 */


#if POOM_BLE_GATT_CLIENT_LOG_ENABLED
static const char *POOM_BLE_GATT_CLIENT_LOG_TAG = "poom_ble_gatt_client";

#define POOM_BLE_GATT_CLIENT_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_CLIENT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_BLE_GATT_CLIENT_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_CLIENT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_BLE_GATT_CLIENT_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_CLIENT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#if POOM_BLE_GATT_CLIENT_DEBUG_LOG_ENABLED
#define POOM_BLE_GATT_CLIENT_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_BLE_GATT_CLIENT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define POOM_BLE_GATT_CLIENT_PRINTF_D(...) do { } while (0)
#endif
#else
#define POOM_BLE_GATT_CLIENT_PRINTF_E(...) do { } while (0)
#define POOM_BLE_GATT_CLIENT_PRINTF_W(...) do { } while (0)
#define POOM_BLE_GATT_CLIENT_PRINTF_I(...) do { } while (0)
#define POOM_BLE_GATT_CLIENT_PRINTF_D(...) do { } while (0)
#endif

#define POOM_BLE_GATT_CLIENT_PROFILE_COUNT                 (1U)
#define POOM_BLE_GATT_CLIENT_PROFILE_ID                    (0U)
#define POOM_BLE_GATT_CLIENT_INVALID_HANDLE                (0U)
#define POOM_BLE_GATT_CLIENT_LOCAL_MTU                     (500U)
#define POOM_BLE_GATT_CLIENT_CONNECT_RSSI_THRESHOLD_DBM    (-50)

/**
 * @brief Internal per-profile state.
 */
typedef struct
{
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
} poom_ble_gatt_client_profile_inst_t;

static void poom_ble_gatt_client_profile_event_handler_(esp_gattc_cb_event_t event,
                                                  esp_gatt_if_t gattc_if,
                                                  esp_ble_gattc_cb_param_t *param);
static void poom_ble_gatt_client_gap_event_handler_(esp_gap_ble_cb_event_t event,
                                              esp_ble_gap_cb_param_t *param);
static void poom_ble_gatt_client_gattc_event_handler_(esp_gattc_cb_event_t event,
                                                esp_gatt_if_t gattc_if,
                                                esp_ble_gattc_cb_param_t *param);

static poom_ble_gatt_client_profile_inst_t s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_COUNT] = {
    [POOM_BLE_GATT_CLIENT_PROFILE_ID] = {
        .gattc_cb = poom_ble_gatt_client_profile_event_handler_,
        .gattc_if = ESP_GATT_IF_NONE,
    }
};

static char s_remote_device_name[POOM_BLE_GATT_CLIENT_REMOTE_NAME_MAX_LEN] = {0};
static bool s_search_by_name = false;
static bool s_is_connected = false;
static bool s_server_attached = false;
static bool s_started = false;

static bool s_scan_params_set = false;
static esp_bt_uuid_t s_remote_filter_service_uuid;
static esp_bt_uuid_t s_remote_filter_char_uuid;
static esp_bt_uuid_t s_notify_descr_uuid;
static esp_ble_scan_params_t s_ble_scan_params;

static poom_ble_gatt_client_event_cb_t s_event_cb = {0};

/**
 * @brief Dumps a BLE address in canonical format.
 */
static void poom_ble_gatt_client_log_bda_(const esp_bd_addr_t bda)
{
    POOM_BLE_GATT_CLIENT_PRINTF_I("BDA %02X:%02X:%02X:%02X:%02X:%02X",
                           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/**
 * @brief Dumps payload bytes in hex format.
 */
static void poom_ble_gatt_client_log_hex_(const uint8_t *data, size_t len)
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
        for (size_t j = 0U; j < chunk; ++j)
        {
            off += (size_t)snprintf(&line[off], sizeof(line) - off, "%02X ", data[i + j]);
            if (off >= sizeof(line))
            {
                break;
            }
        }

        POOM_BLE_GATT_CLIENT_PRINTF_D("HEX[%u]: %s", (unsigned)i, line);
    }
}

/**
 * @brief Dumps payload as printable ASCII.
 */
static void poom_ble_gatt_client_log_ascii_(const uint8_t *data, size_t len)
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

    POOM_BLE_GATT_CLIENT_PRINTF_D("ASCII: %s", text);
}

/**
 * @brief Checks if discovered service matches current filter UUID.
 */
static bool poom_ble_gatt_client_service_match_(const esp_gatt_id_t *service_id)
{
    if (service_id == NULL)
    {
        return false;
    }

    if (s_remote_filter_service_uuid.len != service_id->uuid.len)
    {
        return false;
    }

    if (service_id->uuid.len == ESP_UUID_LEN_16)
    {
        return (service_id->uuid.uuid.uuid16 == s_remote_filter_service_uuid.uuid.uuid16);
    }

    if (service_id->uuid.len == ESP_UUID_LEN_32)
    {
        return (service_id->uuid.uuid.uuid32 == s_remote_filter_service_uuid.uuid.uuid32);
    }

    if (service_id->uuid.len == ESP_UUID_LEN_128)
    {
        return (memcmp(service_id->uuid.uuid.uuid128,
                       s_remote_filter_service_uuid.uuid.uuid128,
                       ESP_UUID_LEN_128) == 0);
    }

    return false;
}

/**
 * @brief Initializes default filter/scan parameters.
 */
static void poom_ble_gatt_client_set_defaults_(void)
{
    s_remote_filter_service_uuid = poom_ble_gatt_client_default_service_uuid();
    s_remote_filter_char_uuid = poom_ble_gatt_client_default_char_uuid();
    s_notify_descr_uuid = poom_ble_gatt_client_default_notify_descr_uuid();
    s_ble_scan_params = poom_ble_gatt_client_default_scan_params();
    s_scan_params_set = true;
}

esp_bt_uuid_t poom_ble_gatt_client_default_service_uuid(void)
{
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {
            .uuid16 = POOM_BLE_GATT_CLIENT_REMOTE_SERVICE_UUID_DEFAULT,
        },
    };

    return uuid;
}

esp_bt_uuid_t poom_ble_gatt_client_default_char_uuid(void)
{
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {
            .uuid16 = POOM_BLE_GATT_CLIENT_REMOTE_NOTIFY_CHAR_UUID_DEFAULT,
        },
    };

    return uuid;
}

esp_bt_uuid_t poom_ble_gatt_client_default_notify_descr_uuid(void)
{
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {
            .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
        },
    };

    return uuid;
}

esp_ble_scan_params_t poom_ble_gatt_client_default_scan_params(void)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x0003,
        .scan_window = 0x0003,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };

    return scan_params;
}

void poom_ble_gatt_client_set_remote_device_name(const char *device_name)
{
    if ((device_name == NULL) || (device_name[0] == '\0'))
    {
        s_remote_device_name[0] = '\0';
        s_search_by_name = false;
        return;
    }

    (void)strncpy(s_remote_device_name, device_name, sizeof(s_remote_device_name) - 1U);
    s_remote_device_name[sizeof(s_remote_device_name) - 1U] = '\0';
    s_search_by_name = true;
}

void poom_ble_gatt_client_set_scan_params(const poom_ble_gatt_client_scan_params_t *scan_params)
{
    if (scan_params == NULL)
    {
        poom_ble_gatt_client_set_defaults_();
        return;
    }

    s_remote_filter_service_uuid = scan_params->remote_filter_service_uuid;
    s_remote_filter_char_uuid = scan_params->remote_filter_char_uuid;
    s_notify_descr_uuid = scan_params->notify_descr_uuid;
    s_ble_scan_params = scan_params->ble_scan_params;
    s_scan_params_set = true;
}

void poom_ble_gatt_client_set_callbacks(poom_ble_gatt_client_event_cb_t event_cb)
{
    s_event_cb = event_cb;
}

void poom_ble_gatt_client_send_data(uint8_t *data, int length)
{
    if ((data == NULL) || (length <= 0))
    {
        return;
    }

    if (!s_is_connected)
    {
        POOM_BLE_GATT_CLIENT_PRINTF_W("send ignored: not connected");
        return;
    }

    (void)esp_ble_gattc_write_char(
        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].gattc_if,
        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id,
        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].char_handle,
        length,
        data,
        ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
}

/**
 * @brief Profile-level GATTC event handler.
 */
static void poom_ble_gatt_client_profile_event_handler_(esp_gattc_cb_event_t event,
                                                  esp_gatt_if_t gattc_if,
                                                  esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p = param;

    if (p == NULL)
    {
        return;
    }

    switch (event)
    {
        case ESP_GATTC_REG_EVT:
        {
            esp_err_t ret = esp_ble_gap_set_scan_params(&s_ble_scan_params);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("set scan params failed: 0x%x", ret);
            }
        }
        break;

        case ESP_GATTC_CONNECT_EVT:
        {
            esp_err_t ret;

            memcpy(s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].remote_bda,
                   p->connect.remote_bda,
                   sizeof(esp_bd_addr_t));
            s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id = p->connect.conn_id;

            POOM_BLE_GATT_CLIENT_PRINTF_I("connected conn_id=%d if=%d", p->connect.conn_id, gattc_if);
            poom_ble_gatt_client_log_bda_(p->connect.remote_bda);

            ret = esp_ble_gattc_send_mtu_req(gattc_if, p->connect.conn_id);
            if (ret != ESP_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("mtu request failed: 0x%x", ret);
            }
        }
        break;

        case ESP_GATTC_OPEN_EVT:
            if (p->open.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("open failed status=%d", p->open.status);
            }
            else
            {
                POOM_BLE_GATT_CLIENT_PRINTF_I("open success");
            }
            break;

        case ESP_GATTC_DIS_SRVC_CMPL_EVT:
            if (p->dis_srvc_cmpl.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("discover service failed status=%d", p->dis_srvc_cmpl.status);
                break;
            }

            (void)esp_ble_gattc_search_service(gattc_if,
                                               p->dis_srvc_cmpl.conn_id,
                                               &s_remote_filter_service_uuid);
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            if (p->cfg_mtu.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("config mtu failed status=0x%x", p->cfg_mtu.status);
            }
            else
            {
                POOM_BLE_GATT_CLIENT_PRINTF_I("MTU=%d conn_id=%d", p->cfg_mtu.mtu, p->cfg_mtu.conn_id);
            }
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if (poom_ble_gatt_client_service_match_(&p->search_res.srvc_id))
            {
                s_server_attached = true;
                s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_start_handle = p->search_res.start_handle;
                s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_end_handle = p->search_res.end_handle;
                POOM_BLE_GATT_CLIENT_PRINTF_I("service attached start=%u end=%u",
                                       (unsigned)p->search_res.start_handle,
                                       (unsigned)p->search_res.end_handle);
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (p->search_cmpl.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("search complete failed status=0x%x", p->search_cmpl.status);
                break;
            }

            if (s_server_attached)
            {
                uint16_t count = 0U;
                esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                    gattc_if,
                    p->search_cmpl.conn_id,
                    ESP_GATT_DB_CHARACTERISTIC,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_start_handle,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_end_handle,
                    POOM_BLE_GATT_CLIENT_INVALID_HANDLE,
                    &count);

                if (status != ESP_GATT_OK)
                {
                    POOM_BLE_GATT_CLIENT_PRINTF_E("get char attr count failed");
                    break;
                }

                if (count > 0U)
                {
                    esp_gattc_char_elem_t *char_elem_result =
                        (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);

                    if (char_elem_result == NULL)
                    {
                        POOM_BLE_GATT_CLIENT_PRINTF_E("no memory for char element list");
                        break;
                    }

                    status = esp_ble_gattc_get_char_by_uuid(
                        gattc_if,
                        p->search_cmpl.conn_id,
                        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_start_handle,
                        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_end_handle,
                        s_remote_filter_char_uuid,
                        char_elem_result,
                        &count);

                    if (status != ESP_GATT_OK)
                    {
                        POOM_BLE_GATT_CLIENT_PRINTF_E("get char by uuid failed");
                        free(char_elem_result);
                        break;
                    }

                    if ((count > 0U) && ((char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0U))
                    {
                        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].char_handle = char_elem_result[0].char_handle;
                        (void)esp_ble_gattc_register_for_notify(
                            gattc_if,
                            s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].remote_bda,
                            char_elem_result[0].char_handle);
                    }

                    free(char_elem_result);
                }
            }
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (p->reg_for_notify.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("register notify failed status=%d", p->reg_for_notify.status);
            }
            else
            {
                uint16_t count = 0U;
                uint16_t notify_en = 1U;
                esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                    gattc_if,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id,
                    ESP_GATT_DB_DESCRIPTOR,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_start_handle,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_end_handle,
                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].char_handle,
                    &count);

                if (status != ESP_GATT_OK)
                {
                    POOM_BLE_GATT_CLIENT_PRINTF_E("get descriptor count failed");
                    break;
                }

                if (count > 0U)
                {
                    esp_gattc_descr_elem_t *descr_elem_result =
                        (esp_gattc_descr_elem_t *)malloc(sizeof(esp_gattc_descr_elem_t) * count);

                    if (descr_elem_result == NULL)
                    {
                        POOM_BLE_GATT_CLIENT_PRINTF_E("no memory for descriptor list");
                        break;
                    }

                    status = esp_ble_gattc_get_descr_by_char_handle(
                        gattc_if,
                        s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id,
                        p->reg_for_notify.handle,
                        s_notify_descr_uuid,
                        descr_elem_result,
                        &count);

                    if (status != ESP_GATT_OK)
                    {
                        POOM_BLE_GATT_CLIENT_PRINTF_E("get descriptor by char handle failed");
                        free(descr_elem_result);
                        break;
                    }

                    if ((count > 0U) &&
                        (descr_elem_result[0].uuid.len == ESP_UUID_LEN_16) &&
                        (descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG))
                    {
                        status = esp_ble_gattc_write_char_descr(
                            gattc_if,
                            s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id,
                            descr_elem_result[0].handle,
                            sizeof(notify_en),
                            (uint8_t *)&notify_en,
                            ESP_GATT_WRITE_TYPE_RSP,
                            ESP_GATT_AUTH_REQ_NONE);

                        if (status != ESP_GATT_OK)
                        {
                            POOM_BLE_GATT_CLIENT_PRINTF_E("write descriptor failed");
                        }
                    }

                    free(descr_elem_result);
                }
            }
            break;

        case ESP_GATTC_NOTIFY_EVT:
            POOM_BLE_GATT_CLIENT_PRINTF_I("notify len=%u", (unsigned)p->notify.value_len);
            poom_ble_gatt_client_log_hex_(p->notify.value, p->notify.value_len);
            poom_ble_gatt_client_log_ascii_(p->notify.value, p->notify.value_len);
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            if (p->write.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("write descriptor failed status=0x%x", p->write.status);
            }
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            if (p->write.status != ESP_GATT_OK)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("write char failed status=0x%x", p->write.status);
            }
            break;

        case ESP_GATTC_SRVC_CHG_EVT:
            poom_ble_gatt_client_log_bda_(p->srvc_chg.remote_bda);
            break;

        case ESP_GATTC_DISCONNECT_EVT:
            s_is_connected = false;
            s_server_attached = false;
            POOM_BLE_GATT_CLIENT_PRINTF_I("disconnect reason=%d", p->disconnect.reason);
            break;

        default:
            break;
    }

    if (s_event_cb.handler_client_cb != NULL)
    {
        s_event_cb.handler_client_cb(event, param);
    }
}

/**
 * @brief GAP callback for BLE scanning lifecycle.
 */
static void poom_ble_gatt_client_gap_event_handler_(esp_gap_ble_cb_event_t event,
                                              esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0U;

    if (param == NULL)
    {
        return;
    }

    switch (event)
    {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            (void)esp_ble_gap_start_scanning(POOM_BLE_GATT_CLIENT_SCAN_DURATION_SEC_DEFAULT);
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("scan start failed status=0x%x", param->scan_start_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
        {
            esp_ble_gap_cb_param_t *scan_result = param;

            switch (scan_result->scan_rst.search_evt)
            {
                case ESP_GAP_SEARCH_INQ_RES_EVT:
                    if (!s_search_by_name)
                    {
                        break;
                    }
                    adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                                        ESP_BLE_AD_TYPE_NAME_CMPL,
                                                        &adv_name_len);
                    if (adv_name != NULL)
                    {
                        size_t target_len = strlen(s_remote_device_name);
                        if ((target_len == adv_name_len) &&
                            (strncmp((char *)adv_name, s_remote_device_name, adv_name_len) == 0) &&
                            (scan_result->scan_rst.rssi > POOM_BLE_GATT_CLIENT_CONNECT_RSSI_THRESHOLD_DBM))
                        {
                            if (!s_is_connected)
                            {
                                s_is_connected = true;
                                POOM_BLE_GATT_CLIENT_PRINTF_I("connecting to %s (rssi=%d)",
                                                       s_remote_device_name,
                                                       scan_result->scan_rst.rssi);
                                (void)esp_ble_gap_stop_scanning();
                                (void)esp_ble_gattc_open(
                                    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].gattc_if,
                                    scan_result->scan_rst.bda,
                                    scan_result->scan_rst.ble_addr_type,
                                    true);
                            }
                        }
                    }
                    break;

                case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                    POOM_BLE_GATT_CLIENT_PRINTF_D("scan window complete, restarting");
                    if (!s_is_connected)
                    {
                        (void)esp_ble_gap_start_scanning(POOM_BLE_GATT_CLIENT_SCAN_DURATION_SEC_DEFAULT);
                    }
                    break;
                default:
                    break;
            }
        }
        break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("scan stop failed status=0x%x", param->scan_stop_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                POOM_BLE_GATT_CLIENT_PRINTF_E("adv stop failed status=0x%x", param->adv_stop_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            POOM_BLE_GATT_CLIENT_PRINTF_D("conn params status=%d min=%d max=%d int=%d lat=%d to=%d",
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
 * @brief Global GATTC dispatcher.
 */
static void poom_ble_gatt_client_gattc_event_handler_(esp_gattc_cb_event_t event,
                                                esp_gatt_if_t gattc_if,
                                                esp_ble_gattc_cb_param_t *param)
{
    if ((event == ESP_GATTC_REG_EVT) && (param != NULL))
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            s_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        }
        else
        {
            POOM_BLE_GATT_CLIENT_PRINTF_E("register app failed app_id=%04x status=%d",
                                   param->reg.app_id,
                                   param->reg.status);
            return;
        }
    }

    for (int i = 0; i < (int)POOM_BLE_GATT_CLIENT_PROFILE_COUNT; ++i)
    {
        if ((gattc_if == ESP_GATT_IF_NONE) || (gattc_if == s_profile_tab[i].gattc_if))
        {
            if (s_profile_tab[i].gattc_cb != NULL)
            {
                s_profile_tab[i].gattc_cb(event, gattc_if, param);
            }
        }
    }
}

void poom_ble_gatt_client_start(void)
{
    esp_err_t ret;
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;

    if (s_started)
    {
        return;
    }

    if (!s_scan_params_set)
    {
        poom_ble_gatt_client_set_defaults_();
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_CLIENT_PRINTF_W("controller mem release classic failed: 0x%x", ret);
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&cfg);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_CLIENT_PRINTF_E("controller init failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status != ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_CLIENT_PRINTF_E("controller enable failed: %s", esp_err_to_name(ret));
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
            POOM_BLE_GATT_CLIENT_PRINTF_E("bluedroid init failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status != ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK)
        {
            POOM_BLE_GATT_CLIENT_PRINTF_E("bluedroid enable failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_ble_gap_register_callback(poom_ble_gatt_client_gap_event_handler_);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_CLIENT_PRINTF_E("gap register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gattc_register_callback(poom_ble_gatt_client_gattc_event_handler_);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_CLIENT_PRINTF_E("gattc register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gattc_app_register(POOM_BLE_GATT_CLIENT_PROFILE_ID);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_CLIENT_PRINTF_E("gattc app register failed: 0x%x", ret);
        return;
    }

    ret = esp_ble_gatt_set_local_mtu(POOM_BLE_GATT_CLIENT_LOCAL_MTU);
    if (ret != ESP_OK)
    {
        POOM_BLE_GATT_CLIENT_PRINTF_E("set local mtu failed: 0x%x", ret);
    }

    s_started = true;
    POOM_BLE_GATT_CLIENT_PRINTF_I("started");
}

void poom_ble_gatt_client_stop(void)
{
    esp_err_t ret;
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;

    if (!s_started)
    {
        return;
    }

    s_is_connected = false;
    s_server_attached = false;

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_CLIENT_PRINTF_W("bluedroid disable failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_CLIENT_PRINTF_W("bluedroid deinit failed: 0x%x", ret);
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_disable();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_CLIENT_PRINTF_W("controller disable failed: 0x%x", ret);
        }
    }

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_deinit();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
        {
            POOM_BLE_GATT_CLIENT_PRINTF_W("controller deinit failed: 0x%x", ret);
        }
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        POOM_BLE_GATT_CLIENT_PRINTF_W("controller mem release ble failed: 0x%x", ret);
    }

    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].gattc_if = ESP_GATT_IF_NONE;
    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].conn_id = 0U;
    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_start_handle = 0U;
    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].service_end_handle = 0U;
    s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].char_handle = 0U;
    memset(s_profile_tab[POOM_BLE_GATT_CLIENT_PROFILE_ID].remote_bda, 0, sizeof(esp_bd_addr_t));

    s_started = false;
    POOM_BLE_GATT_CLIENT_PRINTF_I("stopped");
}
