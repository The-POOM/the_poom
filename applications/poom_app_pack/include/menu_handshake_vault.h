// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#ifndef MENU_HANDSHAKE_VAULT_H
#define MENU_HANDSHAKE_VAULT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opens Handshake Vault: a two-choice menu over the captured handshakes.
 *
 *  - CRACK HANDSHAKE  -> the existing on-device dictionary cracker
 *                        (menu_handshake_crack_show).
 *  - EXPORT HANDSHAKE -> joins a saved Wi-Fi profile and serves the captured
 *                        .22000 / .pcap files over HTTP so another device on
 *                        the LAN can download them (poom_hs_link + poom_hs_transfer).
 *
 * Runs as a self-contained app (own task + button subscription) and publishes
 * the menu-resume topic on exit, like the other THE BEAST apps.
 */
void menu_handshake_vault_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_HANDSHAKE_VAULT_H */
