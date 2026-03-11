// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_tracker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "poom_ble_gatt_client.h"

#if POOM_BLE_TRACKER_ENABLE_LOG
    static const char *POOM_BLE_TRACKER_TAG = "poom_ble_tracker";

    #define POOM_BLE_TRACKER_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_TRACKER_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_BLE_TRACKER_DEBUG_LOG_ENABLED
        #define POOM_BLE_TRACKER_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_BLE_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_BLE_TRACKER_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_BLE_TRACKER_PRINTF_E(...) do { } while (0)
    #define POOM_BLE_TRACKER_PRINTF_I(...) do { } while (0)
    #define POOM_BLE_TRACKER_PRINTF_D(...) do { } while (0)
#endif

#define POOM_BLE_TRACKER_SCAN_DURATION_SECONDS (60)
#define POOM_BLE_TRACKER_RSSI_AT_1M_DBM (-65)
#define POOM_BLE_TRACKER_PATH_LOSS_EXPONENT (2.2f)
#define POOM_BLE_TRACKER_DISTANCE_SCALE (10.0f)

typedef struct {
    uint8_t adv_cmp[4];
    const char *name;
    const char *vendor;
} poom_ble_tracker_signature_t;

static TaskHandle_t s_scan_timer_task = NULL;
static poom_ble_tracker_scan_cb_t s_scan_callback = NULL;
static int s_scan_elapsed_seconds = 0;
static bool s_scanner_active = false;

static const poom_ble_tracker_signature_t s_tracker_signatures[] = {
    {.adv_cmp = {0x1E, 0xFF, 0x4C, 0x00}, .name = "AirTag", .vendor = "Apple"},
    {.adv_cmp = {0x4C, 0x00, 0x12, 0x19}, .name = "UATag", .vendor = "Apple"},
    {.adv_cmp = {0x02, 0x01, 0x06, 0x0D}, .name = "Tile", .vendor = "Tile"},
};

/**
 * @brief Estimate tracker distance from RSSI value.
 * @param[in/out] rssi_dbm Averaged RSSI in dBm.
 * @return esp_err_t
 */
static float poom_ble_tracker_estimate_distance_m_(float rssi_dbm) {
    float exponent = (POOM_BLE_TRACKER_RSSI_AT_1M_DBM - rssi_dbm) /
                     (10.0f * POOM_BLE_TRACKER_PATH_LOSS_EXPONENT);
    return powf(10.0f, exponent) / POOM_BLE_TRACKER_DISTANCE_SCALE;
}

/**
 * @brief Match advertisement data against known tracker signatures.
 * @param[in/out] adv_data Pointer to advertisement payload.
 * @param[in/out] adv_len Advertisement payload length.
 * @param[in/out] out_signature Pointer to matched signature output.
 * @return esp_err_t
 */
static bool poom_ble_tracker_match_signature_(const uint8_t *adv_data,
                                              size_t adv_len,
                                              const poom_ble_tracker_signature_t **out_signature) {
    size_t i;

    if ((adv_data == NULL) || (out_signature == NULL) || (adv_len < 4U)) {
        return false;
    }

    for (i = 0U; i < (sizeof(s_tracker_signatures) / sizeof(s_tracker_signatures[0])); i++) {
        if ((adv_data[0] == s_tracker_signatures[i].adv_cmp[0]) &&
            (adv_data[1] == s_tracker_signatures[i].adv_cmp[1]) &&
            (adv_data[2] == s_tracker_signatures[i].adv_cmp[2]) &&
            (adv_data[3] == s_tracker_signatures[i].adv_cmp[3])) {
            *out_signature = &s_tracker_signatures[i];
            return true;
        }
    }

    return false;
}

/**
 * @brief Parse one scan result and fill tracker record when matched.
 * @param[in/out] scan_result Pointer to BLE scan result.
 * @param[in/out] tracker_record Pointer to tracker record output.
 * @return esp_err_t
 */
static void poom_ble_tracker_parse_adv_record_(const esp_ble_gap_cb_param_t *scan_result,
                                               poom_ble_tracker_profile_t *tracker_record) {
    const poom_ble_tracker_signature_t *signature = NULL;
    uint8_t copy_len;

    if ((scan_result == NULL) || (tracker_record == NULL)) {
        return;
    }

    if (!poom_ble_tracker_match_signature_(scan_result->scan_rst.ble_adv,
                                           (size_t)scan_result->scan_rst.adv_data_len,
                                           &signature)) {
        return;
    }

    tracker_record->is_tracker = true;
    tracker_record->name = signature->name;
    tracker_record->vendor = signature->vendor;
    tracker_record->rssi = scan_result->scan_rst.rssi;

    copy_len = scan_result->scan_rst.adv_data_len;
    if (copy_len > sizeof(tracker_record->adv_data)) {
        copy_len = sizeof(tracker_record->adv_data);
    }

    tracker_record->adv_data_length = copy_len;
    memcpy(tracker_record->mac_address, scan_result->scan_rst.bda, sizeof(tracker_record->mac_address));
    memcpy(tracker_record->adv_data, scan_result->scan_rst.ble_adv, copy_len);

    tracker_record->distance_m = poom_ble_tracker_estimate_distance_m_((float)tracker_record->rssi);

    POOM_BLE_TRACKER_PRINTF_I("Tracker found: %s (%s), RSSI=%d dBm, distance=%.2f m",
                              tracker_record->name,
                              tracker_record->vendor,
                              tracker_record->rssi,
                              tracker_record->distance_m);
}

/**
 * @brief Handle BLE GAP events and dispatch tracker records.
 * @param[in/out] event_type GAP event type.
 * @param[in/out] param GAP event payload.
 * @return esp_err_t
 */
static void poom_ble_tracker_handle_gap_event_(esp_gap_ble_cb_event_t event_type,
                                                esp_ble_gap_cb_param_t *param) {
    if (event_type != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        return;
    }

    if ((param == NULL) || !s_scanner_active) {
        return;
    }

    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }

    if (param->scan_rst.adv_data_len == 0U) {
        return;
    }

    {
        poom_ble_tracker_profile_t tracker_record = {
            .rssi = 0,
            .name = "",
            .vendor = "",
            .mac_address = {0},
            .adv_data = {0},
            .adv_data_length = 0,
            .is_tracker = false,
            .distance_m = -1.0f,
        };

        poom_ble_tracker_parse_adv_record_(param, &tracker_record);

        if (tracker_record.is_tracker && (s_scan_callback != NULL)) {
            s_scan_callback(tracker_record);
        }
    }
}

/**
 * @brief Run scan duration timer and stop scanner on timeout.
 * @param[in/out] arg Unused task argument.
 * @return esp_err_t
 */
static void poom_ble_tracker_timer_task_(void *arg) {
    (void)arg;

    POOM_BLE_TRACKER_PRINTF_I("Scanner timer task started");
    s_scan_elapsed_seconds = 0;

    while (s_scanner_active) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
        s_scan_elapsed_seconds++;

        if (s_scan_elapsed_seconds >= POOM_BLE_TRACKER_SCAN_DURATION_SECONDS) {
            POOM_BLE_TRACKER_PRINTF_I("Scan duration reached");
            poom_ble_tracker_stop();
            return;
        }
    }

    s_scan_timer_task = NULL;
    POOM_BLE_TRACKER_PRINTF_I("Scanner timer task stopped");
    vTaskDelete(NULL);
}

/**
 * @brief Register scan callback to receive tracker records.
 * @param[in/out] cb Callback function pointer.
 * @return esp_err_t
 */
void poom_ble_tracker_register_scan_callback(poom_ble_tracker_scan_cb_t cb) {
    s_scan_callback = cb;
}

/**
 * @brief Start BLE tracker scanning pipeline.
 * @return esp_err_t
 */
void poom_ble_tracker_start(void) {
    poom_ble_gatt_client_scan_params_t scan_params;
    poom_ble_gatt_client_event_cb_t event_cb;

    if (s_scanner_active) {
        return;
    }

    scan_params.remote_filter_service_uuid = poom_ble_gatt_client_default_service_uuid();
    scan_params.remote_filter_char_uuid = poom_ble_gatt_client_default_char_uuid();
    scan_params.notify_descr_uuid = poom_ble_gatt_client_default_notify_descr_uuid();
    scan_params.ble_scan_params = poom_ble_gatt_client_default_scan_params();

    poom_ble_gatt_client_set_scan_params(&scan_params);

    event_cb.handler_client_cb = NULL;
    event_cb.handler_gap_cb = poom_ble_tracker_handle_gap_event_;
    poom_ble_gatt_client_set_callbacks(event_cb);

    poom_ble_gatt_client_start();

    s_scanner_active = true;
    s_scan_elapsed_seconds = 0;

    if (s_scan_timer_task == NULL) {
        if (xTaskCreate(poom_ble_tracker_timer_task_,
                        "poom_ble_tracker_task",
                        4096,
                        NULL,
                        5,
                        &s_scan_timer_task) != pdPASS) {
            s_scanner_active = false;
            poom_ble_gatt_client_stop();
            POOM_BLE_TRACKER_PRINTF_E("Failed to create scanner timer task");
            return;
        }
    }

    POOM_BLE_TRACKER_PRINTF_I("Scanner started");
}

/**
 * @brief Stop BLE tracker scanning pipeline.
 * @return esp_err_t
 */
void poom_ble_tracker_stop(void) {
    TaskHandle_t current_task;

    s_scanner_active = false;
    s_scan_elapsed_seconds = 0;

    poom_ble_gatt_client_stop();

    if (s_scan_timer_task != NULL) {
        current_task = xTaskGetCurrentTaskHandle();
        if (current_task == s_scan_timer_task) {
            s_scan_timer_task = NULL;
            POOM_BLE_TRACKER_PRINTF_I("Scanner stopped from timer task");
            vTaskDelete(NULL);
            return;
        }

        vTaskDelete(s_scan_timer_task);
        s_scan_timer_task = NULL;
    }

    POOM_BLE_TRACKER_PRINTF_I("Scanner stopped");
}

/**
 * @brief Check if BLE tracker scanner is active.
 * @return esp_err_t
 */
bool poom_ble_tracker_is_active(void) {
    return s_scanner_active;
}

/**
 * @brief Append one tracker profile to a dynamic list.
 * @param[in/out] profiles Pointer to dynamic profile array pointer.
 * @param[in/out] num_profiles Pointer to current profile count.
 * @param[in/out] new_profile Profile to append.
 * @return esp_err_t
 */
void poom_ble_tracker_add_profile(poom_ble_tracker_profile_t **profiles,
                                  uint16_t *num_profiles,
                                  poom_ble_tracker_profile_t new_profile) {
    poom_ble_tracker_profile_t *new_profiles;

    if ((profiles == NULL) || (num_profiles == NULL)) {
        return;
    }

    new_profiles = realloc(*profiles, (size_t)(*num_profiles + 1U) * sizeof(poom_ble_tracker_profile_t));
    if (new_profiles == NULL) {
        return;
    }

    *profiles = new_profiles;
    (*profiles)[*num_profiles] = new_profile;
    (*num_profiles)++;
}

/**
 * @brief Find tracker profile index by MAC address.
 * @param[in/out] profiles Pointer to profile array.
 * @param[in/out] num_profiles Number of profiles in the array.
 * @param[in/out] mac_address Pointer to 6-byte MAC address.
 * @return esp_err_t
 */
int poom_ble_tracker_find_profile_by_mac(const poom_ble_tracker_profile_t *profiles,
                                         uint16_t num_profiles,
                                         const uint8_t mac_address[6]) {
    uint16_t i;

    if ((profiles == NULL) || (mac_address == NULL)) {
        return -1;
    }

    for (i = 0U; i < num_profiles; i++) {
        if (memcmp(profiles[i].mac_address, mac_address, 6U) == 0) {
            return (int)i;
        }
    }

    return -1;
}
