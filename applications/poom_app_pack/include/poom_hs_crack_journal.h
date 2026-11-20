// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Persistent crack journal for the WPA handshake cracker.
//
// Records, per access point (keyed by BSSID), which wordlists have been tried
// against it and the outcome, plus the recovered password once cracked. This
// lets the UI skip work already done (show a known password instantly, warn
// before repeating a list) and show which networks are already solved.
//
// Storage is twofold:
//   * a human-readable CSV on the SD card (full attempt history, PC-inspectable)
//       /sdcard/pcaps/handshakes/crack_journal.csv
//       columns: bssid,essid,list,outcome,password,epoch
//   * an NVS mirror of cracked results (survives SD removal; the fast lookup).
// Both degrade gracefully: if the SD or NVS is unavailable the others still work.

#ifndef POOM_HS_CRACK_JOURNAL_H
#define POOM_HS_CRACK_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of a (handshake, wordlist) attempt. */
typedef enum
{
    HS_JOURNAL_NONE = 0,  /**< never attempted                                */
    HS_JOURNAL_MISS,      /**< attempted, passphrase not found                */
    HS_JOURNAL_CRACK,     /**< cracked                                        */
} hs_journal_outcome_t;

/**
 * @brief Look up a stored cracked password for an AP (NVS first, then CSV).
 * @param[in]  bssid 6-byte AP MAC.
 * @param[out] pw    Buffer for the NUL-terminated password.
 * @param[in]  pwsz  Size of @p pw.
 * @return true if a cracked password is on record.
 */
bool poom_hs_journal_get_cracked(const uint8_t bssid[6], char *pw, size_t pwsz);

/**
 * @brief Outcome of the most recent attempt of (bssid, list_id).
 * @return HS_JOURNAL_NONE if never attempted.
 */
hs_journal_outcome_t poom_hs_journal_lookup(const uint8_t bssid[6],
                                            const char *list_id);

/**
 * @brief Record one attempt. @p password is stored only when outcome is CRACK.
 *        A HS_JOURNAL_NONE outcome is ignored.
 */
void poom_hs_journal_record(const uint8_t bssid[6], const char *essid,
                            const char *list_id, hs_journal_outcome_t outcome,
                            const char *password);

/** Callback for poom_hs_journal_iterate_cracked(). All strings are transient. */
typedef void (*poom_hs_journal_iter_fn)(const char *essid, const char *bssid_hex,
                                        const char *password, void *user);

/**
 * @brief Enumerate cracked entries (latest crack per BSSID) from the CSV.
 */
void poom_hs_journal_iterate_cracked(poom_hs_journal_iter_fn cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* POOM_HS_CRACK_JOURNAL_H */
