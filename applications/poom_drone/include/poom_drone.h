// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_DRONE_H
#define POOM_DRONE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "opendroneid.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stores parsed OpenDroneID telemetry fields for one UAV.
 */
typedef struct {
    uint8_t mac[6];
    uint8_t padding[1];
    int8_t rssi;
    char op_id[ODID_ID_SIZE + 1];
    char uav_id[ODID_ID_SIZE + 1];
    double lat_d;
    double long_d;
    double base_lat_d;
    double base_long_d;
    int altitude_msl;
    int height_agl;
    int speed;
    int heading;
    int speed_vertical;
    int altitude_pressure;
    int horizontal_accuracy;
    int vertical_accuracy;
    int baro_accuracy;
    int speed_accuracy;
    int timestamp;
    int status;
    int height_type;
    int operator_location_type;
    int classification_type;
    int area_count;
    int area_radius;
    int area_ceiling;
    int area_floor;
    int operator_altitude_geo;
    uint32_t system_timestamp;
    int operator_id_type;
    uint8_t ua_type;
    uint8_t auth_type;
    uint8_t auth_page;
    uint8_t auth_length;
    uint32_t auth_timestamp;
    char auth_data[ODID_AUTH_PAGE_NONZERO_DATA_SIZE + 1];
    uint8_t desc_type;
    char description[ODID_STR_SIZE + 1];
    uint16_t channel;
} poom_drone_uav_data_t;

typedef enum
{
    POOM_DRONE_SCAN_WIFI = (1u << 0),
    POOM_DRONE_SCAN_BLE  = (1u << 1),
} poom_drone_scan_mask_t;

typedef struct
{
    uint32_t scan_mask;          /* poom_drone_scan_mask_t bits */
    uint32_t wifi_phase_ms;      /* used when WIFI+BLE enabled */
    uint32_t ble_phase_ms;       /* used when WIFI+BLE enabled */
    uint32_t hop_interval_ms;    /* Wi-Fi channel hop period */
    bool enable_pcap_to_sd;      /* start Wi-Fi PCAP capture to SD */
    bool enable_cli_print;       /* printf() each decoded report */
} poom_drone_config_t;

typedef void (*poom_drone_report_cb_t)(const poom_drone_uav_data_t *report, void *user_ctx);

/**
 * @brief Fills a config struct with defaults.
 *
 * Default is Wi-Fi only, channel hopping enabled, no BLE, no PCAP, no CLI print.
 */
void poom_drone_config_default(poom_drone_config_t *out_cfg);

/**
 * @brief Register an optional callback invoked for each decoded report.
 *
 * The callback is invoked from the internal parser task context.
 */
void poom_drone_register_report_cb(poom_drone_report_cb_t cb, void *user_ctx);

/**
 * @brief Returns whether the scanner is currently running.
 */
bool poom_drone_is_running(void);

/**
 * @brief Starts the DroneID scanner runtime.
 *
 * @param[in/out] none No input parameter.
 * @return esp_err_t
 */
esp_err_t poom_drone_start(void);

/**
 * @brief Starts the DroneID scanner runtime with an explicit config.
 *
 * @param[in] cfg Optional config; NULL uses defaults.
 * @return esp_err_t
 */
esp_err_t poom_drone_start_ex(const poom_drone_config_t *cfg);

/**
 * @brief Stops the DroneID scanner runtime.
 *
 * @param[in/out] none No input parameter.
 * @return esp_err_t
 */
esp_err_t poom_drone_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_DRONE_H */
