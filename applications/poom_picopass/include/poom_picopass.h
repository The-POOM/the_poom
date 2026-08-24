// SPDX-License-Identifier: GPL-3.0-or-later
// High-level PicoPass (HID iCLASS) read flow for POOM.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_PICOPASS_BLOCK_LEN      8
#define POOM_PICOPASS_MAX_APP_BLOCKS 32  // enough for 2KS/16KS AA1 dumps
#define POOM_PICOPASS_MAX_WIEGAND    2  // 37-bit matches both H10302 and H10304

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

// Well-known standard iCLASS debit key.
extern const uint8_t poom_picopass_standard_key[POOM_PICOPASS_BLOCK_LEN];

#ifdef __cplusplus
}
#endif
