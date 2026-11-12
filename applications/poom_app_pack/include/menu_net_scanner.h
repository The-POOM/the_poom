// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file menu_net_scanner.h
 * @brief Network Scanner app (Nmap-style) screen flow for the POOM menu.
 *
 * Standalone app: connects Wi-Fi via the shared saved-profiles store, discovers
 * LAN hosts (ICMP + ARP) and runs TCP port scans, all on top of the headless
 * poom_net_scanner engine. Independent of Wi-Fi Detect and the ARP "SCAN NET"
 * app; it does not alter their behaviour.
 */

#ifndef MENU_NET_SCANNER_H
#define MENU_NET_SCANNER_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Enter the Network Scanner app (blocking screen flow on its task). */
void menu_net_scanner_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_NET_SCANNER_H */
