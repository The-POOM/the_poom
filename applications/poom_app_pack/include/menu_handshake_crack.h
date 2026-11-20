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

/**
 * @brief Register a one-shot callback invoked when the cracker exits, INSTEAD
 *        of publishing the menu-resume token.
 *
 * Lets the launcher (e.g. the Wi-Fi detector's Capture Handshake flow) take
 * control back when the user leaves the cracker, rather than dropping all the
 * way out to the main menu. The callback is cleared as it fires, so a normal
 * launch from the main menu (no callback set) still returns to the menu. Pass
 * cb = NULL to clear a pending callback. The callback runs in the cracker's
 * task context during teardown; it is responsible for re-entering the launcher.
 *
 * @param[in] cb  Callback, or NULL to clear.
 * @param[in] ctx Opaque context passed to @p cb.
 */
void menu_handshake_crack_set_exit_callback(void (*cb)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MENU_HANDSHAKE_CRACK_H */
