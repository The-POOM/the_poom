// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_DEAUTH_DETECTOR_H
#define POOM_WIFI_DEAUTH_DETECTOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN (1U)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX (165U)
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT (38U)
#else
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX (13U)
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT (13U)
#endif
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_HOP_SENTINEL (255U)
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_HOP_SENTINEL_LEGACY (99U)
#define POOM_WIFI_DEAUTH_DETECTOR_PPS_WINDOW_MS (1000U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_MED_PPS (4U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_PPS (12U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_MED_BCAST_PPS (2U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_BCAST_PPS (5U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_MED_SRC_PPS (3U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_SRC_PPS (8U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_MULTI_VICTIM_PPS (5U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_MED_VICTIMS (1U)
#define POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_VICTIMS (2U)

typedef struct
{
    uint32_t deauth_total;
    uint32_t disassoc_total;
    uint32_t *deauth_by_channel;
    uint32_t *disassoc_by_channel;
    uint8_t current_channel;
    bool channel_hopping;
} poom_wifi_deauth_detector_stats_t;

typedef enum
{
    POOM_WIFI_DEAUTH_DETECTOR_ALERT_OK = 0,
    POOM_WIFI_DEAUTH_DETECTOR_ALERT_MED,
    POOM_WIFI_DEAUTH_DETECTOR_ALERT_HIGH,
} poom_wifi_deauth_detector_alert_t;

typedef struct
{
    uint32_t deauth_pps;
    uint32_t disassoc_pps;
    poom_wifi_deauth_detector_alert_t alert;

    uint8_t top_src[6];
    uint8_t top_dst[6];
    uint8_t top_bssid[6];
    uint8_t top_channel;
    uint32_t top_pps;
    uint16_t top_unique_victims;
    uint16_t top_reason;
    uint32_t top_reason_count;
    bool top_valid;

    uint32_t bad_len;
    uint32_t protected_frames;
    uint32_t weird_ds;
} poom_wifi_deauth_detector_report_t;

/**
 * @brief Starts passive deauth/disassoc frame detection in promiscuous mode.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_start(void);

/**
 * @brief Stops passive detection and disables promiscuous capture callback.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_stop(void);

/**
 * @brief Returns whether detector runtime is currently active.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_deauth_detector_is_running(void);

/**
 * @brief Reads detector runtime counters and mode/channel status.
 * @param[out] out_stats Output stats structure.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_get_stats(poom_wifi_deauth_detector_stats_t *out_stats);

/**
 * @brief Reads summarized detector report (pps/alert/top talker).
 * @param[out] out_report Output report structure.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_get_report(poom_wifi_deauth_detector_report_t *out_report);

/**
 * @brief Returns number of channels in the detector hop list.
 * @return uint8_t
 */
uint8_t poom_wifi_deauth_detector_channel_count(void);

/**
 * @brief Returns channel number for a detector channel-list index.
 * @param[in] index 0-based channel-list index.
 * @return uint8_t Channel number, or 0 when index is invalid.
 */
uint8_t poom_wifi_deauth_detector_channel_at(uint8_t index);

/**
 * @brief Returns channel-list index for a channel number.
 * @param[in] channel Wi-Fi channel number.
 * @return int8_t 0-based index, or -1 when unsupported.
 */
int8_t poom_wifi_deauth_detector_channel_index(uint8_t channel);

/**
 * @brief Switches detector to fixed-channel mode and sets channel.
 * @param[in] channel Supported Wi-Fi channel number.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_set_fixed_channel(uint8_t channel);

/**
 * @brief Enables/disables channel hopping mode at runtime.
 * @param[in] enabled true to hop channels, false for fixed channel (keeps current).
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_set_channel_hopping(bool enabled);

/**
 * @brief Clears detector counters while keeping current runtime state.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_reset_stats(void);

/**
 * @brief Prints current detector counters to console.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_print_stats(void);

/**
 * @brief Sets detector saved-channel value.
 * @param[in] saved_channel `255` or legacy `99` enables hopping; otherwise use the channel when supported.
 * @return void
 */
void poom_wifi_deauth_detector_set_saved_channel(uint8_t saved_channel);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_DEAUTH_DETECTOR_H */
