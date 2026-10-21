// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#ifndef MENU_WIFI_DETECTOR_H
#define MENU_WIFI_DETECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void menu_wifi_detector_show(void);

/**
 * @brief Opens WIFI DETECT resumed on a specific AP's client list.
 *
 * Starts in the AP CLIENTS view with @p ap_bssid already selected and its
 * passive client monitor running on @p channel, so control returns here from a
 * Fox Hunt that was launched off one of these clients.
 *
 * @param[in] ap_bssid BSSID of the AP whose clients to list.
 * @param[in] channel  Primary channel of that AP.
 * @return void
 */
void menu_wifi_detector_show_clients(const uint8_t ap_bssid[6], uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* MENU_WIFI_DETECTOR_H */
