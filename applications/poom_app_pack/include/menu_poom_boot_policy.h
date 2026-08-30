// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file menu_poom_boot_policy.h
 * @brief Visual menu for launching the current game or loading a new binary from the SD card.
 */

#ifndef MENU_POOM_BOOT_POLICY_H
#define MENU_POOM_BOOT_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows the game selector menu.
 *
 * @details This menu can launch the game installed in `ota_1` or open the SD browser
 *          to select and flash a new binary.
 */
void menu_poom_boot_policy_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_POOM_BOOT_POLICY_H */
