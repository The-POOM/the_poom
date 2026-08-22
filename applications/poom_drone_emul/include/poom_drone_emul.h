// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_DRONE_EMUL_H
#define POOM_DRONE_EMUL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t channel; /* 1..13 */
    char ssid[33];   /* 1..32 + NUL */
    double latitude_d;
    double longitude_d;
    uint8_t count; /* 1..16 */
    bool ble_enabled;
} poom_drone_emul_config_t;

void poom_drone_emul_config_default(poom_drone_emul_config_t *out_cfg);

bool poom_drone_emul_is_running(void);

esp_err_t poom_drone_emul_start(const poom_drone_emul_config_t *cfg);
esp_err_t poom_drone_emul_stop(void);

/**
 * @brief Updates emulated drone location (persists to NVS).
 *
 * @param[in] latitude_d Latitude in degrees [-90..90].
 * @param[in] longitude_d Longitude in degrees [-180..180].
 */
esp_err_t poom_drone_emul_set_location(double latitude_d, double longitude_d);

/**
 * @brief Reads current emulated location (from RAM).
 */
esp_err_t poom_drone_emul_get_location(double *out_latitude_d, double *out_longitude_d);

/**
 * @brief Updates number of emulated drones (persists to NVS).
 *
 * @param[in] count Number of drones to emulate [1..16].
 */
esp_err_t poom_drone_emul_set_count(uint8_t count);

/**
 * @brief Reads current emulated drone count (from RAM).
 */
esp_err_t poom_drone_emul_get_count(uint8_t *out_count);

/**
 * @brief Enables/disables BLE RemoteID advertising (persists to NVS).
 */
esp_err_t poom_drone_emul_set_ble_enabled(bool enabled);

/**
 * @brief Reads BLE enabled flag (from RAM).
 */
esp_err_t poom_drone_emul_get_ble_enabled(bool *out_enabled);

#ifdef __cplusplus
}
#endif

#endif /* POOM_DRONE_EMUL_H */
