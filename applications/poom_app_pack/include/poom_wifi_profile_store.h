// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_wifi_profile_store.h
 * @brief Shared saved-Wi-Fi-profile model + NVS persistence.
 *
 * Both Settings > WIFI PROFILES (menu_wifi_profiles.c) and
 * THE BEAST > CONNECTION TEST (menu_conntest.c) read and write the same saved
 * networks through this store, so a profile saved in one appears in the other.
 *
 * The store is a thin library over a caller-owned array: it holds no global
 * state of its own, which keeps the callers' existing UI scaffolding unchanged
 * and makes the logic trivial to host-compile. Persistence uses the same NVS
 * record layout the profiles menu used before this was factored out
 * ("wifiprof/<slot>" record blobs + a "wifi_prof_n" count), so existing saved
 * profiles load unchanged.
 */

#ifndef POOM_WIFI_PROFILE_STORE_H
#define POOM_WIFI_PROFILE_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "esp_err.h"

/** @brief Maximum number of saved profiles. */
#define POOM_WIFI_PROFILE_MAX       (12U)
/** @brief SSID buffer capacity (32 chars + NUL). */
#define POOM_WIFI_PROFILE_SSID_CAP  (33U)
/** @brief Password buffer capacity (63 chars + NUL). */
#define POOM_WIFI_PROFILE_PASS_CAP  (64U)

/** @brief One saved network. */
typedef struct
{
    char ssid[POOM_WIFI_PROFILE_SSID_CAP];
    char pass[POOM_WIFI_PROFILE_PASS_CAP];
} poom_wifi_profile_t;

/**
 * @brief Loads all saved profiles from NVS into the caller's array.
 *
 * @param[out] arr       Destination array (must hold at least @p cap entries).
 * @param[in]  cap       Capacity of @p arr in entries.
 * @param[out] out_count Number of profiles loaded (0..cap). Must not be NULL.
 */
void poom_wifi_profile_store_load(poom_wifi_profile_t *arr, uint16_t cap,
                                  uint16_t *out_count);

/**
 * @brief Adds a new profile or overwrites the existing one with the same SSID,
 *        then persists the whole set.
 *
 * @param[in,out] arr   Caller's profile array.
 * @param[in,out] count Current profile count; incremented on a new add.
 * @param[in]     cap   Capacity of @p arr in entries.
 * @param[in]     ssid  SSID to add/update (NUL-terminated).
 * @param[in]     pass  Password (NULL or "" for open networks).
 * @return ESP_OK on success, ESP_ERR_NO_MEM when full, else a persistence error.
 */
esp_err_t poom_wifi_profile_store_add_or_update(poom_wifi_profile_t *arr,
                                                uint16_t *count, uint16_t cap,
                                                const char *ssid,
                                                const char *pass);

/**
 * @brief Deletes the profile at @p slot (shifting the rest down) and persists.
 *
 * @param[in,out] arr   Caller's profile array.
 * @param[in,out] count Current profile count; decremented on success.
 * @param[in]     slot  Index to delete.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG when @p slot is out of range.
 */
esp_err_t poom_wifi_profile_store_delete(poom_wifi_profile_t *arr,
                                         uint16_t *count, uint16_t slot);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_PROFILE_STORE_H */
