// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_plot.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_system.h"
#include "nvs_flash.h"

/**
 * @file poom_ble_plot.c
 * @brief BLE plot transport over Nordic UART Service (NUS).
 */

/* =========================
 * Local log macros (printf)
 * ========================= */

#if BLEPLOT_LOG_ENABLED
static const char *BLEPLOT_LOG_TAG = BLEPLOT_TAG;

#define BLEPLOT_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", BLEPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define BLEPLOT_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", BLEPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define BLEPLOT_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", BLEPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

#if BLEPLOT_DEBUG_LOG_ENABLED
#define BLEPLOT_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", BLEPLOT_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define BLEPLOT_PRINTF_D(...) do { } while (0)
#endif
#else
#define BLEPLOT_PRINTF_E(...) do { } while (0)
#define BLEPLOT_PRINTF_W(...) do { } while (0)
#define BLEPLOT_PRINTF_I(...) do { } while (0)
#define BLEPLOT_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define BLEPLOT_ADV_CONFIG_FLAG               (1U << 0)
#define BLEPLOT_SCAN_RSP_CONFIG_FLAG          (1U << 1)

#define BLEPLOT_ADV_INT_MIN                   (0x20U)
#define BLEPLOT_ADV_INT_MAX                   (0x40U)
#define BLEPLOT_GAP_MIN_INTERVAL              (0x0006U)
#define BLEPLOT_GAP_MAX_INTERVAL              (0x0010U)
#define BLEPLOT_GAP_APPEARANCE                (0x00U)

#define BLEPLOT_ATT_OVERHEAD_BYTES            (3U)
#define BLEPLOT_MTU_PAYLOAD_MIN               (3U)

#define BLEPLOT_APP_ID                        (0x42U)
#define BLEPLOT_SVC_INST_ID                   (0U)
#define BLEPLOT_CONN_PARAM_LATENCY            (0U)
#define BLEPLOT_CONN_PARAM_MAX_INT            (0x10U)
#define BLEPLOT_CONN_PARAM_MIN_INT            (0x0BU)
#define BLEPLOT_CONN_PARAM_TIMEOUT            (400U)

#define BLEPLOT_DEFAULT_SERIES_COUNT          (1U)
#define BLEPLOT_DEFAULT_SEPARATOR             (',')
#define BLEPLOT_DEFAULT_PRECISION             (2U)
#define BLEPLOT_MAX_PRECISION                 (9U)
#define BLEPLOT_FORMAT_BUFFER_LEN             (8U)

#define BLEPLOT_RET_OK                        (0)
#define BLEPLOT_RET_ERR_INVALID_ARG           (-1)
#define BLEPLOT_RET_ERR_FORMAT                (-2)
#define BLEPLOT_RET_ERR_SEND                  (-3)
#define BLEPLOT_RET_ERR_LINE_TOO_LONG         (-4)
#define BLEPLOT_RET_ERR_BUFFER                (-5)

/* ==============================
 * NUS UUIDs (Bluefruit / Nordic)
 * ============================== */
static const uint8_t k_nus_uuid_service[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
}; /* 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */

static const uint8_t k_nus_uuid_rx[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
}; /* 6E400002-B5A3-F393-E0A9-E50E24DCCA9E */

static const uint8_t k_nus_uuid_tx[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
}; /* 6E400003-B5A3-F393-E0A9-E50E24DCCA9E */

/* =========================
 * GAP / ADV state
 * ========================= */
static uint8_t s_adv_config_done = 0U;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = BLEPLOT_ADV_INT_MIN,
    .adv_int_max = BLEPLOT_ADV_INT_MAX,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t s_device_name[32] = BLEPLOT_DEVICE_NAME;

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = true,
    .min_interval = BLEPLOT_GAP_MIN_INTERVAL,
    .max_interval = BLEPLOT_GAP_MAX_INTERVAL,
    .appearance = BLEPLOT_GAP_APPEARANCE,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(k_nus_uuid_service),
    .p_service_uuid = (uint8_t *)k_nus_uuid_service,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .min_interval = BLEPLOT_GAP_MIN_INTERVAL,
    .max_interval = BLEPLOT_GAP_MAX_INTERVAL,
    .appearance = BLEPLOT_GAP_APPEARANCE,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(k_nus_uuid_service),
    .p_service_uuid = (uint8_t *)k_nus_uuid_service,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* =========================
 * GATT state
 * ========================= */
static uint16_t s_mtu_usable = (BLEPLOT_LOCAL_MTU > BLEPLOT_ATT_OVERHEAD_BYTES)
                                   ? (uint16_t)(BLEPLOT_LOCAL_MTU - BLEPLOT_ATT_OVERHEAD_BYTES)
                                   : BLEPLOT_MTU_PAYLOAD_MIN;

enum
{
    IDX_SVC,
    IDX_TX_CHAR,
    IDX_TX_VAL,
    IDX_TX_CCC,
    IDX_RX_CHAR,
    IDX_RX_VAL,
    IDX_NB
};

static uint16_t s_handle_table[IDX_NB];
static bool s_connected = false;
static uint16_t s_conn_id = 0U;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static bool s_initialized = false;
static bool s_stopping = false;

/* Output format state */
static uint8_t s_series_count = BLEPLOT_DEFAULT_SERIES_COUNT;
static char s_sep = BLEPLOT_DEFAULT_SEPARATOR;
static uint8_t s_precision = BLEPLOT_DEFAULT_PRECISION;

/* Line buffer */
static char s_linebuf[BLEPLOT_MAX_LINE_LEN];

/* Base GATT declarations */
static const uint16_t k_uuid_primary_svc = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t k_uuid_char_decl = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t k_uuid_ccc = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t k_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t k_prop_write_nr =
    ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t k_ccc_init[2] = {0x00U, 0x00U};

/* GATT DB: SVC (NUS) + TX (notify) + RX (write) */
static const esp_gatts_attr_db_t gatt_db[IDX_NB] = {
    [IDX_SVC] = {{
                      ESP_GATT_AUTO_RSP,
                  },
                  {
                      ESP_UUID_LEN_16,
                      (uint8_t *)&k_uuid_primary_svc,
                      ESP_GATT_PERM_READ,
                      sizeof(k_nus_uuid_service),
                      sizeof(k_nus_uuid_service),
                      (uint8_t *)k_nus_uuid_service,
                  }},

    [IDX_TX_CHAR] = {{
                          ESP_GATT_AUTO_RSP,
                      },
                      {
                          ESP_UUID_LEN_16,
                          (uint8_t *)&k_uuid_char_decl,
                          ESP_GATT_PERM_READ,
                          sizeof(uint8_t),
                          sizeof(uint8_t),
                          (uint8_t *)&(const uint8_t){k_prop_notify},
                      }},

    [IDX_TX_VAL] = {{
                         ESP_GATT_AUTO_RSP,
                     },
                     {
                         ESP_UUID_LEN_128,
                         (uint8_t *)k_nus_uuid_tx,
                         ESP_GATT_PERM_READ,
                         BLEPLOT_MAX_LINE_LEN,
                         0,
                         NULL,
                     }},

    [IDX_TX_CCC] = {{
                         ESP_GATT_AUTO_RSP,
                     },
                     {
                         ESP_UUID_LEN_16,
                         (uint8_t *)&k_uuid_ccc,
                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                         sizeof(uint16_t),
                         sizeof(k_ccc_init),
                         (uint8_t *)k_ccc_init,
                     }},

    [IDX_RX_CHAR] = {{
                          ESP_GATT_AUTO_RSP,
                      },
                      {
                          ESP_UUID_LEN_16,
                          (uint8_t *)&k_uuid_char_decl,
                          ESP_GATT_PERM_READ,
                          sizeof(uint8_t),
                          sizeof(uint8_t),
                          (uint8_t *)&(const uint8_t){k_prop_write_nr},
                      }},

    [IDX_RX_VAL] = {{
                         ESP_GATT_AUTO_RSP,
                     },
                     {
                         ESP_UUID_LEN_128,
                         (uint8_t *)k_nus_uuid_rx,
                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                         BLEPLOT_MAX_LINE_LEN,
                         0,
                         NULL,
                     }},
};

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Returns current usable ATT payload for notifications.
 *
 * @return Payload length in bytes.
 */
static inline uint16_t poom_ble_plot_usable_payload_(void)
{
    uint16_t payload = s_mtu_usable;

    if (payload > BLEPLOT_MAX_LINE_LEN)
    {
        payload = BLEPLOT_MAX_LINE_LEN;
    }

    return payload;
}

/**
 * @brief Dumps RX payload bytes when debug logs are enabled.
 *
 * @param data Pointer to payload bytes.
 * @param len Payload length.
 */
static void poom_ble_plot_log_rx_payload_(const uint8_t *data, size_t len)
{
#if BLEPLOT_DEBUG_LOG_ENABLED
    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    for (size_t i = 0; i < len; ++i)
    {
        BLEPLOT_PRINTF_D("RX[%u]=0x%02X", (unsigned)i, (unsigned)data[i]);
    }
#else
    (void)data;
    (void)len;
#endif
}

/**
 * @brief Resets runtime state to defaults.
 */
static void poom_ble_plot_reset_state_(void)
{
    s_adv_config_done = 0U;
    s_mtu_usable = (BLEPLOT_LOCAL_MTU > BLEPLOT_ATT_OVERHEAD_BYTES)
                       ? (uint16_t)(BLEPLOT_LOCAL_MTU - BLEPLOT_ATT_OVERHEAD_BYTES)
                       : BLEPLOT_MTU_PAYLOAD_MIN;
    s_connected = false;
    s_conn_id = 0U;
    s_gatts_if = ESP_GATT_IF_NONE;
    s_series_count = BLEPLOT_DEFAULT_SERIES_COUNT;
    s_sep = BLEPLOT_DEFAULT_SEPARATOR;
    s_precision = BLEPLOT_DEFAULT_PRECISION;
    memset(s_handle_table, 0, sizeof(s_handle_table));
    memset(s_linebuf, 0, sizeof(s_linebuf));
    memset(s_device_name, 0, sizeof(s_device_name));
    (void)strncpy((char *)s_device_name, BLEPLOT_DEVICE_NAME, sizeof(s_device_name) - 1U);
    s_initialized = false;
    s_stopping = false;
}

/**
 * @brief GAP callback handler.
 *
 * @param event GAP event type.
 * @param param GAP event data.
 */
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~BLEPLOT_ADV_CONFIG_FLAG);
            if ((s_adv_config_done == 0U) && !s_stopping)
            {
                (void)esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~BLEPLOT_SCAN_RSP_CONFIG_FLAG);
            if ((s_adv_config_done == 0U) && !s_stopping)
            {
                (void)esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                BLEPLOT_PRINTF_E("adv start failed");
            }
            else
            {
                BLEPLOT_PRINTF_I("adv started");
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            BLEPLOT_PRINTF_I("conn params: ci=%d lat=%d to=%d",
                             param->update_conn_params.conn_int,
                             param->update_conn_params.latency,
                             param->update_conn_params.timeout);
            break;

        default:
            break;
    }
}

/**
 * @brief GATTS profile callback.
 *
 * @param event GATTS event type.
 * @param gatts_if GATTS interface.
 * @param param GATTS event data.
 */
static void gatts_profile_cb(esp_gatts_cb_event_t event,
                             esp_gatt_if_t gatts_if,
                             esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GATTS_REG_EVT:
        {
            esp_err_t ret;

            ret = esp_ble_gap_set_device_name((const char *)s_device_name);
            if (ret != ESP_OK)
            {
                BLEPLOT_PRINTF_E("set name err=0x%x", ret);
            }

            ret = esp_ble_gap_config_adv_data(&s_adv_data);
            if (ret != ESP_OK)
            {
                BLEPLOT_PRINTF_E("adv data err=0x%x", ret);
            }
            s_adv_config_done |= BLEPLOT_ADV_CONFIG_FLAG;

            ret = esp_ble_gap_config_adv_data(&s_scan_rsp_data);
            if (ret != ESP_OK)
            {
                BLEPLOT_PRINTF_E("scan rsp data err=0x%x", ret);
            }
            s_adv_config_done |= BLEPLOT_SCAN_RSP_CONFIG_FLAG;

            ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, IDX_NB, BLEPLOT_SVC_INST_ID);
            if (ret != ESP_OK)
            {
                BLEPLOT_PRINTF_E("create attr tab err=0x%x", ret);
            }
        }
        break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK)
            {
                BLEPLOT_PRINTF_E("attr table status=0x%x", param->add_attr_tab.status);
                break;
            }

            if (param->add_attr_tab.num_handle != IDX_NB)
            {
                BLEPLOT_PRINTF_E("attr handle count %d != %d",
                                 param->add_attr_tab.num_handle,
                                 IDX_NB);
                break;
            }

            memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
            (void)esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
            break;

        case ESP_GATTS_START_EVT:
            BLEPLOT_PRINTF_I("service started");
            break;

        case ESP_GATTS_CONNECT_EVT:
        {
            esp_ble_conn_update_params_t params = {0};

            s_connected = true;
            s_conn_id = param->connect.conn_id;
            s_gatts_if = gatts_if;

            memcpy(params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            params.latency = BLEPLOT_CONN_PARAM_LATENCY;
            params.max_int = BLEPLOT_CONN_PARAM_MAX_INT;
            params.min_int = BLEPLOT_CONN_PARAM_MIN_INT;
            params.timeout = BLEPLOT_CONN_PARAM_TIMEOUT;

            (void)esp_ble_gap_update_conn_params(&params);
            BLEPLOT_PRINTF_I("connected");
        }
        break;

        case ESP_GATTS_DISCONNECT_EVT:
            s_connected = false;
            BLEPLOT_PRINTF_I("disconnected");
            if (!s_stopping)
            {
                (void)esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GATTS_MTU_EVT:
        {
            uint16_t mtu = param->mtu.mtu;
            s_mtu_usable = (mtu > BLEPLOT_ATT_OVERHEAD_BYTES)
                               ? (uint16_t)(mtu - BLEPLOT_ATT_OVERHEAD_BYTES)
                               : BLEPLOT_MTU_PAYLOAD_MIN;
            BLEPLOT_PRINTF_I("MTU updated: %u (usable %u)",
                             (unsigned)mtu,
                             (unsigned)poom_ble_plot_usable_payload_());
        }
        break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.handle == s_handle_table[IDX_RX_VAL])
            {
                poom_ble_plot_log_rx_payload_(param->write.value, param->write.len);
            }

            if (param->write.need_rsp)
            {
                (void)esp_ble_gatts_send_response(gatts_if,
                                                  param->write.conn_id,
                                                  param->write.trans_id,
                                                  ESP_GATT_OK,
                                                  NULL);
            }
            break;

        case ESP_GATTS_CONF_EVT:
            /* no-op: notifications do not require app-level confirmation */
            break;

        default:
            break;
    }
}

/**
 * @brief Dispatches GATTS events to the single profile handler.
 *
 * @param event GATTS event type.
 * @param gatts_if GATTS interface.
 * @param param GATTS event data.
 */
static void gatts_dispatcher(esp_gatts_cb_event_t event,
                             esp_gatt_if_t gatts_if,
                             esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT)
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            s_gatts_if = gatts_if;
        }
        else
        {
            BLEPLOT_PRINTF_E("reg app failed: %d", param->reg.status);
            return;
        }
    }

    gatts_profile_cb(event, gatts_if, param);
}

/**
 * @brief Sends one ASCII line by BLE notification.
 *
 * @param line Pointer to line buffer.
 * @param len Line length.
 * @return 0 on success, negative value on error.
 */
static int32_t poom_ble_plot_send_ascii_line_(const char *line, size_t len)
{
    if (!s_connected)
    {
        return BLEPLOT_RET_ERR_INVALID_ARG;
    }

    if (len > poom_ble_plot_usable_payload_())
    {
        return BLEPLOT_RET_ERR_FORMAT;
    }

    return (esp_ble_gatts_send_indicate(s_gatts_if,
                                        s_conn_id,
                                        s_handle_table[IDX_TX_VAL],
                                        (uint16_t)len,
                                        (uint8_t *)line,
                                        false) == ESP_OK)
               ? BLEPLOT_RET_OK
               : BLEPLOT_RET_ERR_SEND;
}

/* =========================
 * Public API
 * ========================= */
/**
 * @brief Initializes BLE GAP/GATT stack and NUS service.
 *
 * @param device_name Advertised BLE device name.
 * @return 0 on success.
 */
int32_t poom_ble_plot_init(const char *device_name)
{
    esp_err_t ret;

    if (s_initialized)
    {
        BLEPLOT_PRINTF_W("already initialized");
        return BLEPLOT_RET_OK;
    }

    s_stopping = false;

    if ((device_name != NULL) && (device_name[0] != '\0'))
    {
        size_t name_len = strnlen(device_name, sizeof(s_device_name) - 1U);
        memcpy(s_device_name, device_name, name_len);
        s_device_name[name_len] = '\0';
    }

    ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        BLEPLOT_PRINTF_W("controller mem release (classic bt) failed: 0x%x", ret);
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_dispatcher));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(BLEPLOT_APP_ID));

    ret = esp_ble_gatt_set_local_mtu(BLEPLOT_LOCAL_MTU);
    if (ret != ESP_OK)
    {
        BLEPLOT_PRINTF_W("set local MTU failed: 0x%x", ret);
    }

    s_initialized = true;
    return BLEPLOT_RET_OK;
}

/**
 * @brief Sets the number of series to emit per line.
 *
 * @param n_series Number of series.
 * @return 0 on success, negative value on error.
 */
int32_t poom_ble_plot_set_series_count(uint8_t n_series)
{
    if (n_series == 0U)
    {
        return BLEPLOT_RET_ERR_INVALID_ARG;
    }

    s_series_count = n_series;
    return BLEPLOT_RET_OK;
}

/**
 * @brief Configures output separator and precision.
 *
 * @param sep Value separator character.
 * @param precision Decimal precision.
 */
void poom_ble_plot_set_format(char sep, uint8_t precision)
{
    s_sep = sep;
    if (precision > BLEPLOT_MAX_PRECISION)
    {
        precision = BLEPLOT_MAX_PRECISION;
    }

    s_precision = precision;
}

/**
 * @brief Returns BLE connection state.
 *
 * @return true if connected.
 */
bool poom_ble_plot_is_connected(void)
{
    return s_connected;
}

/**
 * @brief Stops BLE plot transport and releases BLE resources.
 */
void poom_ble_plot_stop(void)
{
    esp_err_t ret;
    esp_bt_controller_status_t bt_status;
    esp_bluedroid_status_t bluedroid_status;

    if (!s_initialized)
    {
        return;
    }

    s_stopping = true;

    if (s_connected && (s_gatts_if != ESP_GATT_IF_NONE))
    {
        ret = esp_ble_gatts_close(s_gatts_if, s_conn_id);
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("close connection failed: 0x%x", ret);
        }
    }

    ret = esp_ble_gap_stop_advertising();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        BLEPLOT_PRINTF_W("stop advertising failed: 0x%x", ret);
    }

    if (s_gatts_if != ESP_GATT_IF_NONE)
    {
        ret = esp_ble_gatts_app_unregister(s_gatts_if);
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("app unregister failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED)
    {
        ret = esp_bluedroid_disable();
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("bluedroid disable failed: 0x%x", ret);
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_deinit();
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("bluedroid deinit failed: 0x%x", ret);
        }
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ret = esp_bt_controller_disable();
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("controller disable failed: 0x%x", ret);
        }
    }

    bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_deinit();
        if (ret != ESP_OK)
        {
            BLEPLOT_PRINTF_W("controller deinit failed: 0x%x", ret);
        }
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        BLEPLOT_PRINTF_W("controller mem release failed: 0x%x", ret);
    }

    poom_ble_plot_reset_state_();
    BLEPLOT_PRINTF_I("stopped");
}

/**
 * @brief Formats and sends one CSV line to BLE plot client.
 *
 * @param values Input values array.
 * @param n Number of values in array.
 * @return 0 on success, negative value on error.
 */
int32_t poom_ble_plot_send_line(const float *values, size_t n)
{
    char fmt[BLEPLOT_FORMAT_BUFFER_LEN];
    size_t cols;
    size_t pos = 0U;

    if ((values == NULL) || (n == 0U))
    {
        return BLEPLOT_RET_ERR_INVALID_ARG;
    }

    cols = (n < s_series_count) ? n : s_series_count;

    (void)snprintf(fmt, sizeof(fmt), "%%.%uf", (unsigned)s_precision);

    for (size_t i = 0; i < cols; ++i)
    {
        int wrote;

        if ((pos + 2U) >= sizeof(s_linebuf))
        {
            break;
        }

        if (i > 0U)
        {
            s_linebuf[pos++] = s_sep;
        }

        wrote = snprintf(&s_linebuf[pos], sizeof(s_linebuf) - pos, fmt, (double)values[i]);
        if (wrote <= 0)
        {
            return BLEPLOT_RET_ERR_FORMAT;
        }

        pos += (size_t)wrote;

        if ((pos + 1U) > poom_ble_plot_usable_payload_())
        {
            return BLEPLOT_RET_ERR_LINE_TOO_LONG;
        }
    }

    if ((pos + 1U) > sizeof(s_linebuf))
    {
        return BLEPLOT_RET_ERR_BUFFER;
    }

    s_linebuf[pos++] = '\n';

    return poom_ble_plot_send_ascii_line_(s_linebuf, pos);
}
