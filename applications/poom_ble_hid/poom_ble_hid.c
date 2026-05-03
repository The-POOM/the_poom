// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_hid.h"

#include <string.h>
#include <stdio.h>

#include "nvs_flash.h"
#include "esp_err.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"

#include "esp_gap_ble_api.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"

/* Some ESP-IDF releases do not expose these prototypes in public headers. */
void esp_hidd_send_keyboard_value(uint16_t conn_id,
                                 uint8_t special_key_mask,
                                 uint8_t *keyboard_cmd,
                                 uint8_t num_key);

void esp_hidd_send_mouse_value(uint16_t conn_id,
                              uint8_t mouse_button,
                              int8_t mouse_x,
                              int8_t mouse_y);

/* =========================
 * Logging
 * ========================= */

static const char *POOM_BLE_HID_TAG = "poom_ble_hid";

#if (CONFIG_POOM_BLE_HID_ENABLE_LOG == 1)

  #if (POOM_BLE_HID_USE_PRINTF == 1)
    #define POOM_LOGI(fmt, ...)  printf("[I][%s] %s:%d: " fmt "\n", POOM_BLE_HID_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #define POOM_LOGW(fmt, ...)  printf("[W][%s] %s:%d: " fmt "\n", POOM_BLE_HID_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #define POOM_LOGE(fmt, ...)  printf("[E][%s] %s:%d: " fmt "\n", POOM_BLE_HID_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #define POOM_LOGD(fmt, ...)  printf("[D][%s] %s:%d: " fmt "\n", POOM_BLE_HID_TAG, __func__, __LINE__, ##__VA_ARGS__)
  #else
    #include "esp_log.h"
    #define POOM_LOGI(fmt, ...)  ESP_LOGI(POOM_BLE_HID_TAG, fmt, ##__VA_ARGS__)
    #define POOM_LOGW(fmt, ...)  ESP_LOGW(POOM_BLE_HID_TAG, fmt, ##__VA_ARGS__)
    #define POOM_LOGE(fmt, ...)  ESP_LOGE(POOM_BLE_HID_TAG, fmt, ##__VA_ARGS__)
    #define POOM_LOGD(fmt, ...)  ESP_LOGD(POOM_BLE_HID_TAG, fmt, ##__VA_ARGS__)
  #endif

#else
  #define POOM_LOGI(...)
  #define POOM_LOGW(...)
  #define POOM_LOGE(...)
  #define POOM_LOGD(...)
#endif

/* =========================
 * Local constants
 * ========================= */
#define POOM_BLE_HID_KEY_MAX_KEYS            (6U)

#define POOM_BLE_HID_ADV_INT_MIN             (0x20U)
#define POOM_BLE_HID_ADV_INT_MAX             (0x30U)

#define POOM_BLE_HID_APPEARANCE_HID_GENERIC  (0x03C0U)

#define POOM_BLE_HID_CONN_MIN_INTERVAL       (0x0006U)
#define POOM_BLE_HID_CONN_MAX_INTERVAL       (0x0010U)

#define POOM_BLE_HID_CLAMP_MIN_I8            (-127)
#define POOM_BLE_HID_CLAMP_MAX_I8            (127)

/* =========================
 * Local state
 * ========================= */
static uint16_t s_hid_conn_id = 0U;
static bool s_secured = false;
static bool s_is_started = false;
static hid_event_callback_f s_conn_cb = NULL;
static bool s_is_connected = false;
static esp_bd_addr_t s_remote_bda = {0};
static bool s_gap_cb_registered = false;
static bool s_hidd_cb_registered = false;
static bool s_hidd_profile_inited = false;

static const char s_device_name[] = "POOM_HID";

static uint8_t s_hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = POOM_BLE_HID_CONN_MIN_INTERVAL,
    .max_interval        = POOM_BLE_HID_CONN_MAX_INTERVAL,
    .appearance          = POOM_BLE_HID_APPEARANCE_HID_GENERIC,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(s_hidd_service_uuid128),
    .p_service_uuid      = s_hidd_service_uuid128,
    .flag                = 0x06,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = POOM_BLE_HID_ADV_INT_MIN,
    .adv_int_max       = POOM_BLE_HID_ADV_INT_MAX,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Notify registered connection callback.
 * @param[in] connected Connection state.
 * @return esp_err_t
 */
static void poom_ble_hid_notify_connection_state_(bool connected)
{
    if (s_conn_cb != NULL)
    {
        s_conn_cb(connected);
    }
}

/**
 * @brief Initialize NVS storage required by BLE stack.
 * @return esp_err_t
 */
static esp_err_t poom_ble_hid_initialize_nvs_(void)
{
    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        esp_err_t e2 = nvs_flash_erase();
        if (e2 != ESP_OK)
        {
            return e2;
        }
        ret = nvs_flash_init();
    }
    return ret;
}

/**
 * @brief Clamp integer value to int8 report range.
 * @param[in] v Input value.
 * @return esp_err_t
 */
static int8_t poom_ble_hid_clamp_to_i8_(int v)
{
    if (v > POOM_BLE_HID_CLAMP_MAX_I8)
    {
        return (int8_t)POOM_BLE_HID_CLAMP_MAX_I8;
    }
    if (v < POOM_BLE_HID_CLAMP_MIN_I8)
    {
        return (int8_t)POOM_BLE_HID_CLAMP_MIN_I8;
    }
    return (int8_t)v;
}

/**
 * @brief Print hexadecimal payload in debug logs.
 * @param[in] data Pointer to payload bytes.
 * @param[in] len Number of bytes.
 * @return esp_err_t
 */
static void poom_ble_hid_log_hex_payload_(const uint8_t *data, uint16_t len)
{
#if (CONFIG_POOM_BLE_HID_ENABLE_LOG == 1) && (POOM_BLE_HID_USE_PRINTF == 1)
    uint16_t i;
    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    printf("[D][%s] HEX(%u): ", POOM_BLE_HID_TAG, (unsigned)len);
    for (i = 0U; i < len; i++)
    {
        printf("%02X", (unsigned)data[i]);
        if ((i + 1U) < len)
        {
            printf(" ");
        }
    }
    printf("\n");
#else
    (void)data;
    (void)len;
#endif
}

/**
 * @brief Check if HID reports can be sent.
 * @return esp_err_t
 */
static bool poom_ble_hid_is_report_ready_(void)
{
    return (s_secured == true);
}

/* =========================
 * Forward declarations
 * ========================= */
static void poom_ble_hid_on_hidd_event_(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param);
static void poom_ble_hid_on_gap_event_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/* =========================
 * HIDD callback
 * ========================= */
/**
 * @brief Handle HID profile callback events.
 * @param[in] event HID callback event.
 * @param[in/out] param HID callback parameters.
 * @return esp_err_t
 */
static void poom_ble_hid_on_hidd_event_(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event)
    {
        case ESP_HIDD_EVENT_REG_FINISH:
            if ((param != NULL) && (param->init_finish.state == ESP_HIDD_INIT_OK))
            {
                (void)esp_ble_gap_set_device_name(s_device_name);
                (void)esp_ble_gap_config_adv_data(&s_adv_data);
                POOM_LOGI("HID REG_FINISH OK -> config adv data");
            }
            else
            {
                int st = (param != NULL) ? (int)param->init_finish.state : -1;
                POOM_LOGE("HID REG_FINISH failed (state=%d)", st);
            }
            break;

        case ESP_HIDD_EVENT_BLE_CONNECT:
            if (param != NULL)
            {
                s_hid_conn_id = param->connect.conn_id;
                memcpy(s_remote_bda, param->connect.remote_bda, sizeof(s_remote_bda));
                s_is_connected = true;
                POOM_LOGI("BLE_CONNECT conn_id=%u", (unsigned)s_hid_conn_id);
                poom_ble_hid_notify_connection_state_(true);
            }
            break;

        case ESP_HIDD_EVENT_BLE_DISCONNECT:
            s_secured = false;
            s_is_connected = false;
            memset(s_remote_bda, 0, sizeof(s_remote_bda));
            POOM_LOGW("BLE_DISCONNECT -> restart advertising");
            (void)esp_ble_gap_start_advertising(&s_adv_params);
            poom_ble_hid_notify_connection_state_(false);
            break;

        case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT:
            if (param != NULL)
            {
                POOM_LOGI("VENDOR_REPORT_WRITE len=%u", (unsigned)param->vendor_write.length);
                poom_ble_hid_log_hex_payload_(param->vendor_write.data, (uint16_t)param->vendor_write.length);
            }
            break;

        case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
            if (param != NULL)
            {
                POOM_LOGI("LED_REPORT_WRITE len=%u", (unsigned)param->led_write.length);
                poom_ble_hid_log_hex_payload_(param->led_write.data, (uint16_t)param->led_write.length);
            }
            break;

        default:
            break;
    }
}

/* =========================
 * GAP callback
 * ========================= */
/**
 * @brief Handle BLE GAP callback events.
 * @param[in] event GAP callback event.
 * @param[in/out] param GAP callback parameters.
 * @return esp_err_t
 */
static void poom_ble_hid_on_gap_event_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            (void)esp_ble_gap_start_advertising(&s_adv_params);
            POOM_LOGI("ADV data set complete -> start advertising");
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if ((param != NULL) && (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS))
            {
                POOM_LOGE("ADV_START_COMPLETE failed status=0x%x", (unsigned)param->adv_start_cmpl.status);
            }
            else
            {
                POOM_LOGI("ADV_START_COMPLETE OK");
            }
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            if (param != NULL)
            {
                (void)esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
                POOM_LOGD("SEC_REQ accepted");
            }
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (param != NULL)
            {
                s_secured = (param->ble_security.auth_cmpl.success != 0);
                POOM_LOGI("AUTH complete: %s", (s_secured != 0) ? "success" : "fail");
                if (s_secured == false)
                {
                    POOM_LOGE("AUTH fail reason=0x%x", (unsigned)param->ble_security.auth_cmpl.fail_reason);
                }
            }
            break;

        default:
            break;
    }
}

/* =========================
 * Public API
 * ========================= */
/**
 * @brief Register BLE connection state callback.
 * @param[in] callback Callback function pointer.
 * @return esp_err_t
 */
void poom_ble_hid_set_connection_callback(hid_event_callback_f callback)
{
    s_conn_cb = callback;
}

/**
 * @brief Initialize BLE HID and security configuration.
 * @return esp_err_t
 */
void poom_ble_hid_start(void)
{
    esp_err_t ret;

    s_is_connected = false;
    s_secured = false;
    memset(s_remote_bda, 0, sizeof(s_remote_bda));

    if (s_is_started)
    {
        // Already started: just ensure advertising is running again.
        (void)esp_ble_gap_config_adv_data(&s_adv_data);
        (void)esp_ble_gap_start_advertising(&s_adv_params);
        s_secured = false;
        s_hid_conn_id = 0U;
        return;
    }

    ret = poom_ble_hid_initialize_nvs_();
    if (ret != ESP_OK)
    {
        POOM_LOGE("NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_controller_status_t bt_st = esp_bt_controller_get_status();
    if (bt_st == ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret != ESP_OK)
        {
            POOM_LOGW("mem_release classic: %s", esp_err_to_name(ret));
        }

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK)
        {
            POOM_LOGE("bt controller init failed: %s", esp_err_to_name(ret));
            return;
        }
        bt_st = esp_bt_controller_get_status();
    }

    if (bt_st == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK)
        {
            POOM_LOGE("bt controller enable failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    esp_bluedroid_status_t bd_st = esp_bluedroid_get_status();
    if (bd_st == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        ret = esp_bluedroid_init();
        if (ret != ESP_OK)
        {
            POOM_LOGE("bluedroid init failed: %s", esp_err_to_name(ret));
            return;
        }
        bd_st = esp_bluedroid_get_status();
    }

    if (bd_st == ESP_BLUEDROID_STATUS_INITIALIZED)
    {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK)
        {
            POOM_LOGE("bluedroid enable failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    if (!s_hidd_profile_inited)
    {
        ret = esp_hidd_profile_init();
        if (ret != ESP_OK)
        {
            POOM_LOGE("hidd profile init failed: %s", esp_err_to_name(ret));
            return;
        }
        s_hidd_profile_inited = true;
    }

    if (!s_gap_cb_registered)
    {
        if (esp_ble_gap_get_callback() == NULL)
        {
            ret = esp_ble_gap_register_callback(poom_ble_hid_on_gap_event_);
            if (ret != ESP_OK)
            {
                POOM_LOGE("gap register callback failed: %s", esp_err_to_name(ret));
                return;
            }
        }
        s_gap_cb_registered = true;
    }

    if (!s_hidd_cb_registered)
    {
        esp_hidd_register_callbacks(poom_ble_hid_on_hidd_event_);
        s_hidd_cb_registered = true;
    }

    {
        esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
        esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
        uint8_t key_size = 16U;
        uint8_t init_key = (uint8_t)(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        uint8_t rsp_key  = (uint8_t)(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        (void)esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
        (void)esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
        (void)esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
        (void)esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
        (void)esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    }

    s_hid_conn_id = 0U;
    s_secured = false;

    s_is_started = true;

    // Ensure advertising restarts whenever we (re)start the module.
    (void)esp_ble_gap_config_adv_data(&s_adv_data);
    (void)esp_ble_gap_start_advertising(&s_adv_params);

    POOM_LOGI("Initialized (name=%s)", s_device_name);
}


/**
 * @brief Stop BLE HID stack and advertising.
 */
void poom_ble_hid_stop(void)
{
    if (!s_is_started)
    {
        return;
    }

    // Best-effort: disconnect (if connected), stop advertising, then stop HID profile.
    if (s_is_connected)
    {
        (void)esp_ble_gap_disconnect(s_remote_bda);
        vTaskDelay(pdMS_TO_TICKS(250));
        s_is_connected = false;
        s_secured = false;
        memset(s_remote_bda, 0, sizeof(s_remote_bda));
        poom_ble_hid_notify_connection_state_(false);
    }

    (void)esp_ble_gap_stop_advertising();

    s_is_started = false;
    s_hid_conn_id = 0U;
    s_secured = false;
    s_conn_cb = NULL;
}

bool poom_ble_hid_is_started(void)
{
    return s_is_started;
}

/* =========================
 * Consumer controls
 * ========================= */
/**
 * @brief Send consumer control key state.
 * @param[in] usage HID consumer usage code.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
static void poom_ble_hid_send_consumer_usage_(uint16_t usage, bool press)
{
    if (poom_ble_hid_is_report_ready_())
    {
        esp_hidd_send_consumer_value(s_hid_conn_id, usage, press);
    }
}

/**
 * @brief Send volume-up consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_volume_up(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_VOLUME_UP, press);
}

/**
 * @brief Send volume-down consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_volume_down(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_VOLUME_DOWN, press);
}

/**
 * @brief Send play consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_play(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_PLAY, press);
}

/**
 * @brief Send pause consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_pause(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_PAUSE, press);
}

/**
 * @brief Send next-track consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_next_track(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_SCAN_NEXT_TRK, press);
}

/**
 * @brief Send previous-track consumer key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_prev_track(bool press)
{
    poom_ble_hid_send_consumer_usage_(HID_CONSUMER_SCAN_PREV_TRK, press);
}

/* =========================
 * Mouse
 * ========================= */
/**
 * @brief Send mouse movement and button report.
 * @param[in] dx Relative X movement.
 * @param[in] dy Relative Y movement.
 * @param[in] buttons Mouse buttons bitmask.
 * @return esp_err_t
 */
void poom_ble_hid_send_mouse_move(int dx, int dy, uint8_t buttons)
{
    if (!poom_ble_hid_is_report_ready_())
    {
        return;
    }

    {
        int8_t x = poom_ble_hid_clamp_to_i8_(dx);
        int8_t y = poom_ble_hid_clamp_to_i8_(dy);
        esp_hidd_send_mouse_value(s_hid_conn_id, buttons, x, y);
    }
}

/* =========================
 * Keyboard (ASCII)
 * ========================= */
#ifndef MOD_LSHIFT
#define MOD_LSHIFT  (0x02U)
#endif

typedef struct
{
    uint8_t mod;
    uint8_t code;
} poom_hid_key_t;

/**
 * @brief Convert ASCII character to HID modifier and keycode.
 * @param[in] c ASCII character.
 * @return esp_err_t
 */
static poom_hid_key_t poom_ble_hid_map_ascii_to_hid_key_(unsigned char c)
{
    poom_hid_key_t out;
    out.mod = 0U;
    out.code = 0U;

    if ((c >= (unsigned char)'a') && (c <= (unsigned char)'z'))
    {
        out.code = (uint8_t)(0x04U + (uint8_t)(c - (unsigned char)'a'));
        return out;
    }

    if ((c >= (unsigned char)'A') && (c <= (unsigned char)'Z'))
    {
        out.mod = MOD_LSHIFT;
        out.code = (uint8_t)(0x04U + (uint8_t)(c - (unsigned char)'A'));
        return out;
    }

    switch (c)
    {
        case '1': out.code = 0x1EU; return out;
        case '!': out.mod = MOD_LSHIFT; out.code = 0x1EU; return out;
        case '2': out.code = 0x1FU; return out;
        case '@': out.mod = MOD_LSHIFT; out.code = 0x1FU; return out;
        case '3': out.code = 0x20U; return out;
        case '#': out.mod = MOD_LSHIFT; out.code = 0x20U; return out;
        case '4': out.code = 0x21U; return out;
        case '$': out.mod = MOD_LSHIFT; out.code = 0x21U; return out;
        case '5': out.code = 0x22U; return out;
        case '%': out.mod = MOD_LSHIFT; out.code = 0x22U; return out;
        case '6': out.code = 0x23U; return out;
        case '^': out.mod = MOD_LSHIFT; out.code = 0x23U; return out;
        case '7': out.code = 0x24U; return out;
        case '&': out.mod = MOD_LSHIFT; out.code = 0x24U; return out;
        case '8': out.code = 0x25U; return out;
        case '*': out.mod = MOD_LSHIFT; out.code = 0x25U; return out;
        case '9': out.code = 0x26U; return out;
        case '(': out.mod = MOD_LSHIFT; out.code = 0x26U; return out;
        case '0': out.code = 0x27U; return out;
        case ')': out.mod = MOD_LSHIFT; out.code = 0x27U; return out;
        default:
            break;
    }

    switch (c)
    {
        case ' ':  out.code = 0x2CU; return out;
        case '\n':
        case '\r': out.code = 0x28U; return out;
        case '\t': out.code = 0x2BU; return out;

        case '-': out.code = 0x2DU; return out;
        case '_': out.mod = MOD_LSHIFT; out.code = 0x2DU; return out;
        case '=': out.code = 0x2EU; return out;
        case '+': out.mod = MOD_LSHIFT; out.code = 0x2EU; return out;
        case '[': out.code = 0x2FU; return out;
        case '{': out.mod = MOD_LSHIFT; out.code = 0x2FU; return out;
        case ']': out.code = 0x30U; return out;
        case '}': out.mod = MOD_LSHIFT; out.code = 0x30U; return out;
        case '\\': out.code = 0x31U; return out;
        case '|': out.mod = MOD_LSHIFT; out.code = 0x31U; return out;

        case ';': out.code = 0x33U; return out;
        case ':': out.mod = MOD_LSHIFT; out.code = 0x33U; return out;
        case '\'': out.code = 0x34U; return out;
        case '"': out.mod = MOD_LSHIFT; out.code = 0x34U; return out;
        case '`': out.code = 0x35U; return out;
        case '~': out.mod = MOD_LSHIFT; out.code = 0x35U; return out;

        case ',': out.code = 0x36U; return out;
        case '<': out.mod = MOD_LSHIFT; out.code = 0x36U; return out;
        case '.': out.code = 0x37U; return out;
        case '>': out.mod = MOD_LSHIFT; out.code = 0x37U; return out;
        case '/': out.code = 0x38U; return out;
        case '?': out.mod = MOD_LSHIFT; out.code = 0x38U; return out;

        default:
            break;
    }

    return out;
}

/**
 * @brief Send keyboard press report with single key.
 * @param[in] mod HID modifier bitmask.
 * @param[in] keycode HID keycode.
 * @return esp_err_t
 */
static void poom_ble_hid_send_keyboard_press_(uint8_t mod, uint8_t keycode)
{
    uint8_t keys[POOM_BLE_HID_KEY_MAX_KEYS];

    (void)memset(keys, 0, sizeof(keys));
    keys[0] = keycode;

    esp_hidd_send_keyboard_value(s_hid_conn_id, mod, keys, 1U);
}

/**
 * @brief Send keyboard report with no pressed keys.
 * @return esp_err_t
 */
static void poom_ble_hid_send_keyboard_release_all_(void)
{
    uint8_t none[POOM_BLE_HID_KEY_MAX_KEYS];

    (void)memset(none, 0, sizeof(none));

    esp_hidd_send_keyboard_value(s_hid_conn_id, 0U, none, 0U);
}

/**
 * @brief Release all currently pressed keyboard keys.
 * @return esp_err_t
 */
void poom_ble_hid_key_release_all(void)
{
    if (!poom_ble_hid_is_report_ready_())
    {
        return;
    }

    poom_ble_hid_send_keyboard_release_all_();
}

/**
 * @brief Press and hold one ASCII character key.
 * @param[in] c ASCII character to press.
 * @return esp_err_t
 */
void poom_ble_hid_key_press_ascii(char c)
{
    poom_hid_key_t k;

    if (!poom_ble_hid_is_report_ready_())
    {
        return;
    }

    k = poom_ble_hid_map_ascii_to_hid_key_((unsigned char)c);
    if (k.code == 0U)
    {
        return;
    }

    poom_ble_hid_send_keyboard_press_(k.mod, k.code);
}
