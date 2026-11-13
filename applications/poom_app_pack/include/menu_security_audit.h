// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file menu_security_audit.h
 * @brief Security Audit app (Nmap-style) screen flow for the POOM menu.
 *
 * Standalone app: connects Wi-Fi via the shared saved-profiles store, discovers
 * LAN hosts (ICMP + ARP) and runs TCP port scans, all on top of the headless
 * poom_security_audit engine. Independent of Wi-Fi Detect and the ARP "SCAN NET"
 * app; it does not alter their behaviour.
 */

#ifndef MENU_SECURITY_AUDIT_H
#define MENU_SECURITY_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Enter the Security Audit app (blocking screen flow on its task). */
void menu_security_audit_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_SECURITY_AUDIT_H */
