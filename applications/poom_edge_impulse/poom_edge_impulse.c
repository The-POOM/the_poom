// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_edge_impulse.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "poom_imu_stream.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"


#if POOM_EDGE_IMPULSE_ENABLE_LOG
    static const char* POOM_EDGE_IMPULSE_TAG = "poom_edge_impulse";

    #define POOM_EDGE_IMPULSE_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_EDGE_IMPULSE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_EDGE_IMPULSE_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_EDGE_IMPULSE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_EDGE_IMPULSE_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_EDGE_IMPULSE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_EDGE_IMPULSE_DEBUG_LOG_ENABLED
        #define POOM_EDGE_IMPULSE_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_EDGE_IMPULSE_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_EDGE_IMPULSE_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_EDGE_IMPULSE_PRINTF_E(...) do { } while (0)
    #define POOM_EDGE_IMPULSE_PRINTF_W(...) do { } while (0)
    #define POOM_EDGE_IMPULSE_PRINTF_I(...) do { } while (0)
    #define POOM_EDGE_IMPULSE_PRINTF_D(...) do { } while (0)
#endif

#ifndef POOM_EDGE_IMPULSE_DEFAULT_LABEL
#define POOM_EDGE_IMPULSE_DEFAULT_LABEL "imu-acc-gyro-temp"
#endif

#ifndef POOM_EDGE_IMPULSE_API_URL
#define POOM_EDGE_IMPULSE_API_URL "http://ingestion.edgeimpulse.com/api/training/files"
#endif

#define POOM_EDGE_IMPULSE_WIFI_SSID_MAX_LEN (32U)
#define POOM_EDGE_IMPULSE_WIFI_PASS_MAX_LEN (64U)
#define POOM_EDGE_IMPULSE_API_KEY_MAX_LEN (192U)
#define POOM_EDGE_IMPULSE_LABEL_BUFFER_LEN (POOM_EDGE_IMPULSE_LABEL_MAX_LEN + 1U)

#define POOM_EDGE_IMPULSE_CAPTURE_STACK_SIZE (8192U)
#define POOM_EDGE_IMPULSE_CAPTURE_TASK_PRIORITY (5U)
#define POOM_EDGE_IMPULSE_MIN_SAMPLE_PERIOD_MS (5U)
#define POOM_EDGE_IMPULSE_DEFAULT_SAMPLE_PERIOD_MS (20U)
#define POOM_EDGE_IMPULSE_DEFAULT_CAPTURE_TIMEOUT_MS (5000U)

#define POOM_EDGE_IMPULSE_MDPS_TO_DPS (1.0f / 1000.0f)
#define POOM_EDGE_IMPULSE_MG_TO_MS2 (9.80665f / 1000.0f)

#define POOM_EDGE_IMPULSE_BOUNDARY "----POOM_EDGE_IMPULSE_BOUNDARY"

typedef struct
{
    bool initialized;
    bool wifi_connected;
    bool capture_abort_requested;
    uint32_t sample_period_ms;
    uint32_t capture_timeout_ms;
    char wifi_ssid[POOM_EDGE_IMPULSE_WIFI_SSID_MAX_LEN + 1U];
    char wifi_password[POOM_EDGE_IMPULSE_WIFI_PASS_MAX_LEN + 1U];
    char api_key[POOM_EDGE_IMPULSE_API_KEY_MAX_LEN + 1U];
    char current_label[POOM_EDGE_IMPULSE_LABEL_BUFFER_LEN];
    TaskHandle_t capture_task;
    TimerHandle_t capture_timer;
    poom_edge_impulse_capture_done_cb_t capture_done_cb;
    void* capture_done_user_ctx;
} poom_edge_impulse_state_t;

static poom_edge_impulse_state_t s_poom_edge_impulse = {0};

/**
 * @brief Copies a C string into fixed destination buffer.
 *
 * @param[in] source Source string.
 * @param[out] destination Destination buffer.
 * @param[in] destination_len Destination buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_copy_string_(const char* source, char* destination, size_t destination_len)
{
    size_t source_len;

    if((source == NULL) || (destination == NULL) || (destination_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    source_len = strlen(source);
    if(source_len >= destination_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)memcpy(destination, source, source_len + 1U);
    return ESP_OK;
}

/**
 * @brief Stops and deletes capture timeout timer.
 *
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_capture_timer_deinit_(void)
{
    if(s_poom_edge_impulse.capture_timer == NULL)
    {
        return ESP_OK;
    }

    (void)xTimerStop(s_poom_edge_impulse.capture_timer, 0U);
    if(xTimerDelete(s_poom_edge_impulse.capture_timer, 0U) != pdPASS)
    {
        return ESP_FAIL;
    }

    s_poom_edge_impulse.capture_timer = NULL;
    return ESP_OK;
}

/**
 * @brief Callback executed on capture timeout.
 *
 * @param[in] timer_handle FreeRTOS timer handle.
 * @return void
 */
static void poom_edge_impulse_capture_timer_cb_(TimerHandle_t timer_handle)
{
    (void)timer_handle;

    s_poom_edge_impulse.capture_abort_requested = true;
    POOM_EDGE_IMPULSE_PRINTF_W("capture timeout reached");
}

/**
 * @brief Ensures capture timeout timer is created and started.
 *
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_capture_timer_start_(void)
{
    TickType_t timeout_ticks;

    timeout_ticks = pdMS_TO_TICKS(s_poom_edge_impulse.capture_timeout_ms);
    if(timeout_ticks == 0)
    {
        timeout_ticks = 1;
    }

    if(s_poom_edge_impulse.capture_timer == NULL)
    {
        s_poom_edge_impulse.capture_timer = xTimerCreate(
            "poom_ei_cap",
            timeout_ticks,
            pdFALSE,
            NULL,
            poom_edge_impulse_capture_timer_cb_);
        if(s_poom_edge_impulse.capture_timer == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    (void)xTimerStop(s_poom_edge_impulse.capture_timer, 0U);
    if(xTimerChangePeriod(s_poom_edge_impulse.capture_timer, timeout_ticks, 0U) != pdPASS)
    {
        return ESP_FAIL;
    }

    if(xTimerStart(s_poom_edge_impulse.capture_timer, 0U) != pdPASS)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Stops active capture timer if present.
 *
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_capture_timer_stop_(void)
{
    if(s_poom_edge_impulse.capture_timer == NULL)
    {
        return ESP_OK;
    }

    if(xTimerStop(s_poom_edge_impulse.capture_timer, 0U) != pdPASS)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Builds Edge Impulse JSON payload from IMU samples.
 *
 * @param[out] out_json Allocated JSON string.
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_build_payload_json_(char** out_json)
{
    cJSON* root;
    cJSON* payload;
    cJSON* sensors;
    cJSON* values;
    int64_t start_us;

    if(out_json == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_json = NULL;

    root = cJSON_CreateObject();
    payload = cJSON_CreateObject();
    sensors = cJSON_CreateArray();
    values = cJSON_CreateArray();

    if((root == NULL) || (payload == NULL) || (sensors == NULL) || (values == NULL))
    {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        cJSON_Delete(sensors);
        cJSON_Delete(values);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(payload, "device_name", "poom-device");
    cJSON_AddStringToObject(payload, "device_type", "ESP32");
    cJSON_AddNumberToObject(payload, "interval_ms", (double)s_poom_edge_impulse.sample_period_ms);

    {
        static const char* sensor_names[] = {"accX", "accY", "accZ", "gyroX", "gyroY", "gyroZ", "tempC"};
        static const char* sensor_units[] = {"m/s2", "m/s2", "m/s2", "deg/s", "deg/s", "deg/s", "degC"};
        size_t sensor_index;

        for(sensor_index = 0U; sensor_index < 7U; sensor_index++)
        {
            cJSON* sensor = cJSON_CreateObject();
            if(sensor == NULL)
            {
                cJSON_Delete(root);
                cJSON_Delete(payload);
                cJSON_Delete(sensors);
                cJSON_Delete(values);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(sensor, "name", sensor_names[sensor_index]);
            cJSON_AddStringToObject(sensor, "units", sensor_units[sensor_index]);
            cJSON_AddItemToArray(sensors, sensor);
        }
    }

    cJSON_AddItemToObject(payload, "sensors", sensors);

    start_us = esp_timer_get_time();
    while(!s_poom_edge_impulse.capture_abort_requested)
    {
        poom_imu_data_t imu_data;
        int64_t elapsed_ms;

        if(poom_imu_stream_read_data(&imu_data))
        {
            cJSON* row = cJSON_CreateArray();
            if(row == NULL)
            {
                cJSON_Delete(root);
                cJSON_Delete(payload);
                cJSON_Delete(values);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.acceleration_mg[0] * POOM_EDGE_IMPULSE_MG_TO_MS2));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.acceleration_mg[1] * POOM_EDGE_IMPULSE_MG_TO_MS2));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.acceleration_mg[2] * POOM_EDGE_IMPULSE_MG_TO_MS2));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.angular_rate_mdps[0] * POOM_EDGE_IMPULSE_MDPS_TO_DPS));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.angular_rate_mdps[1] * POOM_EDGE_IMPULSE_MDPS_TO_DPS));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.angular_rate_mdps[2] * POOM_EDGE_IMPULSE_MDPS_TO_DPS));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(imu_data.temperature_degC));
            cJSON_AddItemToArray(values, row);
        }

        elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if(elapsed_ms >= (int64_t)s_poom_edge_impulse.capture_timeout_ms)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(s_poom_edge_impulse.sample_period_ms));
    }

    cJSON_AddItemToObject(payload, "values", values);

    {
        cJSON* protected_obj = cJSON_CreateObject();
        if(protected_obj == NULL)
        {
            cJSON_Delete(root);
            cJSON_Delete(payload);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(protected_obj, "alg", "none");
        cJSON_AddStringToObject(protected_obj, "ver", "v1");
        cJSON_AddItemToObject(root, "protected", protected_obj);
    }

    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(root, "signature", "00");

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if(*out_json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Uploads one JSON payload to Edge Impulse ingestion API.
 *
 * @param[in] json_payload JSON payload string.
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_upload_json_(const char* json_payload)
{
    esp_http_client_config_t http_config;
    esp_http_client_handle_t http_client;
    char* multipart_body;
    char content_type[96];
    size_t multipart_capacity;
    bool is_https_url;
    int body_len;
    esp_err_t status;

    if(json_payload == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    multipart_capacity = strlen(json_payload) + 512U;
    multipart_body = (char*)malloc(multipart_capacity);
    if(multipart_body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    body_len = snprintf(
        multipart_body,
        multipart_capacity,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"data\"; filename=\"imu.json\"\r\n"
        "Content-Type: application/json\r\n\r\n"
        "%s\r\n"
        "--%s--\r\n",
        POOM_EDGE_IMPULSE_BOUNDARY,
        json_payload,
        POOM_EDGE_IMPULSE_BOUNDARY);
    if((body_len < 0) || ((size_t)body_len >= multipart_capacity))
    {
        free(multipart_body);
        return ESP_ERR_INVALID_SIZE;
    }

    (void)memset(&http_config, 0, sizeof(http_config));
    http_config.url = POOM_EDGE_IMPULSE_API_URL;
    is_https_url = (strncmp(POOM_EDGE_IMPULSE_API_URL, "https://", 8) == 0);
    http_config.transport_type = is_https_url ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
    if(is_https_url)
    {
        http_config.skip_cert_common_name_check = true;
    }

    http_client = esp_http_client_init(&http_config);
    if(http_client == NULL)
    {
        free(multipart_body);
        return ESP_FAIL;
    }

    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", POOM_EDGE_IMPULSE_BOUNDARY);

    (void)esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    (void)esp_http_client_set_header(http_client, "Content-Type", content_type);
    (void)esp_http_client_set_header(http_client, "x-api-key", s_poom_edge_impulse.api_key);
    (void)esp_http_client_set_header(http_client, "x-label", s_poom_edge_impulse.current_label);
    (void)esp_http_client_set_post_field(http_client, multipart_body, body_len);

    status = esp_http_client_perform(http_client);
    if(status == ESP_OK)
    {
        int http_status = esp_http_client_get_status_code(http_client);
        char response_buffer[256] = {0};
        int response_len;

        response_len = esp_http_client_read(http_client, response_buffer, sizeof(response_buffer) - 1);
        POOM_EDGE_IMPULSE_PRINTF_I("upload HTTP status=%d", http_status);
        if(response_len > 0)
        {
            POOM_EDGE_IMPULSE_PRINTF_D("upload response=%s", response_buffer);
        }

        if((http_status < 200) || (http_status >= 300))
        {
            status = ESP_FAIL;
        }
    }
    else
    {
        POOM_EDGE_IMPULSE_PRINTF_E("upload failed: %s", esp_err_to_name(status));
    }

    esp_http_client_cleanup(http_client);
    free(multipart_body);
    return status;
}

/**
 * @brief Capture task that samples IMU and uploads data.
 *
 * @param[in] task_arg Unused task argument.
 * @return void
 */
static void poom_edge_impulse_capture_task_(void* task_arg)
{
    char* payload_json = NULL;
    esp_err_t status;
    char capture_label[POOM_EDGE_IMPULSE_LABEL_BUFFER_LEN];

    (void)task_arg;

    s_poom_edge_impulse.capture_task = xTaskGetCurrentTaskHandle();
    s_poom_edge_impulse.capture_abort_requested = false;
    (void)memcpy(capture_label, s_poom_edge_impulse.current_label, sizeof(capture_label));

    status = poom_edge_impulse_capture_timer_start_();
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("capture timer start failed: %s", esp_err_to_name(status));
    }
    else
    {
        status = poom_edge_impulse_build_payload_json_(&payload_json);
        if(status == ESP_OK)
        {
            status = poom_edge_impulse_upload_json_(payload_json);
        }

        if(status == ESP_OK)
        {
            POOM_EDGE_IMPULSE_PRINTF_I("capture uploaded successfully (label=%s)", s_poom_edge_impulse.current_label);
        }
        else
        {
            POOM_EDGE_IMPULSE_PRINTF_E("capture/upload failed: %s", esp_err_to_name(status));
        }
    }

    (void)poom_edge_impulse_capture_timer_stop_();
    free(payload_json);

    s_poom_edge_impulse.capture_task = NULL;
    s_poom_edge_impulse.capture_abort_requested = false;

    if(s_poom_edge_impulse.capture_done_cb != NULL)
    {
        s_poom_edge_impulse.capture_done_cb(status, capture_label, s_poom_edge_impulse.capture_done_user_ctx);
    }

    vTaskDelete(NULL);
}

/**
 * @brief Starts capture task if module is ready.
 *
 * @param[in] label Capture label.
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_start_capture_(const char* label)
{
    BaseType_t task_status;
    esp_err_t status;

    if(label == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!s_poom_edge_impulse.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(!s_poom_edge_impulse.wifi_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(s_poom_edge_impulse.capture_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    status = poom_edge_impulse_copy_string_(
        label,
        s_poom_edge_impulse.current_label,
        sizeof(s_poom_edge_impulse.current_label));
    if(status != ESP_OK)
    {
        return status;
    }

    task_status = xTaskCreate(
        poom_edge_impulse_capture_task_,
        "poom_ei_capture",
        POOM_EDGE_IMPULSE_CAPTURE_STACK_SIZE,
        NULL,
        POOM_EDGE_IMPULSE_CAPTURE_TASK_PRIORITY,
        NULL);

    if(task_status != pdPASS)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Handles Wi-Fi control events from poom_wifi_ctrl.
 *
 * @param[in] info Wi-Fi event information.
 * @param[in] user_ctx User context pointer.
 * @return void
 */
static void poom_edge_impulse_wifi_ctrl_cb_(const poom_wifi_ctrl_evt_info_t* info, void* user_ctx)
{
    (void)user_ctx;

    if(info == NULL)
    {
        return;
    }

    if(info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)
    {
        s_poom_edge_impulse.wifi_connected = true;
        POOM_EDGE_IMPULSE_PRINTF_I("Wi-Fi connected and got IP");
    }
    else if(info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        s_poom_edge_impulse.wifi_connected = false;
        POOM_EDGE_IMPULSE_PRINTF_W("Wi-Fi disconnected (reason=%ld)", (long)info->reason);
    }
}

/**
 * @brief Loads Wi-Fi credentials from poom_secrets_store.
 *
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_load_wifi_credentials_from_secrets_(void)
{
    esp_err_t status;
    size_t ssid_len;
    size_t password_len;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_secrets_init failed: %s", esp_err_to_name(status));
        return status;
    }

    ssid_len = sizeof(s_poom_edge_impulse.wifi_ssid);
    status = poom_secrets_get_wifi_ssid(s_poom_edge_impulse.wifi_ssid, &ssid_len);
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_secrets_get_wifi_ssid failed: %s", esp_err_to_name(status));
        return status;
    }

    password_len = sizeof(s_poom_edge_impulse.wifi_password);
    status = poom_secrets_get_wifi_pass(s_poom_edge_impulse.wifi_password, &password_len);
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_secrets_get_wifi_pass failed: %s", esp_err_to_name(status));
        return status;
    }

    return ESP_OK;
}

/**
 * @brief Loads Edge Impulse API token from poom_secrets_store.
 *
 * @return esp_err_t
 */
static esp_err_t poom_edge_impulse_load_api_key_from_secrets_(void)
{
    esp_err_t status;
    size_t api_key_len;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_secrets_init failed: %s", esp_err_to_name(status));
        return status;
    }

    api_key_len = sizeof(s_poom_edge_impulse.api_key);
    status = poom_secrets_get_api_token(s_poom_edge_impulse.api_key, &api_key_len);
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_secrets_get_api_token failed: %s", esp_err_to_name(status));
        return status;
    }

    return ESP_OK;
}

/**
 * @brief Fills configuration with default values.
 *
 * @param[out] out_config Output configuration structure.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_get_default_config(poom_edge_impulse_config_t* out_config)
{
    if(out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)memset(out_config, 0, sizeof(*out_config));
    out_config->wifi_ssid = NULL;
    out_config->wifi_password = NULL;
    out_config->label = POOM_EDGE_IMPULSE_DEFAULT_LABEL;
    out_config->sample_period_ms = POOM_EDGE_IMPULSE_DEFAULT_SAMPLE_PERIOD_MS;
    out_config->capture_timeout_ms = POOM_EDGE_IMPULSE_DEFAULT_CAPTURE_TIMEOUT_MS;

    return ESP_OK;
}

/**
 * @brief Starts Edge Impulse uploader module and Wi-Fi connection.
 *
 * @param[in] config Runtime configuration.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_start(const poom_edge_impulse_config_t* config)
{
    esp_err_t status;

    if(config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_poom_edge_impulse.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    status = poom_edge_impulse_load_wifi_credentials_from_secrets_();
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("failed to load Wi-Fi credentials from secrets store");
        return status;
    }

    status = poom_edge_impulse_load_api_key_from_secrets_();
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("failed to load Edge Impulse API key from secrets store");
        return status;
    }

    status = poom_edge_impulse_copy_string_((config->label != NULL) ? config->label : POOM_EDGE_IMPULSE_DEFAULT_LABEL,
                                            s_poom_edge_impulse.current_label,
                                            sizeof(s_poom_edge_impulse.current_label));
    if(status != ESP_OK)
    {
        return status;
    }

    s_poom_edge_impulse.sample_period_ms = (config->sample_period_ms < POOM_EDGE_IMPULSE_MIN_SAMPLE_PERIOD_MS)
        ? POOM_EDGE_IMPULSE_MIN_SAMPLE_PERIOD_MS
        : config->sample_period_ms;

    s_poom_edge_impulse.capture_timeout_ms = (config->capture_timeout_ms == 0U)
        ? POOM_EDGE_IMPULSE_DEFAULT_CAPTURE_TIMEOUT_MS
        : config->capture_timeout_ms;

    poom_imu_stream_init();

    status = poom_wifi_ctrl_register_cb(poom_edge_impulse_wifi_ctrl_cb_, NULL);
    if(status != ESP_OK)
    {
        POOM_EDGE_IMPULSE_PRINTF_E("poom_wifi_ctrl_register_cb failed: %s", esp_err_to_name(status));
        return status;
    }

    status = poom_wifi_ctrl_sta_connect(s_poom_edge_impulse.wifi_ssid, s_poom_edge_impulse.wifi_password);
    if(status != ESP_OK)
    {
        (void)poom_wifi_ctrl_unregister_cb();
        POOM_EDGE_IMPULSE_PRINTF_E("poom_wifi_ctrl_sta_connect failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_edge_impulse.initialized = true;
    s_poom_edge_impulse.wifi_connected = poom_wifi_ctrl_sta_has_ip();
    POOM_EDGE_IMPULSE_PRINTF_I("module started, waiting for Wi-Fi IP");
    return ESP_OK;
}

/**
 * @brief Stops Edge Impulse uploader module and releases runtime resources.
 *
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_stop(void)
{
    esp_err_t status = ESP_OK;
    uint32_t wait_counter;

    if(!s_poom_edge_impulse.initialized)
    {
        return ESP_OK;
    }

    s_poom_edge_impulse.capture_abort_requested = true;
    wait_counter = 0U;
    while((s_poom_edge_impulse.capture_task != NULL) && (wait_counter < 50U))
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_counter++;
    }

    if(s_poom_edge_impulse.capture_task != NULL)
    {
        POOM_EDGE_IMPULSE_PRINTF_W("capture task still running while stopping");
    }

    if(poom_edge_impulse_capture_timer_deinit_() != ESP_OK)
    {
        status = ESP_FAIL;
    }

    if(poom_wifi_ctrl_unregister_cb() != ESP_OK)
    {
        status = ESP_FAIL;
    }

    s_poom_edge_impulse.initialized = false;
    s_poom_edge_impulse.wifi_connected = false;
    s_poom_edge_impulse.capture_abort_requested = false;
    s_poom_edge_impulse.capture_task = NULL;

    POOM_EDGE_IMPULSE_PRINTF_I("module stopped");
    return status;
}

/**
 * @brief Triggers one capture and upload cycle with the provided label.
 *
 * @param[in] label Capture label.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_trigger_capture(const char* label)
{
    if(label == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return poom_edge_impulse_start_capture_(label);
}

/**
 * @brief Registers callback invoked when one capture/upload cycle finishes.
 *
 * @param[in] callback Completion callback. Set NULL to disable callbacks.
 * @param[in] user_ctx User context passed back in callback.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_set_capture_done_cb(poom_edge_impulse_capture_done_cb_t callback, void* user_ctx)
{
    s_poom_edge_impulse.capture_done_cb = callback;
    s_poom_edge_impulse.capture_done_user_ctx = user_ctx;
    return ESP_OK;
}

/**
 * @brief Reads module state flags.
 *
 * @param[out] out_initialized True when module has been started.
 * @param[out] out_wifi_connected True when STA has a valid IP.
 * @param[out] out_capture_running True when capture task is active.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_get_state(bool* out_initialized,
                                      bool* out_wifi_connected,
                                      bool* out_capture_running)
{
    if((out_initialized == NULL) || (out_wifi_connected == NULL) || (out_capture_running == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_initialized = s_poom_edge_impulse.initialized;
    *out_wifi_connected = s_poom_edge_impulse.wifi_connected;
    *out_capture_running = (s_poom_edge_impulse.capture_task != NULL);

    return ESP_OK;
}
