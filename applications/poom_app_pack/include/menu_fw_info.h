// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_FW_INFO_H
#define MENU_FW_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows firmware info screen (version/build/OTA slots).
 *
 * Exits back to the main menu on B.
 */
void menu_fw_info_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_FW_INFO_H */
