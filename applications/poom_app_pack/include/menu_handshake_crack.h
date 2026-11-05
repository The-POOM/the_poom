// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_HANDSHAKE_CRACK_H
#define MENU_HANDSHAKE_CRACK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opens the handshake cracker as a browser: lists the saved .22000
 *        files under /sdcard/pcaps/handshakes, pick one and crack it against
 *        the built-in common-passphrase list.
 * @return void
 */
void menu_handshake_crack_show(void);

/**
 * @brief Opens the handshake cracker directly on a specific .22000 file,
 *        skipping the browser (used by the post-capture "Crack" option).
 *
 * @param[in] path Absolute path to the .22000 file to crack.
 * @return void
 */
void menu_handshake_crack_show_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MENU_HANDSHAKE_CRACK_H */
