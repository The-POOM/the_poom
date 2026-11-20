// SPDX-License-Identifier: MIT
#pragma once

/*
 * poom_hs_transfer — "Export Handshake over Wi-Fi" HTTP server.
 *
 * A trimmed copy of the proven wardrive "Transfer over Wi-Fi" server
 * (modules/poom_wardrive/wardrive_transfer.c). After the device has joined a
 * trusted AP (see poom_hs_link), this starts a small esp_http_server on the
 * device's own IP so another machine on the same LAN can pull the captured
 * handshakes with a browser:
 *
 *   - a 10-digit access code (shown on the device) gates login; login is
 *     throttled (5 failures -> 60s lockout),
 *   - authenticated routes require a separate 128-bit session cookie (the short
 *     numeric code is never accepted for file access directly),
 *   - access expires 10 minutes after start,
 *   - the index lists every .22000 (and its paired .pcap) under
 *     /sdcard/pcaps/handshakes; downloads stream as octet-stream attachments.
 *
 * HTTP is unencrypted — intended for a trusted private network only.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the server. On success returns true and writes the NUL-terminated
 * 10-digit access code into token (buffer must hold at least 11 bytes; 33 is
 * used here for parity with the wardrive original). Fails if already running or
 * the HTTP server/code could not be created. */
bool poom_hs_transfer_start(char token[33]);

/* Stop the server and wipe the secrets. Safe to call when not running. */
void poom_hs_transfer_stop(void);

/* True once the running server's initial-connection window has lapsed with no
 * client having authenticated. After a client authenticates this stays false
 * (the export then runs until it is stopped). */
bool poom_hs_transfer_expired(void);

/* True once too many wrong access codes were entered. The export then rejects
 * all logins until it is stopped and restarted (no timed auto-unlock). */
bool poom_hs_transfer_locked(void);

#ifdef __cplusplus
}
#endif
