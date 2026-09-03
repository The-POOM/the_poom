// SPDX-License-Identifier: GPL-3.0-or-later
// High-level PicoPass (HID iCLASS) read flow for POOM.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_PICOPASS_BLOCK_LEN      8
#define POOM_PICOPASS_MAX_APP_BLOCKS 32  // enough for 2KS/16KS AA1 dumps
#define POOM_PICOPASS_MAX_WIEGAND    2  // 37-bit matches both H10302 and H10304
// Max save-name length: name (22) + ".picopass" (9) = 31, FATFS's exact limit.
#define POOM_PICOPASS_NAME_MAX       22

// iCLASS block indices (AA1).
#define POOM_PICOPASS_CSN_BLOCK      0
#define POOM_PICOPASS_CONFIG_BLOCK   1
#define POOM_PICOPASS_EPURSE_BLOCK   2
#define POOM_PICOPASS_KD_BLOCK       3
#define POOM_PICOPASS_KC_BLOCK       4
#define POOM_PICOPASS_AIA_BLOCK      5
#define POOM_PICOPASS_PACS_CFG_BLOCK 6

typedef enum
{
    PoomPicopassOk = 0,
    PoomPicopassErrNoCard,
    PoomPicopassErrIdentify,
    PoomPicopassErrSelect,
    PoomPicopassErrReadCheck,
    PoomPicopassErrAuth,
    PoomPicopassErrRead,
    PoomPicopassErrInit,
} PoomPicopassStatus;

typedef struct
{
    uint8_t csn[POOM_PICOPASS_BLOCK_LEN];     // real CSN (block 0)
    uint8_t config[POOM_PICOPASS_BLOCK_LEN];  // block 1
    uint8_t epurse[POOM_PICOPASS_BLOCK_LEN];  // block 2
    uint8_t aia[POOM_PICOPASS_BLOCK_LEN];  // block 5 (application issuer area)
    uint8_t div_key[POOM_PICOPASS_BLOCK_LEN];  // diversified key used for auth

    // Key blocks 3 (Kd) and 4 (Kc), as read back post-auth. Cards mask these,
    // but they are captured for a faithful dump.
    uint8_t kd[POOM_PICOPASS_BLOCK_LEN];
    uint8_t kc[POOM_PICOPASS_BLOCK_LEN];
    bool kd_valid;
    bool kc_valid;

    bool authenticated;
    uint8_t app_block_count;  // number of valid entries in blocks[]
    uint8_t blocks[POOM_PICOPASS_MAX_APP_BLOCKS]
                  [POOM_PICOPASS_BLOCK_LEN];  // AA1 from block 6

    uint8_t app_limit;  // from config: last block of AA1

    // HID PACS credential (from blocks 6-9), when present.
    bool pacs_present;        // credential blocks were read
    uint8_t pacs_encryption;  // block6[7]: 0x14 none, 0x15 DES, 0x17 3DES
    uint8_t credential[POOM_PICOPASS_BLOCK_LEN];  // decrypted, sentinel removed
    uint8_t bit_length;                           // Wiegand bit length

    // Decoded Wiegand credential(s). A 37-bit length is ambiguous (H10302 and
    // H10304 share a parity scheme), so more than one format can validate.
    uint8_t wiegand_count;  // number of parity-valid matches in wiegand[]
    bool parity_error;      // a known length matched but none passed parity
    struct
    {
        const char* format;  // e.g. "H10301"
        uint32_t facility_code;
        uint64_t card_number;
    } wiegand[POOM_PICOPASS_MAX_WIEGAND];
} PoomPicopassDump;

// Read a standard-keyed iCLASS card into `out`.
// key: 8-byte iCLASS key. Pass NULL to use the well-known standard debit key.
// elite: true if `key` is an elite key (needs diversified keygen).
PoomPicopassStatus poom_picopass_read(PoomPicopassDump* out,
                                      const uint8_t* key,
                                      bool elite);

// Format `dump` as human-readable hex lines into `buf` (returns bytes written).
int poom_picopass_format(const PoomPicopassDump* dump, char* buf, int buf_len);

// Save `dump` as a Flipper-compatible .picopass file on the SD card under
// /picopass. `name` is the base filename (no extension); it is sanitized to
// FAT-safe characters and falls back to the CSN if empty. Writes the relative
// path into `out_rel_path` (if non-NULL). Returns ESP_OK or an ESP error code.
esp_err_t poom_picopass_save(const PoomPicopassDump* dump,
                             const char* name,
                             char* out_rel_path,
                             size_t out_rel_path_len);

// True if an SD card is mounted (mounting it if needed). Lets the UI hide the
// save option when there's no usable card. Mounts the card as a side effect.
bool poom_picopass_sd_ready(void);

// Well-known standard iCLASS debit key.
extern const uint8_t poom_picopass_standard_key[POOM_PICOPASS_BLOCK_LEN];

#ifdef __cplusplus
}
#endif
