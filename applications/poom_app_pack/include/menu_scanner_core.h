// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_SCANNER_CORE_H
#define MENU_SCANNER_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scanner core UI: select mode (Wi-Fi / IEEE 802.15.4) then render
 * a live channel-occupancy histogram.
 */
void menu_scanner_core_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_SCANNER_CORE_H */
