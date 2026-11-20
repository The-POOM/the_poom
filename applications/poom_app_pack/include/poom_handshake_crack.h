// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// On-device WPA handshake "quick crack" engine.
//
// Parses a saved hashcat-22000 file (as written by poom_pcap's handshake
// capture) and tries a small built-in list of the most common WPA-valid
// passphrases against every target line in it. This is a quick check -- like
// the BLEShark Nano's "Auto Crack" -- not a full dictionary attack.
//
// Supports both line kinds the capture emits:
//   WPA*01* : PMKID    -> PMKID = HMAC-SHA1(PMK, "PMK Name"|AP|STA)[:16]
//   WPA*02* : EAPOL/MIC-> derive PTK, MIC = HMAC(KCK, eapol) by key version
// where PMK = PBKDF2-HMAC-SHA1(passphrase, essid, 4096, 32), the slow part.
//
// Crypto is done with mbedtls (already linked for TLS). Only key versions 1
// (HMAC-MD5) and 2 (HMAC-SHA1) are handled; version 3 (AES-128-CMAC) lines are
// counted as skipped.

#ifndef POOM_HANDSHAKE_CRACK_H
#define POOM_HANDSHAKE_CRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of a crack run over one .22000 file. */
typedef struct
{
    bool     found;            /**< true if a passphrase matched a target.     */
    char     password[64];     /**< the matched passphrase (valid if found).   */
    uint32_t tried;            /**< passphrases actually tried.                */
    uint32_t total;            /**< size of the built-in wordlist.             */
    uint32_t elapsed_ms;       /**< wall time of the run.                      */
    uint16_t target_count;     /**< crackable targets parsed from the file.    */
    uint16_t pmkid_count;      /**< of which PMKID (WPA*01*) targets.          */
    uint16_t eapol_count;      /**< of which EAPOL (WPA*02*) targets.          */
    uint16_t skipped_count;    /**< lines skipped (malformed / keyver 3).      */
    char     essid[33];        /**< ESSID of the first target (for display).   */
    bool     cancelled;        /**< true if the run was cancelled early.       */
    bool     error;            /**< true if the file could not be used.        */
    char     error_msg[32];    /**< short reason when error is true.           */
} poom_hs_crack_result_t;

/**
 * @brief Progress callback, invoked by the engine as it works.
 *
 * @param[in] tried Passphrases tried so far.
 * @param[in] total Total passphrases in the wordlist.
 * @param[in] user  Opaque pointer passed to poom_hs_crack_run_file().
 */
typedef void (*poom_hs_crack_progress_fn)(uint32_t tried, uint32_t total, void *user);

/** How much of the keyspace to try in one run. */
typedef enum
{
    POOM_HS_SCOPE_QUICK = 0,  /**< base wordlist only (exact PSKs).            */
    POOM_HS_SCOPE_FULL,       /**< base + rule variations + ESSID-derived.     */
    POOM_HS_SCOPE_CONTINUE,   /**< the variations/ESSID only -- resumes a      */
                              /**< prior QUICK run without retrying the base.  */
} poom_hs_crack_scope_t;

/**
 * @brief Number of passphrases in the built-in quick-crack wordlist.
 *
 * @return The wordlist length.
 */
uint32_t poom_hs_crack_wordlist_count(void);

/**
 * @brief Number of candidate passphrases a QUICK run tries: the built-in
 *        wordlist expanded by the as-is / capitalize / "!" / cap+"!" rules.
 *        Target-independent (QUICK does not use ESSID-derived candidates).
 *
 * @return The QUICK candidate count.
 */
uint32_t poom_hs_crack_quick_count(void);

/**
 * @brief Peek a .22000 file's first AP identity without cracking it.
 *
 * Parses the file and returns the first crackable target's BSSID (AP MAC) and
 * ESSID. Used to key the crack journal (already-cracked / already-tried checks)
 * before deciding whether to run.
 *
 * @param[in]  path     Absolute path to the .22000 file.
 * @param[out] bssid    6-byte AP MAC (required).
 * @param[out] essid    NUL-terminated ESSID (optional, may be NULL).
 * @param[in]  essid_sz Size of @p essid.
 * @return ESP_OK if a target was found; ESP_FAIL if none; ESP_ERR_* on bad args
 *         or allocation failure.
 */
esp_err_t poom_hs_crack_peek(const char *path, uint8_t bssid[6],
                             char *essid, size_t essid_sz);

/**
 * @brief Parse a .22000 file and run the built-in wordlist against it.
 *
 * Runs synchronously in the caller's context (expected to be a dedicated
 * task). Stops as soon as a passphrase matches any target.
 *
 * @param[in]  path   Absolute path to the .22000 file.
 * @param[in]  scope  How much of the keyspace to try (see the enum).
 * @param[in]  cancel Optional flag polled between passphrases; set true to
 *                    abort early. May be NULL.
 * @param[in]  cb     Optional progress callback. May be NULL.
 * @param[in]  user   Opaque pointer handed back to @p cb.
 * @param[out] out    Result, always populated (check out->error).
 * @return ESP_OK when the run completed (found or exhausted/cancelled);
 *         an error code if @p path or @p out is invalid.
 */
esp_err_t poom_hs_crack_run_file(const char               *path,
                                 poom_hs_crack_scope_t      scope,
                                 const volatile bool       *cancel,
                                 poom_hs_crack_progress_fn  cb,
                                 void                      *user,
                                 poom_hs_crack_result_t    *out);

/**
 * @brief Parse a .22000 file and try every WPA-valid line of an SD-card
 *        wordlist against it, streamed line-by-line (words tried as-is).
 *
 * Same contract as poom_hs_crack_run_file(), but the candidates come from
 * @p wordlist_path instead of the built-in list. Lines are CR/LF-stripped and
 * filtered to the WPA 8..63 length range; out->total is the count of valid
 * lines (a fast pre-pass), out->tried advances as they are tried. Stops on the
 * first match, on @p cancel, or at end of file.
 *
 * @param[in]  hs_path       Absolute path to the .22000 file.
 * @param[in]  wordlist_path Absolute path to the wordlist file on the SD card.
 * @param[in]  cancel        Optional abort flag, polled between candidates.
 * @param[in]  cb            Optional progress callback. May be NULL.
 * @param[in]  user          Opaque pointer handed back to @p cb.
 * @param[out] out           Result, always populated (check out->error).
 * @return ESP_OK when the run completed; an error code if an argument is NULL.
 */
esp_err_t poom_hs_crack_run_file_wordlist(const char               *hs_path,
                                          const char               *wordlist_path,
                                          const volatile bool       *cancel,
                                          poom_hs_crack_progress_fn  cb,
                                          void                      *user,
                                          poom_hs_crack_result_t    *out);

#ifdef __cplusplus
}
#endif

#endif /* POOM_HANDSHAKE_CRACK_H */
