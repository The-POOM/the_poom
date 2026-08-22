// SPDX-License-Identifier: GPL-3.0-or-later
// PicoPass low-level poller on ST RFAL. See rfal_picopass.h.

#include "rfal_picopass.h"
#include <string.h>

// All PicoPass frames use a manual TX CRC (the app supplies CRC bytes, or none
// for the anticollision frames), keep the RX CRC so we can verify it, remove RX
// parity, and leave AGC on.
#define PICOPASS_TXRX_FLAGS                    \
    ((uint32_t)RFAL_TXRX_FLAGS_CRC_TX_MANUAL | \
     (uint32_t)RFAL_TXRX_FLAGS_AGC_ON |        \
     (uint32_t)RFAL_TXRX_FLAGS_PAR_RX_REMV |   \
     (uint32_t)RFAL_TXRX_FLAGS_CRC_RX_KEEP)

// Per-frame wait time. 20 ms is generous; PicoPass answers in well under that.
#define PICOPASS_FWT rfalConvMsTo1fc(20)

// Guard / frame-delay times for PicoPass, set explicitly to the known-good
// values from the original ST-RFAL port. TODO: prefer RFAL_GT_PICOPASS /
// RFAL_FDT_*_PICOPASS if nfcal ever exposes them.
#ifndef PICOPASS_GT
#define PICOPASS_GT rfalConvMsTo1fc(1)  // ~1 ms guard after field-on
#endif
#ifndef PICOPASS_FDT_LISTEN
#define PICOPASS_FDT_LISTEN 3400  // fc, min time before declaring RX timeout
#endif
#ifndef PICOPASS_FDT_POLL
#define PICOPASS_FDT_POLL 4096  // fc, min gap between our frames
#endif

uint16_t rfalPicoPassCrc16(const uint8_t* buf, uint16_t length)
{
    // ISO13239 CRC (CCITT variant) with the PicoPass preset (0xE012).
    uint16_t crc = RFAL_PICOPASS_CRC_PRELOAD;
    for(uint16_t i = 0; i < length; i++)
    {
        uint8_t dat = buf[i];
        dat ^= (uint8_t)(crc & 0xFFU);
        dat ^= (uint8_t)(dat << 4);
        crc = (uint16_t)((crc >> 8) ^ (((uint16_t)dat) << 8) ^
                         (((uint16_t)dat) << 3) ^ (((uint16_t)dat) >> 4));
    }
    return crc;
}

ReturnCode rfalPicoPassPollerInitialize(void)
{
    ReturnCode ret;

    ret = rfalSetMode(RFAL_MODE_POLL_PICOPASS, RFAL_BR_26p48, RFAL_BR_26p48);
    if(ret != RFAL_ERR_NONE)
    {
        return ret;
    }

    rfalSetErrorHandling(RFAL_ERRORHANDLING_NONE);
    rfalSetGT(PICOPASS_GT);
    rfalSetFDTListen(PICOPASS_FDT_LISTEN);
    rfalSetFDTPoll(PICOPASS_FDT_POLL);

    // Energize the field and wait out the guard time before the first command.
    return rfalFieldOnAndStartGT();
}

ReturnCode rfalPicoPassPollerCheckPresence(void)
{
    uint8_t txBuf[1]  = {RFAL_PICOPASS_CMD_ACTALL};
    uint8_t rxBuf[32] = {0};
    uint16_t recvLen  = 0;

    // ACTALL is answered by an SOF only; an incomplete/short frame means a card
    // is present. Caller treats INCOMPLETE_BYTE / TIMEOUT as "maybe present".
    return rfalTransceiveBlockingTxRx(txBuf, sizeof(txBuf), rxBuf,
                                      sizeof(rxBuf), &recvLen,
                                      PICOPASS_TXRX_FLAGS, PICOPASS_FWT);
}

ReturnCode rfalPicoPassPollerIdentify(rfalPicoPassIdentifyRes* idRes)
{
    uint8_t txBuf[1] = {RFAL_PICOPASS_CMD_IDENTIFY};
    uint16_t recvLen = 0;

    return rfalTransceiveBlockingTxRx(txBuf, sizeof(txBuf), (uint8_t*)idRes,
                                      sizeof(rfalPicoPassIdentifyRes), &recvLen,
                                      PICOPASS_TXRX_FLAGS, PICOPASS_FWT);
}

ReturnCode rfalPicoPassPollerSelect(const uint8_t* csn,
                                    rfalPicoPassSelectRes* selRes)
{
    uint8_t txBuf[1 + RFAL_PICOPASS_UID_LEN];
    uint16_t recvLen = 0;

    txBuf[0] = RFAL_PICOPASS_CMD_SELECT;
    memcpy(&txBuf[1], csn, RFAL_PICOPASS_UID_LEN);

    ReturnCode ret = rfalTransceiveBlockingTxRx(
        txBuf, sizeof(txBuf), (uint8_t*)selRes, sizeof(rfalPicoPassSelectRes),
        &recvLen, PICOPASS_TXRX_FLAGS, PICOPASS_FWT);

    // The original port tolerated a timeout here on some tags.
    if(ret == RFAL_ERR_TIMEOUT)
    {
        return RFAL_ERR_NONE;
    }
    return ret;
}

ReturnCode rfalPicoPassPollerReadCheck(rfalPicoPassReadCheckRes* rcRes,
                                       uint8_t key_block_num,
                                       int use_credit)
{
    uint8_t txBuf[2] = {use_credit ? RFAL_PICOPASS_CMD_READCHECK_KC
                                   : RFAL_PICOPASS_CMD_READCHECK_KD,
                        key_block_num};
    uint16_t recvLen = 0;

    ReturnCode ret = rfalTransceiveBlockingTxRx(
        txBuf, sizeof(txBuf), (uint8_t*)rcRes, sizeof(rfalPicoPassReadCheckRes),
        &recvLen, PICOPASS_TXRX_FLAGS, PICOPASS_FWT);

    // The read-check response has no CRC, so a "CRC error" here is expected/OK.
    if(ret == RFAL_ERR_CRC)
    {
        return RFAL_ERR_NONE;
    }
    return ret;
}

ReturnCode rfalPicoPassPollerCheck(const uint8_t* mac,
                                   rfalPicoPassCheckRes* chkRes)
{
    uint8_t txBuf[9];
    uint16_t recvLen = 0;

    txBuf[0] = RFAL_PICOPASS_CMD_CHECK;
    memset(&txBuf[1], 0, 4);  // 4 null bytes (challenge slot)
    memcpy(&txBuf[5], mac, 4);

    ReturnCode ret = rfalTransceiveBlockingTxRx(
        txBuf, sizeof(txBuf), (uint8_t*)chkRes, sizeof(rfalPicoPassCheckRes),
        &recvLen, PICOPASS_TXRX_FLAGS, PICOPASS_FWT);

    // Like read-check, the CHECK response (4-byte MAC) carries no CRC, so a
    // CRC error here is expected/OK.
    if(ret == RFAL_ERR_CRC)
    {
        return RFAL_ERR_NONE;
    }
    return ret;
}

ReturnCode rfalPicoPassPollerReadBlock(uint8_t blockNum,
                                       rfalPicoPassReadBlockRes* readRes)
{
    uint8_t txBuf[4] = {RFAL_PICOPASS_CMD_READ, blockNum, 0, 0};
    uint16_t recvLen = 0;

    uint16_t crc =
        rfalPicoPassCrc16(&txBuf[1], 1);  // CRC over the block number only
    txBuf[2] = (uint8_t)(crc & 0xFF);
    txBuf[3] = (uint8_t)(crc >> 8);

    return rfalTransceiveBlockingTxRx(txBuf, sizeof(txBuf), (uint8_t*)readRes,
                                      sizeof(rfalPicoPassReadBlockRes),
                                      &recvLen, PICOPASS_TXRX_FLAGS,
                                      PICOPASS_FWT);
}

ReturnCode rfalPicoPassPollerWriteBlock(uint8_t blockNum,
                                        const uint8_t data[8],
                                        const uint8_t mac[4])
{
    // Authenticated UPDATE carries NO CRC: the 4-byte MAC is the integrity
    // check. Frame is CMD + block + data[8] + mac[4] = 14 bytes (matches both
    // references).
    uint8_t txBuf[2 + 8 + 4];
    uint16_t recvLen = 0;
    rfalPicoPassReadBlockRes
        resp;  // card echoes the block; response ignored, success = no error

    txBuf[0] = RFAL_PICOPASS_CMD_WRITE;
    txBuf[1] = blockNum;
    memcpy(&txBuf[2], data, 8);
    memcpy(&txBuf[10], mac, 4);

    return rfalTransceiveBlockingTxRx(txBuf, sizeof(txBuf), (uint8_t*)&resp,
                                      sizeof(resp), &recvLen,
                                      PICOPASS_TXRX_FLAGS, PICOPASS_FWT);
}
