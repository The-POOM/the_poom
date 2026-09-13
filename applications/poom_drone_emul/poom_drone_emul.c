// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_drone_emul.h"

#include <math.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "odid_wifi.h"
#include "opendroneid.h"

#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"

#define POOM_DRONE_EMUL_TAG "poom_drone_emul"

#define POOM_DRONE_EMUL_DEFAULT_CHANNEL (6U)
#define POOM_DRONE_EMUL_DEFAULT_COUNT (1U)
#define POOM_DRONE_EMUL_MAX_COUNT (16U)

/* Default location: San Francisco, USA. */
#define POOM_DRONE_EMUL_DEFAULT_LAT_D (37.7749)
#define POOM_DRONE_EMUL_DEFAULT_LON_D (-122.4194)

#define POOM_DRONE_EMUL_TX_INTERVAL_MS (200U) /* 5 Hz */
#define POOM_DRONE_EMUL_BEACON_BUF_MAX (512U)
#define POOM_DRONE_EMUL_TX_RETRY_COUNT (4U)
#define POOM_DRONE_EMUL_TX_BACKOFF_MS (2U)

/* Wi-Fi IEs (minimal set needed for broad compatibility with scanners/apps). */
#define POOM_WIFI_ELEMID_SSID (0x00U)
#define POOM_WIFI_ELEMID_RATES (0x01U)
#define POOM_WIFI_ELEMID_DS_PARAMS (0x03U)
#define POOM_WIFI_ELEMID_TIM (0x05U)
#define POOM_WIFI_ELEMID_COUNTRY (0x07U)
#define POOM_WIFI_ELEMID_EXT_RATES (0x32U)
#define POOM_WIFI_ELEMID_VENDOR (0xDDU)

/* NVS keys (must be < 16 bytes for poom_secrets_store). */
#define POOM_DRONE_EMUL_KEY_LOC "dremul_loc"
#define POOM_DRONE_EMUL_KEY_COUNT "dremul_cnt"
#define POOM_DRONE_EMUL_KEY_BLE "dremul_ble"

typedef struct
{
    int32_t lat_e7;
    int32_t lon_e7;
} poom_drone_emul_loc_e7_t;

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_channel = POOM_DRONE_EMUL_DEFAULT_CHANNEL;
static char s_ssid[33] = "POOM DRONE";
static int32_t s_lat_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LAT_D * 1e7);
static int32_t s_lon_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LON_D * 1e7);
static uint8_t s_count = POOM_DRONE_EMUL_DEFAULT_COUNT;
static bool s_ble_enabled = false;

static uint8_t s_ap_mac[6] = {0};
static uint8_t s_send_counter = 0;

static bool s_ble_active = false;
static bool s_ble_adv_config_pending = false;
static uint8_t s_ble_msg_counter[16] = {0};
static uint32_t s_tx_error_count = 0U;
static esp_ble_adv_params_t s_ble_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x20,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/**
 * @brief Internal helper for `tx_warn_if_failed`.
 *
 * @param[in] label Parameter passed to the function.
 * @param[in] err Parameter passed to the function.
 * @return void
 */
static void tx_warn_if_failed_(const char *label, esp_err_t err)
{
    if (err == ESP_OK)
    {
        return;
    }

    s_tx_error_count++;
    if (s_tx_error_count <= 8U)
    {
        printf("[W] [%s] %s failed: %s\n",
               POOM_DRONE_EMUL_TAG,
               (label != NULL) ? label : "wifi tx",
               esp_err_to_name(err));
    }
}

/**
 * @brief Internal helper for `tx_backoff_ticks`.
 *
 * @return TickType_t
 */
static TickType_t tx_backoff_ticks_(void)
{
    TickType_t ticks = pdMS_TO_TICKS(POOM_DRONE_EMUL_TX_BACKOFF_MS);
    if (ticks == 0)
    {
        ticks = 1;
    }
    return ticks;
}

/**
 * @brief Internal helper for `raw_tx_with_retry`.
 *
 * @param[in] label Parameter passed to the function.
 * @param[in] frame Parameter passed to the function.
 * @param[in] frame_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t raw_tx_with_retry_(const char *label, const uint8_t *frame, size_t frame_len)
{
    if ((frame == NULL) || (frame_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    const TickType_t backoff_ticks = tx_backoff_ticks_();

    for (uint32_t attempt = 0; attempt < POOM_DRONE_EMUL_TX_RETRY_COUNT; attempt++)
    {
        err = esp_wifi_80211_tx(WIFI_IF_AP, frame, (int)frame_len, true);
        if (err == ESP_OK)
        {
            vTaskDelay(backoff_ticks);
            return ESP_OK;
        }

        if (err != ESP_ERR_NO_MEM)
        {
            break;
        }

        vTaskDelay(backoff_ticks);
    }

    tx_warn_if_failed_(label, err);
    return err;
}

/**
 * @brief Internal helper for `loc_e7_is_valid`.
 *
 * @param[in] lat_e7 Parameter passed to the function.
 * @param[in] lon_e7 Parameter passed to the function.
 * @return bool
 */
static bool loc_e7_is_valid_(int32_t lat_e7, int32_t lon_e7)
{
    if ((lat_e7 < (int32_t)-900000000) || (lat_e7 > (int32_t)900000000))
    {
        return false;
    }
    if ((lon_e7 < (int32_t)-1800000000) || (lon_e7 > (int32_t)1800000000))
    {
        return false;
    }
    return true;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] out_lat_e7 Parameter passed to the function.
 * @param[in] out_lon_e7 Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t load_location_from_nvs_(int32_t *out_lat_e7, int32_t *out_lon_e7)
{
    if ((out_lat_e7 == NULL) || (out_lon_e7 == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_lat_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LAT_D * 1e7);
    *out_lon_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LON_D * 1e7);

    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }

    poom_drone_emul_loc_e7_t loc = {0};
    size_t len = sizeof(loc);
    err = poom_secrets_get_blob(POOM_DRONE_EMUL_KEY_LOC, &loc, &len);
    if (err != ESP_OK)
    {
        return err;
    }
    if (len != sizeof(loc))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!loc_e7_is_valid_(loc.lat_e7, loc.lon_e7))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_lat_e7 = loc.lat_e7;
    *out_lon_e7 = loc.lon_e7;
    return ESP_OK;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] lat_e7 Parameter passed to the function.
 * @param[in] lon_e7 Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t save_location_to_nvs_(int32_t lat_e7, int32_t lon_e7)
{
    if (!loc_e7_is_valid_(lat_e7, lon_e7))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }

    const poom_drone_emul_loc_e7_t loc = {.lat_e7 = lat_e7, .lon_e7 = lon_e7};
    return poom_secrets_set_blob(POOM_DRONE_EMUL_KEY_LOC, &loc, sizeof(loc));
}

/**
 * @brief Internal helper for `count_is_valid`.
 *
 * @param[in] count Parameter passed to the function.
 * @return bool
 */
static bool count_is_valid_(uint32_t count)
{
    return (count >= 1U) && (count <= (uint32_t)POOM_DRONE_EMUL_MAX_COUNT);
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] out_count Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t load_count_from_nvs_(uint8_t *out_count)
{
    if (out_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = POOM_DRONE_EMUL_DEFAULT_COUNT;

    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }

    uint32_t v = 0;
    err = poom_secrets_get_u32(POOM_DRONE_EMUL_KEY_COUNT, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    if (!count_is_valid_(v))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = (uint8_t)v;
    return ESP_OK;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] count Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t save_count_to_nvs_(uint8_t count)
{
    if (!count_is_valid_(count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }

    return poom_secrets_set_u32(POOM_DRONE_EMUL_KEY_COUNT, (uint32_t)count);
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] out_enabled Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t load_ble_enabled_from_nvs_(bool *out_enabled)
{
    if (out_enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_enabled = false;

    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }

    uint32_t v = 0;
    err = poom_secrets_get_u32(POOM_DRONE_EMUL_KEY_BLE, &v);
    if (err != ESP_OK)
    {
        return err;
    }
    *out_enabled = (v != 0U);
    return ESP_OK;
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] enabled Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t save_ble_enabled_to_nvs_(bool enabled)
{
    esp_err_t err = poom_secrets_init();
    if (err != ESP_OK)
    {
        return err;
    }
    return poom_secrets_set_u32(POOM_DRONE_EMUL_KEY_BLE, enabled ? 1U : 0U);
}

/**
 * @brief Internal helper for `odid_fill_static`.
 *
 * @param[in] uas Parameter passed to the function.
 * @return void
 */
static void odid_fill_static_(ODID_UAS_Data *uas)
{
    if (uas == NULL)
    {
        return;
    }

    odid_initUasData(uas);

    uas->BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    uas->BasicID[0].UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    (void)snprintf(uas->BasicID[0].UASID, sizeof(uas->BasicID[0].UASID), "POOM_DRONE-0001");

    uas->OperatorID.OperatorIdType = ODID_OPERATOR_ID;
    (void)snprintf(uas->OperatorID.OperatorId, sizeof(uas->OperatorID.OperatorId), "USA-POOM-OP");

    uas->SelfID.DescType = ODID_DESC_TYPE_TEXT;
    (void)snprintf(uas->SelfID.Desc, sizeof(uas->SelfID.Desc), "MFG:POOM MOD:DRONE");

    uas->System.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    uas->System.ClassificationType = ODID_CLASSIFICATION_TYPE_UNDECLARED;
    uas->System.AreaCount = 1;
    uas->System.AreaRadius = 50;
    uas->System.AreaCeiling = -1000.0f;
    uas->System.AreaFloor = -1000.0f;
    uas->System.OperatorAltitudeGeo = -1000.0f;
}

/**
 * @brief Internal helper for `odid_fill_static_index`.
 *
 * @param[in] uas Parameter passed to the function.
 * @param[in] index_1based Parameter passed to the function.
 * @return void
 */
static void odid_fill_static_index_(ODID_UAS_Data *uas, uint8_t index_1based)
{
    if (uas == NULL)
    {
        return;
    }

    odid_fill_static_(uas);
    (void)snprintf(uas->BasicID[0].UASID, sizeof(uas->BasicID[0].UASID), "POOM_DRONE-%04u", (unsigned)index_1based);
}

/**
 * @brief Internal helper for `odid_fill_location`.
 *
 * @param[in] uas Parameter passed to the function.
 * @param[in] lat_e7 Parameter passed to the function.
 * @param[in] lon_e7 Parameter passed to the function.
 * @return void
 */
static void odid_fill_location_(ODID_UAS_Data *uas, int32_t lat_e7, int32_t lon_e7)
{
    if (uas == NULL)
    {
        return;
    }

    uas->Location.Status = ODID_STATUS_UNDECLARED;
    uas->Location.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
    uas->Location.HorizAccuracy = ODID_HOR_ACC_10_METER;
    uas->Location.VertAccuracy = ODID_VER_ACC_10_METER;
    uas->Location.BaroAccuracy = ODID_VER_ACC_10_METER;
    uas->Location.SpeedAccuracy = ODID_SPEED_ACC_10_METERS_PER_SECOND;

    uas->Location.Latitude = ((double)lat_e7) / 1e7;
    uas->Location.Longitude = ((double)lon_e7) / 1e7;
    uas->Location.AltitudeGeo = 10.0;
    uas->Location.AltitudeBaro = 10.0;
    uas->Location.Height = 0.0;
    uas->Location.SpeedHorizontal = 0.0;
    uas->Location.SpeedVertical = 0.0;
    uas->Location.Direction = 0.0;
    uas->Location.TimeStamp = 0;

    uas->System.OperatorLatitude = uas->Location.Latitude;
    uas->System.OperatorLongitude = uas->Location.Longitude;
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] event Parameter passed to the function.
 * @param[in] param Parameter passed to the function.
 * @return void
 */
static void ble_gap_cb_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    (void)param;

    if (!s_ble_active)
    {
        return;
    }

    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (s_ble_adv_config_pending)
            {
                s_ble_adv_config_pending = false;
                (void)esp_ble_gap_start_advertising(&s_ble_adv_params);
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Internal helper for `ble_fill_random_static_addr`.
 *
 * @param[in] out_addr Parameter passed to the function.
 * @param[in] mix Parameter passed to the function.
 * @return void
 */
static void ble_fill_random_static_addr_(esp_bd_addr_t out_addr, uint8_t mix)
{
    if (out_addr == NULL)
    {
        return;
    }

    for (size_t i = 0; i < sizeof(esp_bd_addr_t); i += 4)
    {
        uint32_t r = esp_random();
        size_t chunk = sizeof(esp_bd_addr_t) - i;
        if (chunk > 4U)
        {
            chunk = 4U;
        }
        memcpy(&out_addr[i], &r, chunk);
    }

    out_addr[0] = (uint8_t)((out_addr[0] & 0x3FU) | 0xC0U);
    out_addr[5] ^= (uint8_t)(mix * 31U);
    out_addr[4] ^= (uint8_t)(mix * 47U);
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t ble_start_(void)
{
    if (s_ble_active)
    {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_cb_));

    s_ble_adv_config_pending = false;
    memset(s_ble_msg_counter, 0, sizeof(s_ble_msg_counter));
    s_ble_active = true;

    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void ble_stop_(void)
{
    if (!s_ble_active)
    {
        return;
    }

    (void)esp_ble_gap_stop_advertising();
    (void)esp_bluedroid_disable();
    (void)esp_bluedroid_deinit();
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();

    s_ble_active = false;
    s_ble_adv_config_pending = false;
}

/**
 * @brief Internal helper for `ble_tx_odid_msg`.
 *
 * @param[in] odid_msg Parameter passed to the function.
 * @param[in] odid_len Parameter passed to the function.
 * @param[in] addr_mix Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ble_tx_odid_msg_(const uint8_t *odid_msg, size_t odid_len, uint8_t addr_mix)
{
    if ((odid_msg == NULL) || (odid_len != ODID_MESSAGE_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ble_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_bd_addr_t addr;
    ble_fill_random_static_addr_(addr, addr_mix);
    (void)esp_ble_gap_set_rand_addr(addr);

    uint8_t adv_raw[31] = {0};
    size_t j = 0;

    adv_raw[j++] = 0x1e; /* length (30 bytes following) */
    adv_raw[j++] = 0x16; /* Service Data - 16-bit UUID */
    adv_raw[j++] = 0xfa; /* 0xFFFA (little-endian) */
    adv_raw[j++] = 0xff;
    adv_raw[j++] = 0x0d; /* ASTM app code */

    const uint8_t msg_type = (uint8_t)(odid_msg[0] >> 4);
    adv_raw[j++] = ++s_ble_msg_counter[msg_type & 0x0f];

    memcpy(&adv_raw[j], odid_msg, odid_len);
    j += odid_len;

    s_ble_adv_config_pending = true;
    (void)esp_ble_gap_stop_advertising();
    return esp_ble_gap_config_adv_data_raw(adv_raw, (uint32_t)j);
}

/**
 * @brief Internal helper for `ensure_ap_for_raw_tx`.
 *
 * @param[in] channel Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t ensure_ap_for_raw_tx_(uint8_t channel)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "POOM DRONE",
            .ssid_len = 0,
            .password = "",
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .max_connection = 4,
            .beacon_interval = 100,
        },
    };

    if ((channel < 1U) || (channel > 13U))
    {
        channel = POOM_DRONE_EMUL_DEFAULT_CHANNEL;
    }
    ap_config.ap.channel = channel;

    esp_err_t err = poom_wifi_ctrl_ap_start(&ap_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_ARG))
    {
        printf("[W] [%s] esp_wifi_set_band_mode failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
    }

    err = esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_SUPPORTED))
    {
        printf("[W] [%s] esp_wifi_set_protocol failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
    }

    err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_SUPPORTED))
    {
        printf("[W] [%s] esp_wifi_set_bandwidth failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
    }

    err = esp_wifi_set_max_tx_power(78);
    if (err != ESP_OK)
    {
        printf("[W] [%s] esp_wifi_set_max_tx_power failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
    {
        printf("[W] [%s] esp_wifi_set_ps failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
    }

    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        printf("[W] [%s] esp_wifi_set_channel failed: %s\n", POOM_DRONE_EMUL_TAG, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `odid_wifi_build_message_pack_beacon_frame_compat`.
 *
 * @param[in] uas Parameter passed to the function.
 * @param[in] mac Parameter passed to the function.
 * @param[in] ssid Parameter passed to the function.
 * @param[in] ssid_len Parameter passed to the function.
 * @param[in] channel Parameter passed to the function.
 * @param[in] interval_tu Parameter passed to the function.
 * @param[in] send_counter Parameter passed to the function.
 * @param[in] buf Parameter passed to the function.
 * @param[in] buf_size Parameter passed to the function.
 * @return int
 */
static int odid_wifi_build_message_pack_beacon_frame_compat_(ODID_UAS_Data *uas,
                                                            const uint8_t mac[6],
                                                            const char *ssid,
                                                            size_t ssid_len,
                                                            uint8_t channel,
                                                            uint16_t interval_tu,
                                                            uint8_t send_counter,
                                                            uint8_t *buf,
                                                            size_t buf_size)
{
    if ((uas == NULL) || (mac == NULL) || (buf == NULL) || (buf_size == 0U))
    {
        return -EINVAL;
    }
    if ((ssid == NULL) || (ssid_len == 0U) || (ssid_len > 32U))
    {
        return -EINVAL;
    }
    if ((channel < 1U) || (channel > 14U))
    {
        return -EINVAL;
    }

    size_t len = 0U;

    if (buf_size < (24U + 12U))
    {
        return -ENOMEM;
    }

    buf[len++] = 0x80U;
    buf[len++] = 0x00U;
    buf[len++] = 0x00U;
    buf[len++] = 0x00U;
    memset(&buf[len], 0xff, 6U);
    len += 6U;
    memcpy(&buf[len], mac, 6U);
    len += 6U;
    memcpy(&buf[len], mac, 6U);
    len += 6U;
    buf[len++] = 0x00U; /* seq ctrl (sys fills if enabled) */
    buf[len++] = 0x00U;

    const uint64_t ts_us = (uint64_t)esp_timer_get_time();
    for (size_t i = 0; i < 8U; i++)
    {
        buf[len++] = (uint8_t)(ts_us >> (8U * i));
    }

    buf[len++] = (uint8_t)(interval_tu & 0xffU);
    buf[len++] = (uint8_t)((interval_tu >> 8) & 0xffU);
    const uint16_t cap = 0x0421U;
    buf[len++] = (uint8_t)(cap & 0xffU);
    buf[len++] = (uint8_t)((cap >> 8) & 0xffU);

    if ((len + 2U + ssid_len) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_SSID;
    buf[len++] = (uint8_t)ssid_len;
    memcpy(&buf[len], ssid, ssid_len);
    len += ssid_len;

    static const uint8_t rates[] = {0x8bU, 0x96U, 0x82U, 0x84U, 0x0cU, 0x18U, 0x30U, 0x60U};
    if ((len + 2U + sizeof(rates)) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_RATES;
    buf[len++] = (uint8_t)sizeof(rates);
    memcpy(&buf[len], rates, sizeof(rates));
    len += sizeof(rates);

    if ((len + 3U) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_DS_PARAMS;
    buf[len++] = 0x01U;
    buf[len++] = channel;

    if ((len + 2U + 4U) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_TIM;
    buf[len++] = 0x04U;
    buf[len++] = 0x00U;
    buf[len++] = 0x02U;
    buf[len++] = 0x00U;
    buf[len++] = 0x00U;

    if ((len + 2U + 6U) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_COUNTRY;
    buf[len++] = 0x06U;
    buf[len++] = 'U';
    buf[len++] = 'S';
    buf[len++] = 0x20U;
    buf[len++] = 0x01U;
    buf[len++] = 11U;   /* common 2.4 GHz channels */
    buf[len++] = 0x14U; /* max TX power (dBm), heuristic */

    static const uint8_t ext_rates[] = {0x6cU, 0x12U, 0x24U, 0x48U};
    if ((len + 2U + sizeof(ext_rates)) > buf_size)
    {
        return -ENOMEM;
    }
    buf[len++] = POOM_WIFI_ELEMID_EXT_RATES;
    buf[len++] = (uint8_t)sizeof(ext_rates);
    memcpy(&buf[len], ext_rates, sizeof(ext_rates));
    len += sizeof(ext_rates);

    if ((len + 2U + 3U + 1U + 1U) > buf_size)
    {
        return -ENOMEM;
    }

    const size_t vendor_len_pos = len + 1U;
    buf[len++] = POOM_WIFI_ELEMID_VENDOR;
    buf[len++] = 0x00U; /* placeholder */
    buf[len++] = 0xfaU;
    buf[len++] = 0x0bU;
    buf[len++] = 0xbcU;
    buf[len++] = 0x0dU; /* ASTM app code */

    buf[len++] = send_counter; /* message_counter */

    const int pack_len = odid_message_build_pack(uas, &buf[len], buf_size - len);
    if (pack_len < 0)
    {
        return pack_len;
    }
    len += (size_t)pack_len;

    const size_t vendor_payload_len = (3U + 1U + 1U + (size_t)pack_len);
    if (vendor_payload_len > 255U)
    {
        return -EMSGSIZE;
    }
    buf[vendor_len_pos] = (uint8_t)vendor_payload_len;

    return (int)len;
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void tx_task_(void *arg)
{
    (void)arg;

    ODID_UAS_Data uas;
    uint8_t buf[POOM_DRONE_EMUL_BEACON_BUF_MAX];
    uint32_t last_nan_sync_ms = 0U;

    memset(&uas, 0, sizeof(uas));
    odid_fill_static_index_(&uas, 1U);

    while (s_running)
    {
        int32_t lat_e7;
        int32_t lon_e7;
        uint8_t channel;
        char ssid_local[33];
        uint8_t count;
        bool ble_enabled;

        portENTER_CRITICAL(&s_cfg_mux);
        lat_e7 = s_lat_e7;
        lon_e7 = s_lon_e7;
        channel = s_channel;
        count = s_count;
        ble_enabled = s_ble_enabled;
        (void)snprintf(ssid_local, sizeof(ssid_local), "%s", s_ssid);
        portEXIT_CRITICAL(&s_cfg_mux);

        (void)esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((uint32_t)(now_ms - last_nan_sync_ms) >= 1000U)
        {
            last_nan_sync_ms = now_ms;
            const int nan_sync_len =
                odid_wifi_build_nan_sync_beacon_frame((char *)s_ap_mac, buf, sizeof(buf));
            if (nan_sync_len > 0)
            {
                (void)raw_tx_with_retry_("nan_sync_tx", buf, (size_t)nan_sync_len);
            }
        }

        if (count < 1U)
        {
            count = POOM_DRONE_EMUL_DEFAULT_COUNT;
        }
        if (count > POOM_DRONE_EMUL_MAX_COUNT)
        {
            count = POOM_DRONE_EMUL_MAX_COUNT;
        }

        for (uint8_t i = 0; i < count; i++)
        {
            uint8_t mac_local[6];
            (void)memcpy(mac_local, s_ap_mac, sizeof(mac_local));

            if (count > 1U)
            {
                mac_local[0] = (uint8_t)((mac_local[0] & 0xFEU) | 0x02U);
                mac_local[5] = (uint8_t)(mac_local[5] + i);
            }

            odid_fill_static_index_(&uas, (uint8_t)(i + 1U));

            char ssid_i[33];
            if (count > 1U)
            {
                (void)snprintf(ssid_i, sizeof(ssid_i), "%.24s %u", ssid_local, (unsigned)(i + 1U));
            }
            else
            {
                (void)snprintf(ssid_i, sizeof(ssid_i), "%s", ssid_local);
            }
            const size_t ssid_len = strnlen(ssid_i, 32U);

            int32_t lat_i = lat_e7 + (int32_t)i * 500;
            int32_t lon_i = lon_e7 + (int32_t)i * 500;
            if (!loc_e7_is_valid_(lat_i, lon_i))
            {
                lat_i = lat_e7;
                lon_i = lon_e7;
            }
            odid_fill_location_(&uas, lat_i, lon_i);

            uas.BasicIDValid[0] = 1;
            uas.LocationValid = 1;
            uas.SelfIDValid = 1;
            uas.SystemValid = 1;
            uas.OperatorIDValid = 1;

            const uint8_t msg_counter = ++s_send_counter;

            const int beacon_len =
                odid_wifi_build_message_pack_beacon_frame_compat_(&uas,
                                                                 mac_local,
                                                                 ssid_i,
                                                                 ssid_len,
                                                                 channel,
                                                                 0x0200, /* interval TU */
                                                                 msg_counter,
                                                                 buf,
                                                                 sizeof(buf));
            if (beacon_len > 0)
            {
                (void)raw_tx_with_retry_("beacon_tx", buf, (size_t)beacon_len);
            }

            const int nan_len =
                odid_wifi_build_message_pack_nan_action_frame(&uas,
                                                             (char *)mac_local,
                                                             msg_counter,
                                                             buf,
                                                             sizeof(buf));
            if (nan_len > 0)
            {
                (void)raw_tx_with_retry_("nan_action_tx", buf, (size_t)nan_len);
            }

            uas.BasicIDValid[0] = 0;
            uas.LocationValid = 0;
            uas.SelfIDValid = 0;
            uas.SystemValid = 0;
            uas.OperatorIDValid = 0;

            if (count > 1U)
            {
                uint32_t slice_ms = (uint32_t)POOM_DRONE_EMUL_TX_INTERVAL_MS / (uint32_t)count;
                if (slice_ms == 0U)
                {
                    slice_ms = 1U;
                }
                vTaskDelay(pdMS_TO_TICKS(slice_ms));
            }
        }

        if (ble_enabled)
        {
            ODID_UAS_Data uas_ble;
            memset(&uas_ble, 0, sizeof(uas_ble));
            odid_fill_static_index_(&uas_ble, 1U);
            odid_fill_location_(&uas_ble, lat_e7, lon_e7);

            ODID_BasicID_encoded basic = {0};
            ODID_Location_encoded loc = {0};
            ODID_SelfID_encoded self = {0};
            ODID_System_encoded sys = {0};
            ODID_OperatorID_encoded op = {0};

            (void)encodeBasicIDMessage(&basic, &uas_ble.BasicID[0]);
            (void)encodeLocationMessage(&loc, &uas_ble.Location);
            (void)encodeSelfIDMessage(&self, &uas_ble.SelfID);
            (void)encodeSystemMessage(&sys, &uas_ble.System);
            (void)encodeOperatorIDMessage(&op, &uas_ble.OperatorID);

            static uint8_t ble_phase = 0;
            const uint8_t *msg = (const uint8_t *)&basic;
            switch (ble_phase % 5U)
            {
                case 0: msg = (const uint8_t *)&basic; break;
                case 1: msg = (const uint8_t *)&loc; break;
                case 2: msg = (const uint8_t *)&self; break;
                case 3: msg = (const uint8_t *)&sys; break;
                default: msg = (const uint8_t *)&op; break;
            }
            ble_phase++;

            (void)ble_tx_odid_msg_(msg, ODID_MESSAGE_SIZE, 1U);
        }

        if (count <= 1U)
        {
            vTaskDelay(pdMS_TO_TICKS(POOM_DRONE_EMUL_TX_INTERVAL_MS));
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void poom_drone_emul_config_default(poom_drone_emul_config_t *out_cfg)
{
    if (out_cfg == NULL)
    {
        return;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->channel = POOM_DRONE_EMUL_DEFAULT_CHANNEL;
    (void)snprintf(out_cfg->ssid, sizeof(out_cfg->ssid), "POOM DRONE");
    out_cfg->latitude_d = POOM_DRONE_EMUL_DEFAULT_LAT_D;
    out_cfg->longitude_d = POOM_DRONE_EMUL_DEFAULT_LON_D;
    out_cfg->count = POOM_DRONE_EMUL_DEFAULT_COUNT;
    out_cfg->ble_enabled = false;
}

bool poom_drone_emul_is_running(void)
{
    return s_running;
}

esp_err_t poom_drone_emul_get_location(double *out_latitude_d, double *out_longitude_d)
{
    if ((out_latitude_d == NULL) || (out_longitude_d == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t lat_e7;
    int32_t lon_e7;
    portENTER_CRITICAL(&s_cfg_mux);
    lat_e7 = s_lat_e7;
    lon_e7 = s_lon_e7;
    portEXIT_CRITICAL(&s_cfg_mux);

    *out_latitude_d = ((double)lat_e7) / 1e7;
    *out_longitude_d = ((double)lon_e7) / 1e7;
    return ESP_OK;
}

esp_err_t poom_drone_emul_get_count(uint8_t *out_count)
{
    if (out_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t count = 0;
    portENTER_CRITICAL(&s_cfg_mux);
    count = s_count;
    portEXIT_CRITICAL(&s_cfg_mux);

    if (count < 1U)
    {
        count = POOM_DRONE_EMUL_DEFAULT_COUNT;
    }
    if (count > POOM_DRONE_EMUL_MAX_COUNT)
    {
        count = POOM_DRONE_EMUL_MAX_COUNT;
    }

    *out_count = count;
    return ESP_OK;
}

esp_err_t poom_drone_emul_get_ble_enabled(bool *out_enabled)
{
    if (out_enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool enabled = false;
    portENTER_CRITICAL(&s_cfg_mux);
    enabled = s_ble_enabled;
    portEXIT_CRITICAL(&s_cfg_mux);
    *out_enabled = enabled;
    return ESP_OK;
}

esp_err_t poom_drone_emul_set_location(double latitude_d, double longitude_d)
{
    const int32_t lat_e7 = (int32_t)llround(latitude_d * 1e7);
    const int32_t lon_e7 = (int32_t)llround(longitude_d * 1e7);
    if (!loc_e7_is_valid_(lat_e7, lon_e7))
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_cfg_mux);
    s_lat_e7 = lat_e7;
    s_lon_e7 = lon_e7;
    portEXIT_CRITICAL(&s_cfg_mux);

    (void)save_location_to_nvs_(lat_e7, lon_e7);
    return ESP_OK;
}

esp_err_t poom_drone_emul_set_count(uint8_t count)
{
    if (!count_is_valid_(count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_cfg_mux);
    s_count = count;
    portEXIT_CRITICAL(&s_cfg_mux);

    (void)save_count_to_nvs_(count);
    return ESP_OK;
}

esp_err_t poom_drone_emul_set_ble_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_cfg_mux);
    s_ble_enabled = enabled;
    portEXIT_CRITICAL(&s_cfg_mux);

    if (enabled && s_running && !s_ble_active)
    {
        (void)ble_start_();
    }
    else if (!enabled && s_ble_active)
    {
        ble_stop_();
    }

    (void)save_ble_enabled_to_nvs_(enabled);
    return ESP_OK;
}

esp_err_t poom_drone_emul_start(const poom_drone_emul_config_t *cfg)
{
    if (s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_drone_emul_config_t local;
    poom_drone_emul_config_default(&local);
    if (cfg != NULL)
    {
        local = *cfg;
    }

    if ((local.channel < 1U) || (local.channel > 13U))
    {
        local.channel = POOM_DRONE_EMUL_DEFAULT_CHANNEL;
    }
    if ((local.ssid[0] == '\0') || (strnlen(local.ssid, 32U) == 0U))
    {
        (void)snprintf(local.ssid, sizeof(local.ssid), "POOM DRONE");
    }
    if (!count_is_valid_(local.count))
    {
        local.count = POOM_DRONE_EMUL_DEFAULT_COUNT;
    }

    int32_t lat_e7 = (int32_t)llround(local.latitude_d * 1e7);
    int32_t lon_e7 = (int32_t)llround(local.longitude_d * 1e7);

    int32_t lat_saved = 0;
    int32_t lon_saved = 0;
    if (load_location_from_nvs_(&lat_saved, &lon_saved) == ESP_OK)
    {
        lat_e7 = lat_saved;
        lon_e7 = lon_saved;
    }
    if (!loc_e7_is_valid_(lat_e7, lon_e7))
    {
        lat_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LAT_D * 1e7);
        lon_e7 = (int32_t)llround(POOM_DRONE_EMUL_DEFAULT_LON_D * 1e7);
    }

    uint8_t count_saved = 0;
    if (load_count_from_nvs_(&count_saved) == ESP_OK)
    {
        local.count = count_saved;
    }
    if (!count_is_valid_(local.count))
    {
        local.count = POOM_DRONE_EMUL_DEFAULT_COUNT;
    }

    bool ble_saved = false;
    if (load_ble_enabled_from_nvs_(&ble_saved) == ESP_OK)
    {
        local.ble_enabled = ble_saved;
    }

    esp_err_t err = ensure_ap_for_raw_tx_(local.channel);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac);
    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_cfg_mux);
    s_channel = local.channel;
    (void)snprintf(s_ssid, sizeof(s_ssid), "%s", local.ssid);
    s_lat_e7 = lat_e7;
    s_lon_e7 = lon_e7;
    s_count = local.count;
    s_ble_enabled = local.ble_enabled;
    portEXIT_CRITICAL(&s_cfg_mux);

    s_send_counter = 0;
    s_tx_error_count = 0U;
    s_running = true;

    if (local.ble_enabled)
    {
        (void)ble_start_();
    }

    if (xTaskCreate(tx_task_, "poom_drone_emul", 4096, NULL, 5, &s_task) != pdPASS)
    {
        s_running = false;
        (void)poom_wifi_ctrl_ap_stop();
        ble_stop_();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t poom_drone_emul_stop(void)
{
    if (!s_running)
    {
        return ESP_OK;
    }

    s_running = false;
    if (s_task != NULL)
    {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    ble_stop_();
    return poom_wifi_ctrl_ap_stop();
}
