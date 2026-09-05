// SPDX-License-Identifier: GPL-3.0-or-later
// High-level PicoPass read flow. See poom_picopass.h.
//
// Flow (standard-keyed iCLASS tag):
//   init picopass mode -> ACTALL -> IDENTIFY (anticollision CSN) -> SELECT
//   -> read preauth blocks (CSN/config/epurse/AIA)
//   -> diversify key, READCHECK e-purse, compute reader MAC, CHECK (auth)
//   -> read AA1 application blocks (6..app_limit)

#include "poom_picopass.h"
#include "optimized_cipher.h"
#include "poom_des.h"
#include "poom_nfc_controller.h"  // poom_nfc_controller_start(): bring up RFAL/ST25R3916
#include "poom_picopass_elite_dict.h"
#include "poom_picopass_standard_dict.h"
#include "poom_wiegand.h"
#include "rfal_picopass.h"
#include <stdio.h>
#include <string.h>

// iCLASS transport key: 2-key 3DES that decrypts the credential blocks (7-9)
// on standard "3DES-encrypted" cards.
static const uint8_t iclass_3des_key[16] = {0xb4, 0x21, 0x2c, 0xca, 0xb7, 0xed,
                                            0x21, 0x0f, 0x7b, 0x93, 0xd4, 0x59,
                                            0x39, 0xc7, 0xdd, 0x36};

#define PICOPASS_ENC_NONE 0x14
#define PICOPASS_ENC_DES  0x15
#define PICOPASS_ENC_3DES 0x17

const uint8_t poom_picopass_standard_key[POOM_PICOPASS_BLOCK_LEN] = {
    0xaf, 0xa7, 0x85, 0xa7, 0xda, 0xb3, 0x33, 0x78};

// PicoPass factory default debit key (Kd). Cards still on this key carry no
// real PACS, so we flag them and skip credential decoding. The standard
// dictionary carries the same key (that entry does the auth); this copy exists
// only to recognize the card afterward.
static const uint8_t poom_picopass_factory_key[POOM_PICOPASS_BLOCK_LEN] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87};

static PoomPicopassStatus read_block(uint8_t idx,
                                     uint8_t out[POOM_PICOPASS_BLOCK_LEN])
{
    rfalPicoPassReadBlockRes res;
    if(rfalPicoPassPollerReadBlock(idx, &res) != RFAL_ERR_NONE)
    {
        return PoomPicopassErrRead;
    }
    memcpy(out, res.data, POOM_PICOPASS_BLOCK_LEN);
    return PoomPicopassOk;
}

// Decode the HID PACS credential from AA1 blocks 6-9 (out->blocks[0..3]).
static void poom_picopass_decode_pacs(PoomPicopassDump* out)
{
    if(out->app_block_count < 2)
        return;  // need blocks 6 (config) and 7 (credential)
    const uint8_t* blk6  = out->blocks[0];
    const uint8_t* blk7  = out->blocks[1];
    out->pacs_present    = true;
    out->pacs_encryption = blk6[7];

    if(out->pacs_encryption == PICOPASS_ENC_3DES)
    {
        mbedtls_des3_context ctx;
        mbedtls_des3_init(&ctx);
        mbedtls_des3_set2key_dec(&ctx, iclass_3des_key);
        mbedtls_des3_crypt_ecb(&ctx, blk7, out->credential);
        mbedtls_des3_free(&ctx);
    }
    else
    {
        // 3DES is the only encryption we decrypt. None (0x14), DES (0x15, not
        // yet supported), or unknown: use the raw block as-is.
        memcpy(out->credential, blk7, POOM_PICOPASS_BLOCK_LEN);
    }

    // Wiegand bit length = position of the leading sentinel 1-bit; then strip
    // it.
    uint32_t halves[2];
    memcpy(halves, out->credential, sizeof(halves));
    if(halves[0] == 0)
    {
        out->bit_length =
            (uint8_t)(31 - __builtin_clz(__builtin_bswap32(halves[1])));
    }
    else
    {
        out->bit_length =
            (uint8_t)(63 - __builtin_clz(__builtin_bswap32(halves[0])));
    }
    uint64_t sentinel = __builtin_bswap64(1ULL << out->bit_length);
    uint64_t swapped;
    memcpy(&swapped, out->credential, sizeof(swapped));
    swapped ^= sentinel;
    memcpy(out->credential, &swapped, sizeof(swapped));

    // Decode every Wiegand format whose length matches and parity validates
    // (37-bit is ambiguous and can yield both H10302 and H10304).
    poom_wiegand_result_t w[POOM_PICOPASS_MAX_WIEGAND];
    bool any_len = false;
    int n        = poom_wiegand_decode(out->credential, out->bit_length, w,
                                       POOM_PICOPASS_MAX_WIEGAND, &any_len);
    out->wiegand_count = (uint8_t)n;
    for(int i = 0; i < n; i++)
    {
        out->wiegand[i].format        = w[i].format;
        out->wiegand[i].facility_code = w[i].facility_code;
        out->wiegand[i].card_number   = w[i].card_number;
    }
    // A known length matched but nothing passed parity -> flag it.
    out->parity_error = (n == 0) && any_len;
}

// Select the card and read the blocks that don't need authentication.
static PoomPicopassStatus pp_select_and_preauth(PoomPicopassDump* out)
{
    // Bring up the NFC core (RFAL + ST25R3916) if it isn't already, so the
    // command works standalone without a prior nfc-core-start. Idempotent.
    if(!poom_nfc_controller_start())
    {
        return PoomPicopassErrInit;
    }

    if(rfalPicoPassPollerInitialize() != RFAL_ERR_NONE)
    {
        return PoomPicopassErrInit;
    }

    // Wake up. ACTALL returns an incomplete frame when a card is present; any
    // other hard error means nothing is on the reader.
    (void)rfalPicoPassPollerCheckPresence();

    // Anticollision -> CSN.
    rfalPicoPassIdentifyRes id;
    if(rfalPicoPassPollerIdentify(&id) != RFAL_ERR_NONE)
    {
        return PoomPicopassErrIdentify;
    }

    // Select by the anticollision CSN -> real CSN (block 0).
    rfalPicoPassSelectRes sel;
    if(rfalPicoPassPollerSelect(id.CSN, &sel) != RFAL_ERR_NONE)
    {
        return PoomPicopassErrSelect;
    }
    memcpy(out->csn, sel.CSN, POOM_PICOPASS_BLOCK_LEN);

    // Pre-auth readable blocks. Config (1), e-purse (2) and AIA (5) are
    // readable without authentication on a standard card. A failed config read
    // leaves app_limit at 0 so the app-block loop reads nothing.
    bool config_ok =
        read_block(POOM_PICOPASS_CONFIG_BLOCK, out->config) == PoomPicopassOk;
    read_block(POOM_PICOPASS_EPURSE_BLOCK, out->epurse);
    read_block(POOM_PICOPASS_AIA_BLOCK, out->aia);

    // config byte 0 is the application limit: the first block of AA1 NOT to
    // read (exclusive bound, so AA1 is blocks 6..app_limit-1).
    out->app_limit = config_ok ? out->config[0] : 0;
    return PoomPicopassOk;
}

// Try to authenticate to AA1 with `key` (elite-diversified when `elite`).
// Returns Ok on success, ErrReadCheck on a comms failure, or ErrAuth when the
// key is simply wrong. out->div_key is left holding this key's diversification
// either way. Safe to call repeatedly on one selection, so the dict attack
// retries without re-selecting.
static PoomPicopassStatus pp_try_key(PoomPicopassDump* out, const uint8_t* key,
                                     bool elite)
{
    loclass_iclass_calc_div_key((uint8_t*)out->csn, (uint8_t*)key, out->div_key,
                                elite);

    // READCHECK fetches the e-purse challenge (CCNR). cc_nr is 12 bytes: the
    // 8-byte challenge followed by 4 zero bytes (the reader nonce, unused
    // here).
    rfalPicoPassReadCheckRes rc;
    if(rfalPicoPassPollerReadCheck(&rc, POOM_PICOPASS_EPURSE_BLOCK,
                                   /*use_credit=*/0) != RFAL_ERR_NONE)
    {
        return PoomPicopassErrReadCheck;
    }
    uint8_t cc_nr[12] = {0};
    memcpy(cc_nr, rc.CCNR, 8);

    // Reader MAC over cc_nr with the diversified key, then CHECK.
    uint8_t mac[4];
    loclass_opt_doReaderMAC(cc_nr, out->div_key, mac);

    rfalPicoPassCheckRes chk;
    if(rfalPicoPassPollerCheck(mac, &chk) != RFAL_ERR_NONE)
    {
        return PoomPicopassErrAuth;
    }
    return PoomPicopassOk;
}

// After a successful auth: record the key, capture the Kd/Kc key blocks and the
// AA1 application blocks, then decode the PACS credential.
static void pp_read_after_auth(PoomPicopassDump* out, const uint8_t* key)
{
    out->authenticated = true;
    out->factory = memcmp(key, poom_picopass_factory_key, POOM_PICOPASS_BLOCK_LEN) == 0;
    memcpy(out->key, key, POOM_PICOPASS_BLOCK_LEN);

    // Capture the key blocks (Kd/Kc) now that we're authenticated, so a saved
    // dump is complete. Cards mask these, but we store whatever they return.
    out->kd_valid = read_block(POOM_PICOPASS_KD_BLOCK, out->kd) == PoomPicopassOk;
    out->kc_valid = read_block(POOM_PICOPASS_KC_BLOCK, out->kc) == PoomPicopassOk;

    // Read AA1 application blocks from block 6 up to (but not including)
    // app_limit.
    uint8_t last  = out->app_limit;
    uint8_t count = 0;
    for(uint8_t blk = POOM_PICOPASS_PACS_CFG_BLOCK;
        blk < last && count < POOM_PICOPASS_MAX_APP_BLOCKS; blk++, count++)
    {
        if(read_block(blk, out->blocks[count]) != PoomPicopassOk)
        {
            break;
        }
    }
    out->app_block_count = count;

    // A factory-keyed card carries no real PACS, so don't decode its default
    // blocks as a credential.
    if(!out->factory)
    {
        poom_picopass_decode_pacs(out);
    }
}

// Try one key on the current selection: authenticate and, on success, read the
// card. Returns Ok (found + read), ErrReadCheck (comms failure, abort), or
// ErrAuth (wrong key, try another).
static PoomPicopassStatus pp_try_one(PoomPicopassDump* out, const uint8_t* key,
                                     bool elite)
{
    PoomPicopassStatus st = pp_try_key(out, key, elite);
    if(st == PoomPicopassOk)
    {
        pp_read_after_auth(out, key);
    }
    return st;
}

// Try every key in a dictionary. Returns Ok as soon as one authenticates,
// ErrReadCheck on a comms failure worth aborting for, or ErrAuth if none match.
static PoomPicopassStatus pp_try_dict(
    PoomPicopassDump* out, const uint8_t keys[][POOM_PICOPASS_BLOCK_LEN],
    size_t count, bool elite)
{
    for(size_t i = 0; i < count; i++)
    {
        PoomPicopassStatus st = pp_try_one(out, keys[i], elite);
        if(st != PoomPicopassErrAuth)
        {
            return st;  // Ok = found, ErrReadCheck = abort
        }
    }
    return PoomPicopassErrAuth;
}

// Many elite keys come from a VB6 LCG keygen: a sliding 8-byte window over the
// LCG's byte stream. Generating the first POOM_PICOPASS_ELITE_PRNG_COUNT keys
// reproduces the LCG-derived entries so they needn't be stored in the elite
// dictionary.
#define POOM_PICOPASS_ELITE_PRNG_SEED  0x429080u
#define POOM_PICOPASS_ELITE_PRNG_COUNT 699

static uint8_t pp_elite_next_byte(uint32_t* seed)
{
    // (x mod 2^32) mod 2^24 == x mod 2^24, so the uint32_t overflow is harmless.
    *seed = (0xFD43FDu * *seed + 0xC39EC3u) % 0x1000000u;
    return (uint8_t)((*seed >> 16) & 0xFF);
}

// Try the LCG-generated elite keys (elite diversification). Returns like
// pp_try_dict.
static PoomPicopassStatus pp_try_elite_prng(PoomPicopassDump* out)
{
    uint32_t seed = POOM_PICOPASS_ELITE_PRNG_SEED;
    uint8_t key[POOM_PICOPASS_BLOCK_LEN];
    for(size_t i = 0; i < POOM_PICOPASS_ELITE_PRNG_COUNT; i++)
    {
        if(i == 0)
        {
            // First key: fill the whole window from the byte stream.
            for(int b = 0; b < POOM_PICOPASS_BLOCK_LEN; b++)
            {
                key[b] = pp_elite_next_byte(&seed);
            }
        }
        else
        {
            // Slide the window left and append the next byte.
            memmove(key, key + 1, POOM_PICOPASS_BLOCK_LEN - 1);
            key[POOM_PICOPASS_BLOCK_LEN - 1] = pp_elite_next_byte(&seed);
        }
        PoomPicopassStatus st = pp_try_one(out, key, /*elite=*/true);
        if(st != PoomPicopassErrAuth)
        {
            return st;  // Ok = found, ErrReadCheck = abort
        }
    }
    return PoomPicopassErrAuth;
}

PoomPicopassStatus poom_picopass_read(PoomPicopassDump* out)
{
    memset(out, 0, sizeof(*out));

    PoomPicopassStatus st = pp_select_and_preauth(out);
    if(st != PoomPicopassOk)
    {
        return st;
    }

    // Try known keys until one authenticates: the well-known debit key and the
    // standard dictionary (standard diversification), then the elite dictionary
    // and the VB6 LCG elite keygen (elite diversification). ErrAuth means "keep
    // going"; Ok (found) or ErrReadCheck (comms failure) stop early. The card is
    // selected once and every key retries READCHECK/CHECK without re-selecting.
    st = pp_try_one(out, poom_picopass_standard_key, /*elite=*/false);
    if(st != PoomPicopassErrAuth)
    {
        return st;
    }

    st = pp_try_dict(out, poom_picopass_standard_keys,
                     POOM_PICOPASS_STANDARD_KEY_COUNT, /*elite=*/false);
    if(st != PoomPicopassErrAuth)
    {
        return st;
    }

    st = pp_try_dict(out, poom_picopass_elite_keys,
                     POOM_PICOPASS_ELITE_KEY_COUNT, /*elite=*/true);
    if(st != PoomPicopassErrAuth)
    {
        return st;
    }

    return pp_try_elite_prng(out);
}

static int hexline(char* p, int n, const char* label, const uint8_t* b)
{
    return snprintf(p, n, "%-8s %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    label, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

int poom_picopass_format(const PoomPicopassDump* dump, char* buf, int buf_len)
{
    char* p = buf;
    int rem = buf_len;
    int w;
#define ADV(expr)             \
    do                        \
    {                         \
        w = (expr);           \
        if(w < 0 || w >= rem) \
            return p - buf;   \
        p += w;               \
        rem -= w;             \
    } while(0)

    ADV(hexline(p, rem, "CSN", dump->csn));
    ADV(hexline(p, rem, "CONFIG", dump->config));
    ADV(hexline(p, rem, "EPURSE", dump->epurse));
    ADV(hexline(p, rem, "AIA", dump->aia));
    ADV(snprintf(p, rem, "AUTH     %s\n", dump->authenticated ? "yes" : "no"));
    if(dump->pacs_present)
    {
        const char* enc = dump->pacs_encryption == 0x17   ? "3DES"
                          : dump->pacs_encryption == 0x14 ? "none"
                          : dump->pacs_encryption == 0x15 ? "DES"
                                                          : "?";
        ADV(snprintf(p, rem, "PACS     enc=%s bits=%u%s\n", enc,
                     dump->bit_length, dump->parity_error ? "!" : ""));
        // Credential, significant bytes only (drop leading zeros).
        int nb = (dump->bit_length + 7) / 8;
        if(nb < 1)
            nb = 1;
        if(nb > 8)
            nb = 8;
        ADV(snprintf(p, rem, "%s", "CRED    "));
        for(int i = 8 - nb; i < 8; i++)
        {
            ADV(snprintf(p, rem, " %02X", dump->credential[i]));
        }
        ADV(snprintf(p, rem, "%s", "\n"));
        for(uint8_t i = 0; i < dump->wiegand_count; i++)
        {
            ADV(snprintf(p, rem, "WIEGAND  %s FC=%lu CN=%llu\n",
                         dump->wiegand[i].format,
                         (unsigned long)dump->wiegand[i].facility_code,
                         (unsigned long long)dump->wiegand[i].card_number));
        }
    }
    for(uint8_t i = 0; i < dump->app_block_count; i++)
    {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "BLK%02d", POOM_PICOPASS_PACS_CFG_BLOCK + i);
        ADV(hexline(p, rem, lbl, dump->blocks[i]));
    }
#undef ADV
    return p - buf;
}
