#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_154_recon.h
 * @brief Active IEEE 802.15.4 / Thread network recon on top of the
 *        integrated OpenThread stack.
 *
 * Surfaces the OpenThread scan/discovery primitives that POOM does not
 * otherwise expose:
 *   - otLinkActiveScan()  -> enumerate networks via 802.15.4 beacon requests
 *                            (Zigbee + beacon-payload Thread).
 *   - otThreadDiscover()  -> MLE Discovery; finds Thread / Matter networks
 *                            that do not answer legacy beacon requests.
 *   - otLinkEnergyScan()  -> per-channel noise floor (max RSSI).
 *
 * Unlike the passive pcap sniffer (cfg-154-*), the active scan/discovery
 * TRANSMIT, so they find quiet networks a passive capture never sees. The
 * radio is shared: stop the pcap sniffer (cfg-154-stop) before scanning.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Lowest / highest 2.4 GHz O-QPSK channel. */
#define POOM_154_RECON_CHANNEL_MIN (11U)
#define POOM_154_RECON_CHANNEL_MAX (26U)

/** Channel mask covering 11..26 (all 2.4 GHz channels). */
#define POOM_154_RECON_CHANNELS_ALL (0x07FFF800U)

/** Default per-channel dwell (ms) used when the caller passes 0. */
#define POOM_154_RECON_DEFAULT_DWELL_MS (300U)

/** Max networks cached from one scan/discovery. */
#define POOM_154_RECON_MAX_RESULTS (32U)

/** One cached result (flattened, printable copy of otActiveScanResult). */
typedef struct
{
    uint8_t  ext_address[8];     /**< IEEE 802.15.4 extended address. */
    uint8_t  ext_pan_id[8];      /**< Thread extended PAN ID. */
    char     network_name[17];   /**< Thread network name (NUL-terminated). */
    uint16_t pan_id;             /**< IEEE 802.15.4 PAN ID. */
    uint8_t  channel;            /**< Channel 11..26. */
    int8_t   rssi;               /**< RSSI in dBm. */
    uint8_t  lqi;                /**< Link quality indicator. */
    bool     is_joinable;        /**< Joining permitted (needs beacon-payload parsing). */
    bool     is_discover;        /**< True if found via MLE Discovery (vs beacon scan). */
} poom_154_recon_net_t;

/**
 * @brief Bring the OpenThread stack up for scanning (idempotent).
 *
 * Call once per boot before any scan, like nfc-core-start for NFC.
 *
 * @return ESP_OK on success.
 */
esp_err_t poom_154_recon_init(void);

/**
 * @brief Run a blocking active (beacon-request) scan and cache the results.
 *
 * @param[in]  channel_mask    Bitmask of channels, or POOM_154_RECON_CHANNELS_ALL.
 * @param[in]  per_channel_ms  Dwell per channel in ms (0 = default dwell).
 * @param[out] out_count       Number of networks cached (may be NULL).
 * @return ESP_OK, ESP_ERR_INVALID_STATE if not initialized, ESP_ERR_TIMEOUT.
 */
esp_err_t poom_154_recon_active_scan(uint32_t channel_mask,
                                     uint16_t per_channel_ms,
                                     uint8_t* out_count);

/**
 * @brief Run a blocking MLE Discovery and cache the results.
 *
 * Finds Thread / Matter networks that do not respond to legacy beacon
 * requests. Results reuse the same cache as the active scan.
 *
 * @param[in]  channel_mask    Bitmask of channels, or POOM_154_RECON_CHANNELS_ALL.
 * @param[in]  per_channel_ms  Dwell per channel in ms (0 = default dwell).
 * @param[out] out_count       Number of networks cached (may be NULL).
 * @return ESP_OK, ESP_ERR_INVALID_STATE if not initialized, ESP_ERR_TIMEOUT.
 */
esp_err_t poom_154_recon_discover(uint32_t channel_mask,
                                  uint16_t per_channel_ms,
                                  uint8_t* out_count);

/**
 * @brief Run a blocking energy (noise-floor) scan, printing each channel.
 *
 * @param[in] channel_mask    Bitmask of channels, or POOM_154_RECON_CHANNELS_ALL.
 * @param[in] per_channel_ms  Dwell per channel in ms (0 = default dwell).
 * @return ESP_OK on success.
 */
esp_err_t poom_154_recon_energy_scan(uint32_t channel_mask, uint16_t per_channel_ms);

/**
 * @brief Access a cached result by index.
 */
bool poom_154_recon_get_result(uint8_t index, poom_154_recon_net_t* out);

/**
 * @brief Number of networks cached by the last scan/discovery.
 */
uint8_t poom_154_recon_result_count(void);

/**
 * @brief Convenience: build a single-channel mask (11..26).
 */
static inline uint32_t poom_154_recon_channel_mask(uint8_t channel)
{
    return (uint32_t)(1UL << channel);
}

#ifdef __cplusplus
}
#endif
