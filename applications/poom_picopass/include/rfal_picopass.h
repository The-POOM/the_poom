// SPDX-License-Identifier: GPL-3.0-or-later
// PicoPass (HID iCLASS) low-level poller for POOM, built on ST's RFAL.
//
// iCLASS/PicoPass uses the ISO15693 physical layer but a proprietary
// anticollision. The ST25R3916 supports it via RFAL_MODE_POLL_PICOPASS at
// 26.48 kbit/s with a manual TX CRC.

#pragma once

#include <stdint.h>

// RFAL provides ReturnCode + rfal* API. When building inside the_poom this
// resolves to modules/nfc/include/rfal/rfal_rf.h. Kept as a single include
// point so the dependency is obvious.
#include "rfal_rf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFAL_PICOPASS_UID_LEN       8
#define RFAL_PICOPASS_MAX_BLOCK_LEN 8

// ISO13239 CRC preset used by PicoPass.
#define RFAL_PICOPASS_CRC_PRELOAD 0xE012

// PicoPass commands (iCLASS). Note READ and IDENTIFY share opcode 0x0C; they
// differ by argument length: IDENTIFY is bare 0x0C, READ is 0x0C + block + CRC.
enum
{
    RFAL_PICOPASS_CMD_ACTALL       = 0x0A,
    RFAL_PICOPASS_CMD_IDENTIFY     = 0x0C,
    RFAL_PICOPASS_CMD_SELECT       = 0x81,
    RFAL_PICOPASS_CMD_READCHECK_KD = 0x88,  // read-check with debit key (Kd)
    RFAL_PICOPASS_CMD_READCHECK_KC = 0x18,  // read-check with credit key (Kc)
    RFAL_PICOPASS_CMD_CHECK        = 0x05,
    RFAL_PICOPASS_CMD_READ         = 0x0C,
    RFAL_PICOPASS_CMD_READ4        = 0x06,
    RFAL_PICOPASS_CMD_WRITE        = 0x87,
};

typedef struct
{
    uint8_t CSN[RFAL_PICOPASS_UID_LEN];  // anticollision CSN
    uint8_t crc[2];
} rfalPicoPassIdentifyRes;

typedef struct
{
    uint8_t CSN[RFAL_PICOPASS_UID_LEN];  // real CSN
    uint8_t crc[2];
} rfalPicoPassSelectRes;

typedef struct
{
    uint8_t CCNR[8];  // e-purse challenge, used as the MAC nonce
} rfalPicoPassReadCheckRes;

typedef struct
{
    uint8_t mac[4];
} rfalPicoPassCheckRes;

typedef struct
{
    uint8_t data[RFAL_PICOPASS_MAX_BLOCK_LEN];
    uint8_t crc[2];
} rfalPicoPassReadBlockRes;

// Bring the ST25R3916 into PicoPass mode and turn the field on.
ReturnCode rfalPicoPassPollerInitialize(void);

// ACTALL wake-up. The card answers with just an SOF, so
// RFAL_ERR_INCOMPLETE_BYTE (or a timeout) is the normal, expected result here.
ReturnCode rfalPicoPassPollerCheckPresence(void);

// Anticollision: read the CSN.
ReturnCode rfalPicoPassPollerIdentify(rfalPicoPassIdentifyRes* idRes);

// Select the card by CSN; returns the "real" CSN.
ReturnCode rfalPicoPassPollerSelect(const uint8_t* csn,
                                    rfalPicoPassSelectRes* selRes);

// Read-check: fetch the e-purse (CCNR) that seeds the authentication MAC.
// key_block_num is typically 2 (e-purse). use_credit selects Kc (0x18) vs Kd
// (0x88).
ReturnCode rfalPicoPassPollerReadCheck(rfalPicoPassReadCheckRes* rcRes,
                                       uint8_t key_block_num,
                                       int use_credit);

// Authenticate: send the computed 4-byte MAC.
ReturnCode rfalPicoPassPollerCheck(const uint8_t* mac,
                                   rfalPicoPassCheckRes* chkRes);

// Read one 8-byte block (CRC appended here).
ReturnCode rfalPicoPassPollerReadBlock(uint8_t blockNum,
                                       rfalPicoPassReadBlockRes* readRes);

// Write one 8-byte block with its 4-byte MAC (no CRC; MAC is the integrity check).
ReturnCode rfalPicoPassPollerWriteBlock(uint8_t blockNum,
                                        const uint8_t data[8],
                                        const uint8_t mac[4]);

// ISO13239 CRC over `buf` with the PicoPass preset.
uint16_t rfalPicoPassCrc16(const uint8_t* buf, uint16_t length);

#ifdef __cplusplus
}
#endif
