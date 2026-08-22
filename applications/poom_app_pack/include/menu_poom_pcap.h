// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_POOM_PCAP_H
#define MENU_POOM_PCAP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows the PCAP sniffer menu (WiFi / BLE / Zigbee capture).
 *
 * Zigbee/WiFi prompt for a channel before capture start.
 * A starts capture, B goes back/exit. Capture output uses `poom_pcap_manager`
 * (SD when available, UART fallback otherwise).
 */
void menu_poom_pcap_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_POOM_PCAP_H */
