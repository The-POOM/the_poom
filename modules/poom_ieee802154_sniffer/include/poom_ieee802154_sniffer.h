// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_IEEE802154_SNIFFER_H
#define POOM_IEEE802154_SNIFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_IEEE802154_SNIFFER_CHANNEL_MIN (11U)
#define POOM_IEEE802154_SNIFFER_CHANNEL_MAX (26U)
#define POOM_IEEE802154_SNIFFER_CHANNEL_DEFAULT (15U)

/**
 * @brief Enables or disables UART forwarding of captured 802.15.4 frames.
 *
 * When enabled, each captured frame is exported through the host sniffer
 * transport with its real channel and RSSI metadata.
 */
void poom_ieee802154_sniffer_set_uart_forward_enabled(bool enabled);

/**
 * @brief Starts fixed-channel IEEE 802.15.4 sniffing.
 *
 * @param[in] channel Capture channel in the 11..26 range.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_ieee802154_sniffer_start(uint8_t channel);

/**
 * @brief Stops the active IEEE 802.15.4 sniffer.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_ieee802154_sniffer_stop(void);

/**
 * @brief Returns whether the IEEE 802.15.4 sniffer is active.
 */
bool poom_ieee802154_sniffer_is_active(void);

/**
 * @brief Returns the active fixed capture channel, or 0 when inactive.
 */
uint8_t poom_ieee802154_sniffer_get_channel(void);

/**
 * @brief Returns the most recent frame RSSI, or -127 when unavailable.
 */
int8_t poom_ieee802154_sniffer_get_recent_rssi(void);

/**
 * @brief Returns the number of captured frames since the last start.
 */
uint32_t poom_ieee802154_sniffer_get_packet_count(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_IEEE802154_SNIFFER_H */
