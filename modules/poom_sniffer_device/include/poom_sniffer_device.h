// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_SNIFFER_DEVICE_H
#define POOM_SNIFFER_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    POOM_SNIFFER_DEVICE_FILTER_PROBE = 0,
    POOM_SNIFFER_DEVICE_FILTER_BEACON,
    POOM_SNIFFER_DEVICE_FILTER_MGMT,
    POOM_SNIFFER_DEVICE_FILTER_DATA,
    POOM_SNIFFER_DEVICE_FILTER_CTRL,
    POOM_SNIFFER_DEVICE_FILTER_ALL,
} poom_sniffer_device_filter_t;

typedef struct
{
    poom_sniffer_device_filter_t filter;
    uint8_t channel;           /* 0 = hop, otherwise fixed channel */
    bool save_to_sd;
    bool dedupe_enabled;
    bool attempt_time_sync;
    bool print_packet_summary;
    uint8_t raw_preview_len;
} poom_sniffer_device_config_t;

/**
 * @brief Fills config with default sniffer behavior used by the menu flow.
 * @param[out] out_cfg Output config pointer.
 * @return esp_err_t
 */
esp_err_t poom_sniffer_device_config_default(poom_sniffer_device_config_t *out_cfg);

/**
 * @brief Starts Wi-Fi sniffer runtime using an explicit configuration.
 * @param[in] cfg Sniffer configuration. Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_sniffer_device_start_ex(const poom_sniffer_device_config_t *cfg);

/**
 * @brief Starts Wi-Fi probe-request sniffer runtime.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_sniffer_device_start(void);

/**
 * @brief Stops Wi-Fi probe-request sniffer runtime.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_sniffer_device_stop(void);

/**
 * @brief Gets current sniffer running flag.
 * @param[in/out] out_running Output running state pointer.
 * @return esp_err_t
 */
esp_err_t poom_sniffer_device_get_running(bool *out_running);

#ifdef __cplusplus
}
#endif

#endif /* POOM_SNIFFER_DEVICE_H */
