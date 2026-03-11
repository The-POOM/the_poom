// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_TRACKER_H
#define POOM_BLE_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Store one BLE tracker report.
 */
typedef struct {
    int rssi;
    const char *name;
    const char *vendor;
    uint8_t mac_address[6];
    uint8_t adv_data[31];
    uint8_t adv_data_length;
    bool is_tracker;
    float distance_m;
} poom_ble_tracker_profile_t;

/**
 * @brief Define tracker scan callback type.
 */
typedef void (*poom_ble_tracker_scan_cb_t)(poom_ble_tracker_profile_t record);

/**
 * @brief Register scan callback to receive tracker records.
 * @param[in/out] cb Callback function pointer.
 * @return esp_err_t
 */
void poom_ble_tracker_register_scan_callback(poom_ble_tracker_scan_cb_t cb);

/**
 * @brief Start BLE tracker scanning pipeline.
 * @return esp_err_t
 */
void poom_ble_tracker_start(void);

/**
 * @brief Stop BLE tracker scanning pipeline.
 * @return esp_err_t
 */
void poom_ble_tracker_stop(void);

/**
 * @brief Check if BLE tracker scanner is active.
 * @return esp_err_t
 */
bool poom_ble_tracker_is_active(void);

/**
 * @brief Append one tracker profile to a dynamic list.
 * @param[in/out] profiles Pointer to dynamic profile array pointer.
 * @param[in/out] num_profiles Pointer to current profile count.
 * @param[in/out] new_profile Profile to append.
 * @return esp_err_t
 */
void poom_ble_tracker_add_profile(poom_ble_tracker_profile_t **profiles,
                                  uint16_t *num_profiles,
                                  poom_ble_tracker_profile_t new_profile);

/**
 * @brief Find tracker profile index by MAC address.
 * @param[in/out] profiles Pointer to profile array.
 * @param[in/out] num_profiles Number of profiles in the array.
 * @param[in/out] mac_address Pointer to 6-byte MAC address.
 * @return esp_err_t
 */
int poom_ble_tracker_find_profile_by_mac(const poom_ble_tracker_profile_t *profiles,
                                         uint16_t num_profiles,
                                         const uint8_t mac_address[6]);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_TRACKER_H */
