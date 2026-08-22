#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POOM_SCANNER_CORE_MODE_NONE = 0,
    POOM_SCANNER_CORE_MODE_WIFI,
    POOM_SCANNER_CORE_MODE_WIFI_2G = POOM_SCANNER_CORE_MODE_WIFI, /* legacy alias */
    POOM_SCANNER_CORE_MODE_IEEE802154,
} poom_scanner_core_mode_t;

#if defined(CONFIG_IDF_TARGET_ESP32C5)
  #define POOM_SCANNER_CORE_WIFI_CH_MIN (1U)
  #define POOM_SCANNER_CORE_WIFI_CH_MAX (165U)
  #define POOM_SCANNER_CORE_WIFI_CH_COUNT (38U)
#else
  #define POOM_SCANNER_CORE_WIFI_CH_MIN (1U)
  #define POOM_SCANNER_CORE_WIFI_CH_MAX (13U)
  #define POOM_SCANNER_CORE_WIFI_CH_COUNT (13U)
#endif

#define POOM_SCANNER_CORE_IEEE802154_CH_MIN (11U)
#define POOM_SCANNER_CORE_IEEE802154_CH_MAX (26U)
#define POOM_SCANNER_CORE_IEEE802154_CH_COUNT (16U)

typedef struct
{
    uint8_t channel_min;
    uint8_t channel_max;
    uint8_t current_channel;
    uint32_t hop_ms;

    uint8_t channel_count;
    uint8_t channels[POOM_SCANNER_CORE_WIFI_CH_COUNT];

    uint32_t packet_count[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    int16_t rssi_avg_dbm[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    int8_t rssi_max_dbm[POOM_SCANNER_CORE_WIFI_CH_COUNT];
} poom_scanner_core_wifi_stats_t;

typedef struct
{
    uint8_t channel_min;
    uint8_t channel_max;
    uint8_t current_channel;
    uint32_t hop_ms;

    uint32_t packet_count[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
    int16_t rssi_avg_dbm[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
    int8_t rssi_max_dbm[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
} poom_scanner_core_ieee802154_stats_t;

typedef struct
{
    uint8_t channel;
    uint32_t packet_count;
    uint8_t packet_pct;
    int16_t rssi_avg_dbm;
    int8_t rssi_max_dbm;
} poom_scanner_core_top_channel_t;

/**
 * @brief Reset internal counters (does not stop scanning).
 */
void poom_scanner_core_reset_stats(void);

/**
 * @brief Start scanning Wi-Fi channels using promiscuous RX.
 *
 * - ESP32-C6: scans 2.4GHz channels 1..13.
 * - ESP32-C5: scans 2.4GHz (1..13) + 5GHz common channels.
 *
 * Uses a FreeRTOS software timer to hop channels with `esp_wifi_set_channel()`.
 */
esp_err_t poom_scanner_core_start_wifi(uint32_t hop_ms);

/**
 * @brief Start scanning IEEE 802.15.4 channels 11..26 (Zigbee/Matter).
 *
 * Uses a FreeRTOS software timer to hop channels with
 * `esp_ieee802154_set_channel()`. Packets are processed in
 * `esp_ieee802154_receive_done()` to extract RSSI and update per-channel stats.
 */
esp_err_t poom_scanner_core_start_ieee802154(uint32_t hop_ms);

/**
 * @brief Stop scanning and release radio resources.
 */
esp_err_t poom_scanner_core_stop(void);

/**
 * @brief Return current running mode.
 */
poom_scanner_core_mode_t poom_scanner_core_get_mode(void);

/**
 * @brief Snapshot Wi-Fi stats (thread-safe copy).
 */
bool poom_scanner_core_get_wifi_stats(poom_scanner_core_wifi_stats_t* out);

/**
 * @brief Snapshot IEEE 802.15.4 stats (thread-safe copy).
 */
bool poom_scanner_core_get_ieee802154_stats(poom_scanner_core_ieee802154_stats_t* out);

/**
 * @brief Return the top Wi-Fi channels ordered by packet_count (descending).
 *
 * Only channels with packet_count > 0 are returned.
 *
 * @param[out] out_entries Output array to fill.
 * @param[in] out_len Capacity of @p out_entries.
 * @return Number of entries written (0..out_len).
 */
size_t poom_scanner_core_get_wifi_top_channels(poom_scanner_core_top_channel_t* out_entries, size_t out_len);

/**
 * @brief Return the top IEEE 802.15.4 channels ordered by packet_count (descending).
 *
 * Only channels with packet_count > 0 are returned.
 *
 * @param[out] out_entries Output array to fill.
 * @param[in] out_len Capacity of @p out_entries.
 * @return Number of entries written (0..out_len).
 */
size_t poom_scanner_core_get_ieee802154_top_channels(poom_scanner_core_top_channel_t* out_entries, size_t out_len);

#ifdef __cplusplus
}
#endif
