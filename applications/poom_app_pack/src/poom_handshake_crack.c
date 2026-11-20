// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// On-device WPA handshake quick-crack engine. See poom_handshake_crack.h.

#include "poom_handshake_crack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hs_crack";

/* ----- tunables ---------------------------------------------------------- */

#define HS_PBKDF2_ITERS   (4096U)   /* WPA fixed iteration count              */
#define HS_PMK_LEN        (32U)     /* 256-bit pairwise master key            */
#define HS_MAX_TARGETS    (8U)      /* crackable lines parsed per file        */
#define HS_ESSID_MAX      (32U)
#define HS_EAPOL_MAX      (256U)
#define HS_LINE_MAX       (768U)    /* 22000 line: ~2*256 hex eapol + fields  */

/* EAPOL-Key field offsets inside the stored 802.1X frame (incl. EAPOL hdr). */
#define HS_EAPOL_OFF_KEYINFO (5U)   /* 2 bytes, big-endian                    */
#define HS_EAPOL_OFF_NONCE   (17U)  /* 32 bytes: the STA's SNonce             */
#define HS_EAPOL_OFF_MIC     (81U)  /* 16 bytes: zeroed for MIC computation   */

/* ----- parsed target ----------------------------------------------------- */

typedef struct
{
    uint8_t  kind;                      /* 1 = PMKID, 2 = EAPOL               */
    uint8_t  mac_ap[6];
    uint8_t  mac_sta[6];
    uint8_t  essid[HS_ESSID_MAX];
    uint8_t  essid_len;
    uint8_t  pmkid[16];                 /* kind 1                             */
    uint8_t  keymic[16];                /* kind 2                             */
    uint8_t  anonce[32];                /* kind 2                             */
    uint8_t  snonce[32];                /* kind 2 (extracted from eapol)      */
    uint8_t  eapol[HS_EAPOL_MAX];       /* kind 2, MIC field zeroed           */
    uint16_t eapol_len;
    uint8_t  keyver;                    /* 1 = HMAC-MD5, 2 = HMAC-SHA1        */
} hs_target_t;

/* ----- built-in wordlist ------------------------------------------------- *
 * WPA default / weak-pattern keys only -- NOT human-chosen passphrases. The
 * dictionary-attackable 'default key' space is small by design: modern APs
 * ship unique per-device keys (random, on the label) or algorithmic ISP keys
 * (need a per-vendor generator), neither of which a flat list can catch. What
 * is left, and what this covers, ordered best-first:
 *   - numeric patterns: digit runs, repeats, keypad walks, doubled years,
 *     Fibonacci/Pi (the numeric entries are from real cracked-WPA data);
 *   - keyboard walks (1q2w3e4r, 1qaz2wsx, qwertyuiop ...);
 *   - generic factory/admin words (password, admin, wireless, changeme ...).
 * All WPA-valid (length 8..63). QUICK also tries each with the first letter
 * capitalized, with "!" appended, and both; SMART adds +1/+123 and ESSID.
 * 230 entries, ~3 KB flash. Bigger passphrase lists stream from SD.       */
static const char *const s_wordlist[] = {
    "12345678",      "123456789",     "1234567890",    "0123456789",    "87654321",
    "12341234",      "11223344",      "147258369",     "11111111",      "00000000",
    "22222222",      "33333333",      "44444444",      "55555555",      "66666666",
    "77777777",      "88888888",      "99999999",      "123123123",     "121212121",
    "112233445",     "1234512345",    "1122334455",    "10101010",      "12121212",
    "20202020",      "qwertyuiop",    "qwerty123",     "1q2w3e4r",      "1qaz2wsx",
    "1qazxsw2",      "zaq12wsx",      "qazwsxedc",     "q1w2e3r4",      "1q2w3e4r5t",
    "asdfghjkl",     "asdf1234",      "1234qwer",      "zxcvbnm123",    "password",
    "password1",     "password123",   "admin1234",     "administrator", "changeme123",
    "wireless1",     "internet1",     "computer1",     "default123",    "abcd1234",
    "abcdefgh",      "qwertyui",      "qwerty12",      "qwerty1234",    "qwertyuiop1",
    "q1w2e3r4t5",    "1q2w3e4r5t6y",  "1qaz2wsx3edc",  "qazwsx123",     "qweasdzxc",
    "qweasd123",     "asdfghjk",      "asdfasdf",      "zxcvbnm1",      "zxcvbnmm",
    "1234qwerty",    "12qwaszx",      "poiuytrewq",    "lkjhgfdsa",     "1q2w3e4r5",
    "mnbvcxz1",      "password12",    "passw0rd1",     "p@ssw0rd",      "p@ssword1",
    "admin12345",    "adminadmin",    "admin@123",     "administrator1", "wireless123",
    "wifipassword",  "internet123",   "computer123",   "default1",      "changeme1",
    "letmein1",      "letmein12",     "welcome12",     "welcome1234",   "superuser",
    "guestguest",    "nopassword",    "network123",    "homenetwork",   "router123",
    "myrouter1",     "abc12345",      "1234abcd",      "abcd12345",     "qwerasdf",
    "test1234",      "testtest",      "root1234",      "password!",     "passw0rd!",
    "987654321",     "12344321",      "789456123",     "1111111111",    "0987654321",
    "999999999",     "147852369",     "12345678910",   "123654789",     "741852963",
    "69696969",      "1123581321",    "1234554321",    "0000000000",    "13131313",
    "14789632",      "9876543210",    "1029384756",    "963852741",     "98765432",
    "147896325",     "11235813",      "12345679",      "321654987",     "12301230",
    "21122112",      "4815162342",    "23232323",      "12369874",      "111222333",
    "19841984",      "10203040",      "111111111",     "123454321",     "19891989",
    "11112222",      "19861986",      "19821982",      "123698745",     "123456123",
    "19871987",      "25802580",      "24682468",      "19781978",      "19831983",
    "19801980",      "19751975",      "25252525",      "123789456",     "123456987",
    "12312312",      "31415926",      "21212121",      "123321123",     "19741974",
    "19911991",      "19761976",      "01234567",      "12131415",      "51505150",
    "192837465",     "987456321",     "19941994",      "5555555555",    "19901990",
    "23456789",      "135792468",     "7894561230",    "19811981",      "11221122",
    "19851985",      "19721972",      "1234567899",    "20012001",      "74108520",
    "24681012",      "24242424",      "19881988",      "123456123456",  "19771977",
    "20002000",      "19951995",      "321321321",     "19731973",      "19691969",
    "000000000",     "789654123",     "20102010",      "09876543",      "01020304",
    "20022002",      "19971997",      "1234567891",    "369258147",     "9999999999",
    "13243546",      "19931993",      "19791979",      "19921992",      "14141414",
    "12211221",      "43214321",      "2222222222",    "18436572",      "123456654321",
    "78945612",      "01010101",      "12131213",      "90909090",      "22446688",
    "20032003",      "19641964",      "12141214",      "01230123",      "99887766",
    "19701970",      "1357924680",    "14531453",      "85208520",      "14121412",
    "134679852",     "17171717",      "159159159",     "00112233",      "00001111",
};

#define HS_WORDLIST_LEN (sizeof(s_wordlist) / sizeof(s_wordlist[0]))

uint32_t poom_hs_crack_wordlist_count(void)
{
    return (uint32_t)HS_WORDLIST_LEN;
}

/* ----- hex helpers ------------------------------------------------------- */

static int hs_hex_nibble_(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f'))
    {
        return (c - 'a') + 10;
    }
    if ((c >= 'A') && (c <= 'F'))
    {
        return (c - 'A') + 10;
    }
    return -1;
}

/* Decode exactly `nbytes` of hex from `src` (which must hold >= 2*nbytes hex
 * chars). Returns true on success. */
static bool hs_hex_decode_fixed_(const char *src, size_t nbytes, uint8_t *dst)
{
    for (size_t i = 0U; i < nbytes; i++)
    {
        int hi = hs_hex_nibble_(src[2U * i]);
        int lo = hs_hex_nibble_(src[(2U * i) + 1U]);
        if ((hi < 0) || (lo < 0))
        {
            return false;
        }
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Decode a variable-length hex field (len = number of hex chars) into dst,
 * up to max bytes. Returns the number of bytes written, or 0 on error. */
static size_t hs_hex_decode_var_(const char *src, size_t hexlen, uint8_t *dst, size_t max)
{
    if (((hexlen & 1U) != 0U) || ((hexlen / 2U) > max))
    {
        return 0U;
    }
    size_t nbytes = hexlen / 2U;
    if (!hs_hex_decode_fixed_(src, nbytes, dst))
    {
        return 0U;
    }
    return nbytes;
}

/* ----- crypto -----------------------------------------------------------
 * Self-contained SHA-1 and MD5 (no mbedtls dependency). This sidesteps the
 * mbedtls 2/3/4 API churn entirely, and -- more importantly -- lets PBKDF2
 * precompute the HMAC inner/outer block state once per passphrase and snapshot
 * it (a plain struct copy) for each of the 4096 iterations, roughly halving the
 * SHA-1 work versus re-absorbing the 64-byte pad block every time. */

typedef enum
{
    HS_SHA1 = 0, /* 20-byte digest */
    HS_MD5 = 1,  /* 16-byte digest */
} hs_hash_t;

#define HS_HMAC_BLOCK (64U)

/* ---- SHA-1 ---- */
typedef struct
{
    uint32_t state[5];
    uint64_t nbytes;
    uint8_t  buf[64];
    uint32_t have;
} hs_sha1_t;

#define HS_ROTL32(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void hs_sha1_init(hs_sha1_t *c)
{
    c->state[0] = 0x67452301U;
    c->state[1] = 0xEFCDAB89U;
    c->state[2] = 0x98BADCFEU;
    c->state[3] = 0x10325476U;
    c->state[4] = 0xC3D2E1F0U;
    c->nbytes = 0U;
    c->have = 0U;
}

/* Optimized SHA-1 compression (unrolled, 16-word rolling schedule -- the
 * canonical fast form). This is the inner loop of the whole cracker, so the
 * per-round branch and the 80-word schedule of the naive version are both
 * eliminated. Validated against a reference. */
#define HS_BLK0(i) (w[i])
#define HS_BLK(i)  (w[(i) & 15] = HS_ROTL32(w[((i) + 13) & 15] ^ w[((i) + 8) & 15] ^ \
                                            w[((i) + 2) & 15] ^ w[(i) & 15], 1))
#define HS_R0(v, u, x, y, z, i) z += ((u & (x ^ y)) ^ y) + HS_BLK0(i) + 0x5A827999U + HS_ROTL32(v, 5); u = HS_ROTL32(u, 30);
#define HS_R1(v, u, x, y, z, i) z += ((u & (x ^ y)) ^ y) + HS_BLK(i) + 0x5A827999U + HS_ROTL32(v, 5); u = HS_ROTL32(u, 30);
#define HS_R2(v, u, x, y, z, i) z += (u ^ x ^ y) + HS_BLK(i) + 0x6ED9EBA1U + HS_ROTL32(v, 5); u = HS_ROTL32(u, 30);
#define HS_R3(v, u, x, y, z, i) z += (((u | x) & y) | (u & x)) + HS_BLK(i) + 0x8F1BBCDCU + HS_ROTL32(v, 5); u = HS_ROTL32(u, 30);
#define HS_R4(v, u, x, y, z, i) z += (u ^ x ^ y) + HS_BLK(i) + 0xCA62C1D6U + HS_ROTL32(v, 5); u = HS_ROTL32(u, 30);

static void hs_sha1_block(hs_sha1_t *c, const uint8_t *p)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4];

    HS_R0(a,b,cc,d,e, 0); HS_R0(e,a,b,cc,d, 1); HS_R0(d,e,a,b,cc, 2); HS_R0(cc,d,e,a,b, 3);
    HS_R0(b,cc,d,e,a, 4); HS_R0(a,b,cc,d,e, 5); HS_R0(e,a,b,cc,d, 6); HS_R0(d,e,a,b,cc, 7);
    HS_R0(cc,d,e,a,b, 8); HS_R0(b,cc,d,e,a, 9); HS_R0(a,b,cc,d,e,10); HS_R0(e,a,b,cc,d,11);
    HS_R0(d,e,a,b,cc,12); HS_R0(cc,d,e,a,b,13); HS_R0(b,cc,d,e,a,14); HS_R0(a,b,cc,d,e,15);
    HS_R1(e,a,b,cc,d,16); HS_R1(d,e,a,b,cc,17); HS_R1(cc,d,e,a,b,18); HS_R1(b,cc,d,e,a,19);
    HS_R2(a,b,cc,d,e,20); HS_R2(e,a,b,cc,d,21); HS_R2(d,e,a,b,cc,22); HS_R2(cc,d,e,a,b,23);
    HS_R2(b,cc,d,e,a,24); HS_R2(a,b,cc,d,e,25); HS_R2(e,a,b,cc,d,26); HS_R2(d,e,a,b,cc,27);
    HS_R2(cc,d,e,a,b,28); HS_R2(b,cc,d,e,a,29); HS_R2(a,b,cc,d,e,30); HS_R2(e,a,b,cc,d,31);
    HS_R2(d,e,a,b,cc,32); HS_R2(cc,d,e,a,b,33); HS_R2(b,cc,d,e,a,34); HS_R2(a,b,cc,d,e,35);
    HS_R2(e,a,b,cc,d,36); HS_R2(d,e,a,b,cc,37); HS_R2(cc,d,e,a,b,38); HS_R2(b,cc,d,e,a,39);
    HS_R3(a,b,cc,d,e,40); HS_R3(e,a,b,cc,d,41); HS_R3(d,e,a,b,cc,42); HS_R3(cc,d,e,a,b,43);
    HS_R3(b,cc,d,e,a,44); HS_R3(a,b,cc,d,e,45); HS_R3(e,a,b,cc,d,46); HS_R3(d,e,a,b,cc,47);
    HS_R3(cc,d,e,a,b,48); HS_R3(b,cc,d,e,a,49); HS_R3(a,b,cc,d,e,50); HS_R3(e,a,b,cc,d,51);
    HS_R3(d,e,a,b,cc,52); HS_R3(cc,d,e,a,b,53); HS_R3(b,cc,d,e,a,54); HS_R3(a,b,cc,d,e,55);
    HS_R3(e,a,b,cc,d,56); HS_R3(d,e,a,b,cc,57); HS_R3(cc,d,e,a,b,58); HS_R3(b,cc,d,e,a,59);
    HS_R4(a,b,cc,d,e,60); HS_R4(e,a,b,cc,d,61); HS_R4(d,e,a,b,cc,62); HS_R4(cc,d,e,a,b,63);
    HS_R4(b,cc,d,e,a,64); HS_R4(a,b,cc,d,e,65); HS_R4(e,a,b,cc,d,66); HS_R4(d,e,a,b,cc,67);
    HS_R4(cc,d,e,a,b,68); HS_R4(b,cc,d,e,a,69); HS_R4(a,b,cc,d,e,70); HS_R4(e,a,b,cc,d,71);
    HS_R4(d,e,a,b,cc,72); HS_R4(cc,d,e,a,b,73); HS_R4(b,cc,d,e,a,74); HS_R4(a,b,cc,d,e,75);
    HS_R4(e,a,b,cc,d,76); HS_R4(d,e,a,b,cc,77); HS_R4(cc,d,e,a,b,78); HS_R4(b,cc,d,e,a,79);

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d; c->state[4] += e;
}

/* Specialized single-block SHA-1 compression for a 20-byte message with the
 * standard padding, starting from a midstate that has already absorbed exactly
 * one 64-byte block. This is the precise shape of every HMAC half in the
 * PBKDF2 inner loop -- both the ipad and opad passes compress a 20-byte value
 * (salt || U_prev is only the very first inner call; every iteration after is
 * a bare 20-byte U). Specializing it removes the streaming buffer copies, the
 * per-call struct copy of the primed context, and the byte-at-a-time final
 * padding -- the dominant per-op overhead across the ~16k hashes per guess.
 * The 11 tail words are compile-time constants, which the optimizer folds into
 * rounds 5..15 and the early schedule expansions. Safe when out == in (the
 * message is loaded into w[] before out is written). Validated against the
 * streaming path and a Python reference. */
static void hs_sha1_block_u20(const uint32_t mid[5], const uint8_t in[20], uint8_t out[20])
{
    uint32_t w[16];
    w[0]  = ((uint32_t)in[0]  << 24) | ((uint32_t)in[1]  << 16) | ((uint32_t)in[2]  << 8) | (uint32_t)in[3];
    w[1]  = ((uint32_t)in[4]  << 24) | ((uint32_t)in[5]  << 16) | ((uint32_t)in[6]  << 8) | (uint32_t)in[7];
    w[2]  = ((uint32_t)in[8]  << 24) | ((uint32_t)in[9]  << 16) | ((uint32_t)in[10] << 8) | (uint32_t)in[11];
    w[3]  = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) | ((uint32_t)in[14] << 8) | (uint32_t)in[15];
    w[4]  = ((uint32_t)in[16] << 24) | ((uint32_t)in[17] << 16) | ((uint32_t)in[18] << 8) | (uint32_t)in[19];
    w[5]  = 0x80000000U;   /* 0x80 padding byte right after the 20 message bytes */
    w[6]  = 0U; w[7]  = 0U; w[8]  = 0U; w[9]  = 0U; w[10] = 0U;
    w[11] = 0U; w[12] = 0U; w[13] = 0U; w[14] = 0U;
    w[15] = 0x000002A0U;   /* bit length = (64 + 20) * 8 = 672 */

    uint32_t a = mid[0], b = mid[1], cc = mid[2], d = mid[3], e = mid[4];

    HS_R0(a,b,cc,d,e, 0); HS_R0(e,a,b,cc,d, 1); HS_R0(d,e,a,b,cc, 2); HS_R0(cc,d,e,a,b, 3);
    HS_R0(b,cc,d,e,a, 4); HS_R0(a,b,cc,d,e, 5); HS_R0(e,a,b,cc,d, 6); HS_R0(d,e,a,b,cc, 7);
    HS_R0(cc,d,e,a,b, 8); HS_R0(b,cc,d,e,a, 9); HS_R0(a,b,cc,d,e,10); HS_R0(e,a,b,cc,d,11);
    HS_R0(d,e,a,b,cc,12); HS_R0(cc,d,e,a,b,13); HS_R0(b,cc,d,e,a,14); HS_R0(a,b,cc,d,e,15);
    HS_R1(e,a,b,cc,d,16); HS_R1(d,e,a,b,cc,17); HS_R1(cc,d,e,a,b,18); HS_R1(b,cc,d,e,a,19);
    HS_R2(a,b,cc,d,e,20); HS_R2(e,a,b,cc,d,21); HS_R2(d,e,a,b,cc,22); HS_R2(cc,d,e,a,b,23);
    HS_R2(b,cc,d,e,a,24); HS_R2(a,b,cc,d,e,25); HS_R2(e,a,b,cc,d,26); HS_R2(d,e,a,b,cc,27);
    HS_R2(cc,d,e,a,b,28); HS_R2(b,cc,d,e,a,29); HS_R2(a,b,cc,d,e,30); HS_R2(e,a,b,cc,d,31);
    HS_R2(d,e,a,b,cc,32); HS_R2(cc,d,e,a,b,33); HS_R2(b,cc,d,e,a,34); HS_R2(a,b,cc,d,e,35);
    HS_R2(e,a,b,cc,d,36); HS_R2(d,e,a,b,cc,37); HS_R2(cc,d,e,a,b,38); HS_R2(b,cc,d,e,a,39);
    HS_R3(a,b,cc,d,e,40); HS_R3(e,a,b,cc,d,41); HS_R3(d,e,a,b,cc,42); HS_R3(cc,d,e,a,b,43);
    HS_R3(b,cc,d,e,a,44); HS_R3(a,b,cc,d,e,45); HS_R3(e,a,b,cc,d,46); HS_R3(d,e,a,b,cc,47);
    HS_R3(cc,d,e,a,b,48); HS_R3(b,cc,d,e,a,49); HS_R3(a,b,cc,d,e,50); HS_R3(e,a,b,cc,d,51);
    HS_R3(d,e,a,b,cc,52); HS_R3(cc,d,e,a,b,53); HS_R3(b,cc,d,e,a,54); HS_R3(a,b,cc,d,e,55);
    HS_R3(e,a,b,cc,d,56); HS_R3(d,e,a,b,cc,57); HS_R3(cc,d,e,a,b,58); HS_R3(b,cc,d,e,a,59);
    HS_R4(a,b,cc,d,e,60); HS_R4(e,a,b,cc,d,61); HS_R4(d,e,a,b,cc,62); HS_R4(cc,d,e,a,b,63);
    HS_R4(b,cc,d,e,a,64); HS_R4(a,b,cc,d,e,65); HS_R4(e,a,b,cc,d,66); HS_R4(d,e,a,b,cc,67);
    HS_R4(cc,d,e,a,b,68); HS_R4(b,cc,d,e,a,69); HS_R4(a,b,cc,d,e,70); HS_R4(e,a,b,cc,d,71);
    HS_R4(d,e,a,b,cc,72); HS_R4(cc,d,e,a,b,73); HS_R4(b,cc,d,e,a,74); HS_R4(a,b,cc,d,e,75);
    HS_R4(e,a,b,cc,d,76); HS_R4(d,e,a,b,cc,77); HS_R4(cc,d,e,a,b,78); HS_R4(b,cc,d,e,a,79);

    const uint32_t s0 = mid[0] + a, s1 = mid[1] + b, s2 = mid[2] + cc,
                   s3 = mid[3] + d, s4 = mid[4] + e;
    out[0]  = (uint8_t)(s0 >> 24); out[1]  = (uint8_t)(s0 >> 16); out[2]  = (uint8_t)(s0 >> 8); out[3]  = (uint8_t)s0;
    out[4]  = (uint8_t)(s1 >> 24); out[5]  = (uint8_t)(s1 >> 16); out[6]  = (uint8_t)(s1 >> 8); out[7]  = (uint8_t)s1;
    out[8]  = (uint8_t)(s2 >> 24); out[9]  = (uint8_t)(s2 >> 16); out[10] = (uint8_t)(s2 >> 8); out[11] = (uint8_t)s2;
    out[12] = (uint8_t)(s3 >> 24); out[13] = (uint8_t)(s3 >> 16); out[14] = (uint8_t)(s3 >> 8); out[15] = (uint8_t)s3;
    out[16] = (uint8_t)(s4 >> 24); out[17] = (uint8_t)(s4 >> 16); out[18] = (uint8_t)(s4 >> 8); out[19] = (uint8_t)s4;
}
#undef HS_BLK0
#undef HS_BLK
#undef HS_R0
#undef HS_R1
#undef HS_R2
#undef HS_R3
#undef HS_R4

static void hs_sha1_update(hs_sha1_t *c, const uint8_t *p, size_t len)
{
    c->nbytes += len;
    while (len > 0U)
    {
        uint32_t take = 64U - c->have;
        if (take > len) { take = (uint32_t)len; }
        memcpy(c->buf + c->have, p, take);
        c->have += take; p += take; len -= take;
        if (c->have == 64U) { hs_sha1_block(c, c->buf); c->have = 0U; }
    }
}

static void hs_sha1_final(hs_sha1_t *c, uint8_t out[20])
{
    uint64_t bits = c->nbytes * 8U;
    uint8_t pad = 0x80U;
    hs_sha1_update(c, &pad, 1U);
    uint8_t z = 0U;
    while (c->have != 56U) { hs_sha1_update(c, &z, 1U); }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) { lenb[i] = (uint8_t)(bits >> (56 - 8 * i)); }
    hs_sha1_update(c, lenb, 8U);
    for (int i = 0; i < 5; i++)
    {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->state[i];
    }
}

/* ---- MD5 (only for WPA/TKIP keyver-1 MIC; not perf-critical) ---- */
typedef struct
{
    uint32_t state[4];
    uint64_t nbytes;
    uint8_t  buf[64];
    uint32_t have;
} hs_md5_t;

static const uint32_t HS_MD5_K[64] = {
    0xd76aa478U,0xe8c7b756U,0x242070dbU,0xc1bdceeeU,0xf57c0fafU,0x4787c62aU,0xa8304613U,0xfd469501U,
    0x698098d8U,0x8b44f7afU,0xffff5bb1U,0x895cd7beU,0x6b901122U,0xfd987193U,0xa679438eU,0x49b40821U,
    0xf61e2562U,0xc040b340U,0x265e5a51U,0xe9b6c7aaU,0xd62f105dU,0x02441453U,0xd8a1e681U,0xe7d3fbc8U,
    0x21e1cde6U,0xc33707d6U,0xf4d50d87U,0x455a14edU,0xa9e3e905U,0xfcefa3f8U,0x676f02d9U,0x8d2a4c8aU,
    0xfffa3942U,0x8771f681U,0x6d9d6122U,0xfde5380cU,0xa4beea44U,0x4bdecfa9U,0xf6bb4b60U,0xbebfbc70U,
    0x289b7ec6U,0xeaa127faU,0xd4ef3085U,0x04881d05U,0xd9d4d039U,0xe6db99e5U,0x1fa27cf8U,0xc4ac5665U,
    0xf4292244U,0x432aff97U,0xab9423a7U,0xfc93a039U,0x655b59c3U,0x8f0ccc92U,0xffeff47dU,0x85845dd1U,
    0x6fa87e4fU,0xfe2ce6e0U,0xa3014314U,0x4e0811a1U,0xf7537e82U,0xbd3af235U,0x2ad7d2bbU,0xeb86d391U};
static const uint8_t HS_MD5_S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

static void hs_md5_init(hs_md5_t *c)
{
    c->state[0] = 0x67452301U;
    c->state[1] = 0xefcdab89U;
    c->state[2] = 0x98badcfeU;
    c->state[3] = 0x10325476U;
    c->nbytes = 0U;
    c->have = 0U;
}

static void hs_md5_block(hs_md5_t *c, const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
    {
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }
    uint32_t a = c->state[0], b = c->state[1], d = c->state[2], e = c->state[3];
    for (int i = 0; i < 64; i++)
    {
        uint32_t f, g;
        if (i < 16) { f = (b & d) | ((~b) & e); g = (uint32_t)i; }
        else if (i < 32) { f = (e & b) | ((~e) & d); g = (uint32_t)(5 * i + 1) % 16U; }
        else if (i < 48) { f = b ^ d ^ e; g = (uint32_t)(3 * i + 5) % 16U; }
        else { f = d ^ (b | (~e)); g = (uint32_t)(7 * i) % 16U; }
        f = f + a + HS_MD5_K[i] + m[g];
        a = e; e = d; d = b;
        b = b + HS_ROTL32(f, HS_MD5_S[i]);
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += d; c->state[3] += e;
}

static void hs_md5_update(hs_md5_t *c, const uint8_t *p, size_t len)
{
    c->nbytes += len;
    while (len > 0U)
    {
        uint32_t take = 64U - c->have;
        if (take > len) { take = (uint32_t)len; }
        memcpy(c->buf + c->have, p, take);
        c->have += take; p += take; len -= take;
        if (c->have == 64U) { hs_md5_block(c, c->buf); c->have = 0U; }
    }
}

static void hs_md5_final(hs_md5_t *c, uint8_t out[16])
{
    uint64_t bits = c->nbytes * 8U;
    uint8_t pad = 0x80U;
    hs_md5_update(c, &pad, 1U);
    uint8_t z = 0U;
    while (c->have != 56U) { hs_md5_update(c, &z, 1U); }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) { lenb[i] = (uint8_t)(bits >> (8 * i)); }
    hs_md5_update(c, lenb, 8U);
    for (int i = 0; i < 4; i++)
    {
        out[i * 4]     = (uint8_t)c->state[i];
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(c->state[i] >> 24);
    }
}

/* HMAC(key, msg) for SHA-1 or MD5 (one-shot). out holds the full digest
 * (20 or 16 bytes). Used for PMKID / PTK / MIC -- called only a few times per
 * candidate, so no precompute here. */
static int hs_hmac_(hs_hash_t h, const uint8_t *key, size_t keylen,
                    const uint8_t *msg, size_t msglen, uint8_t *out)
{
    const size_t dlen = (h == HS_SHA1) ? 20U : 16U;
    uint8_t k0[HS_HMAC_BLOCK];
    uint8_t ipad[HS_HMAC_BLOCK];
    uint8_t opad[HS_HMAC_BLOCK];
    uint8_t khash[20];
    uint8_t inner[20];

    /* K0 = key (hashed first if longer than the block), zero-padded. */
    if (keylen > HS_HMAC_BLOCK)
    {
        if (h == HS_SHA1)
        {
            hs_sha1_t c; hs_sha1_init(&c); hs_sha1_update(&c, key, keylen); hs_sha1_final(&c, khash);
        }
        else
        {
            hs_md5_t c; hs_md5_init(&c); hs_md5_update(&c, key, keylen); hs_md5_final(&c, khash);
        }
        key = khash;
        keylen = dlen;
    }
    memset(k0, 0, sizeof(k0));
    memcpy(k0, key, keylen);
    for (size_t i = 0U; i < HS_HMAC_BLOCK; i++)
    {
        ipad[i] = k0[i] ^ 0x36U;
        opad[i] = k0[i] ^ 0x5CU;
    }

    if (h == HS_SHA1)
    {
        hs_sha1_t c;
        hs_sha1_init(&c); hs_sha1_update(&c, ipad, HS_HMAC_BLOCK); hs_sha1_update(&c, msg, msglen); hs_sha1_final(&c, inner);
        hs_sha1_init(&c); hs_sha1_update(&c, opad, HS_HMAC_BLOCK); hs_sha1_update(&c, inner, 20U); hs_sha1_final(&c, out);
    }
    else
    {
        hs_md5_t c;
        hs_md5_init(&c); hs_md5_update(&c, ipad, HS_HMAC_BLOCK); hs_md5_update(&c, msg, msglen); hs_md5_final(&c, inner);
        hs_md5_init(&c); hs_md5_update(&c, opad, HS_HMAC_BLOCK); hs_md5_update(&c, inner, 16U); hs_md5_final(&c, out);
    }
    return 0;
}

/* PBKDF2-HMAC-SHA1 (dkLen = outlen), optimized for WPA's fixed 4096 iterations.
 * The HMAC key is the passphrase, constant across all iterations, so we absorb
 * the ipad/opad blocks into a SHA-1 context once and snapshot that primed state
 * (a struct copy) for every iteration -- each HMAC then costs two block
 * compressions instead of four. */
static int hs_pbkdf2_sha1_(const uint8_t *pw, size_t pwlen,
                           const uint8_t *salt, size_t saltlen,
                           uint32_t iters, uint8_t *out, size_t outlen)
{
    uint8_t k0[HS_HMAC_BLOCK];
    uint8_t ipad[HS_HMAC_BLOCK];
    uint8_t opad[HS_HMAC_BLOCK];
    uint8_t khash[20];

    if (pwlen > HS_HMAC_BLOCK)
    {
        hs_sha1_t c; hs_sha1_init(&c); hs_sha1_update(&c, pw, pwlen); hs_sha1_final(&c, khash);
        pw = khash; pwlen = 20U;
    }
    memset(k0, 0, sizeof(k0));
    memcpy(k0, pw, pwlen);
    for (size_t i = 0U; i < HS_HMAC_BLOCK; i++)
    {
        ipad[i] = k0[i] ^ 0x36U;
        opad[i] = k0[i] ^ 0x5CU;
    }

    /* Primed states: SHA-1 having absorbed exactly the 64-byte pad block. */
    hs_sha1_t ictx, octx;
    hs_sha1_init(&ictx); hs_sha1_update(&ictx, ipad, HS_HMAC_BLOCK);
    hs_sha1_init(&octx); hs_sha1_update(&octx, opad, HS_HMAC_BLOCK);

    const uint32_t blocks = (uint32_t)((outlen + 19U) / 20U);
    for (uint32_t b = 1U; b <= blocks; b++)
    {
        uint8_t u[20];
        uint8_t t[20];
        const uint8_t ib[4] = {
            (uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b};
        hs_sha1_t c;

        /* U_1 = HMAC(pw, salt || INT_BE32(b)). The inner message here is the
         * variable-length salt||INT, so it uses the streaming path; the outer
         * (a bare 20-byte U) uses the specialized single-block compression. */
        c = ictx; hs_sha1_update(&c, salt, saltlen); hs_sha1_update(&c, ib, 4U); hs_sha1_final(&c, u);
        hs_sha1_block_u20(octx.state, u, u);
        memcpy(t, u, sizeof(t));

        /* U_i = HMAC(pw, U_{i-1}): every remaining iteration hashes a bare
         * 20-byte value through both the ipad and opad primed states, so both
         * halves are a single specialized block compression -- no streaming
         * buffer, no per-call context copy, no byte-wise final padding. */
        for (uint32_t i = 1U; i < iters; i++)
        {
            hs_sha1_block_u20(ictx.state, u, u);
            hs_sha1_block_u20(octx.state, u, u);
            for (size_t k = 0U; k < sizeof(t); k++)
            {
                t[k] ^= u[k];
            }
        }

        const size_t off = (size_t)(b - 1U) * 20U;
        size_t cpy = outlen - off;
        if (cpy > 20U)
        {
            cpy = 20U;
        }
        memcpy(&out[off], t, cpy);
    }
    return 0;
}

/* PMKID = HMAC-SHA1(PMK, "PMK Name" || AP || STA)[:16]. */
static bool hs_check_pmkid_(const uint8_t *pmk, const hs_target_t *t)
{
    uint8_t buf[8 + 6 + 6];
    uint8_t mac[20];

    memcpy(&buf[0], "PMK Name", 8);
    memcpy(&buf[8], t->mac_ap, 6);
    memcpy(&buf[14], t->mac_sta, 6);

    if (hs_hmac_(HS_SHA1, pmk, HS_PMK_LEN, buf, sizeof(buf), mac) != 0)
    {
        return false;
    }
    return memcmp(mac, t->pmkid, 16) == 0;
}

/* Derive the PTK's KCK and compare the EAPOL MIC. */
static bool hs_check_mic_(const uint8_t *pmk, const hs_target_t *t)
{
    /* B = min(AA,SA) || max(AA,SA) || min(ANonce,SNonce) || max(ANonce,SNonce) */
    const uint8_t *m1 = (memcmp(t->mac_ap, t->mac_sta, 6) < 0) ? t->mac_ap : t->mac_sta;
    const uint8_t *m2 = (memcmp(t->mac_ap, t->mac_sta, 6) < 0) ? t->mac_sta : t->mac_ap;
    const uint8_t *n1 = (memcmp(t->anonce, t->snonce, 32) < 0) ? t->anonce : t->snonce;
    const uint8_t *n2 = (memcmp(t->anonce, t->snonce, 32) < 0) ? t->snonce : t->anonce;

    /* "Pairwise key expansion" (22) + 0x00 + B(76) + counter(1) = 100 bytes. */
    uint8_t prf_in[100];
    memcpy(&prf_in[0], "Pairwise key expansion", 22);
    prf_in[22] = 0x00;
    memcpy(&prf_in[23], m1, 6);
    memcpy(&prf_in[29], m2, 6);
    memcpy(&prf_in[35], n1, 32);
    memcpy(&prf_in[67], n2, 32);
    prf_in[99] = 0x00; /* PRF block counter 0 -> first 20 bytes hold the KCK */

    uint8_t ptk[20];
    if (hs_hmac_(HS_SHA1, pmk, HS_PMK_LEN, prf_in, sizeof(prf_in), ptk) != 0)
    {
        return false;
    }
    const uint8_t *kck = ptk; /* first 16 bytes */

    uint8_t mic[20];
    const hs_hash_t md = (t->keyver == 1U) ? HS_MD5 : HS_SHA1;
    if (hs_hmac_(md, kck, 16, t->eapol, t->eapol_len, mic) != 0)
    {
        return false;
    }
    return memcmp(mic, t->keymic, 16) == 0;
}

/* ----- 22000 line parser ------------------------------------------------- */

/* Split `line` by '*' into up to `max` field pointers (into the buffer,
 * NUL-terminating each). Returns the field count. */
static size_t hs_split_fields_(char *line, char **fields, size_t max, size_t *lens)
{
    size_t n = 0U;
    char *p = line;
    char *start = line;

    for (;;)
    {
        if ((*p == '*') || (*p == '\0'))
        {
            if (n < max)
            {
                lens[n] = (size_t)(p - start);
                fields[n] = start;
                n++;
            }
            if (*p == '\0')
            {
                break;
            }
            *p = '\0';
            start = p + 1;
        }
        p++;
    }
    return n;
}

/* Parse one 22000 line into *t. Returns true if it produced a usable target. */
static bool hs_parse_line_(char *line, hs_target_t *t)
{
    char  *f[9];
    size_t flen[9];
    size_t nf = hs_split_fields_(line, f, 9U, flen);

    if ((nf < 6U) || (strcmp(f[0], "WPA") != 0))
    {
        return false;
    }

    memset(t, 0, sizeof(*t));

    /* Common fields: AP mac (f[3]), STA mac (f[4]), essid (f[5]). */
    if ((flen[3] != 12U) || !hs_hex_decode_fixed_(f[3], 6U, t->mac_ap))
    {
        return false;
    }
    if ((flen[4] != 12U) || !hs_hex_decode_fixed_(f[4], 6U, t->mac_sta))
    {
        return false;
    }
    size_t essid_bytes = hs_hex_decode_var_(f[5], flen[5], t->essid, HS_ESSID_MAX);
    if ((flen[5] == 0U) || (essid_bytes == 0U))
    {
        return false;
    }
    t->essid_len = (uint8_t)essid_bytes;

    if (strcmp(f[1], "01") == 0)
    {
        /* PMKID: WPA*01*<pmkid>*<ap>*<sta>*<essid>*** */
        if ((flen[2] != 32U) || !hs_hex_decode_fixed_(f[2], 16U, t->pmkid))
        {
            return false;
        }
        t->kind = 1U;
        return true;
    }

    if (strcmp(f[1], "02") == 0)
    {
        /* EAPOL: WPA*02*<mic>*<ap>*<sta>*<essid>*<anonce>*<eapol>*<mp> */
        if (nf < 8U)
        {
            return false;
        }
        if ((flen[2] != 32U) || !hs_hex_decode_fixed_(f[2], 16U, t->keymic))
        {
            return false;
        }
        if ((flen[6] != 64U) || !hs_hex_decode_fixed_(f[6], 32U, t->anonce))
        {
            return false;
        }
        size_t elen = hs_hex_decode_var_(f[7], flen[7], t->eapol, HS_EAPOL_MAX);
        if (elen < 99U) /* need through the MIC field + key-data length */
        {
            return false;
        }
        t->eapol_len = (uint16_t)elen;

        /* Key version from the EAPOL Key Information field (big-endian). */
        uint16_t key_info = ((uint16_t)t->eapol[HS_EAPOL_OFF_KEYINFO] << 8) |
                            (uint16_t)t->eapol[HS_EAPOL_OFF_KEYINFO + 1U];
        uint8_t ver = (uint8_t)(key_info & 0x0007U);
        if ((ver != 1U) && (ver != 2U))
        {
            return false; /* keyver 3 (AES-CMAC) unsupported in this quick check */
        }
        t->keyver = ver;

        /* SNonce lives in the STA's EAPOL-Key frame; MIC field must be zero. */
        memcpy(t->snonce, &t->eapol[HS_EAPOL_OFF_NONCE], 32);
        memset(&t->eapol[HS_EAPOL_OFF_MIC], 0, 16);

        t->kind = 2U;
        return true;
    }

    return false;
}

/* ----- smart rules ------------------------------------------------------- *
 * A small curated ruleset applied to each base word (and to the ESSID), in
 * "rule-major" order so the most likely exact passwords are tried first and
 * common variations follow. Covers the patterns people actually use on WiFi:
 * capitalized first letter, and a trailing digit/"!" (rockyou is almost all
 * lowercase with no suffixes, so these are real gaps). */
typedef enum
{
    /* Order matters: QUICK runs ASIS..CAP_BANG, A:MORE/CONTINUE runs the rest.
     * Keep the QUICK family (as-is, capitalize, bang, cap+bang) contiguous and
     * first. */
    HS_RULE_ASIS = 0,  /* word     (QUICK)          */
    HS_RULE_CAP,       /* Word     (QUICK)          */
    HS_RULE_BANG,      /* word!    (QUICK)          */
    HS_RULE_CAP_BANG,  /* Word!    (QUICK)          */
    HS_RULE_ONE,       /* word1    (CONTINUE/SMART) */
    HS_RULE_123,       /* word123  (CONTINUE/SMART) */
    HS_RULE_CAP_ONE,   /* Word1    (CONTINUE/SMART) */
    HS_RULE_COUNT
} hs_rule_t;

/* Apply rule r to word w, writing a NUL-terminated candidate to out. Returns
 * its length, or 0 to skip: capitalization rules capitalize the first letter of
 * the first real "word" -- a run of >=2 consecutive letters -- so "abcd1234" ->
 * "Abcd1234", but a key with only isolated letters ("1q2w3e4r") or none
 * (all-digit) gets no capitalized variant; anything outside the WPA 8..63
 * length range is skipped too. */
static size_t hs_apply_rule_(hs_rule_t r, const char *w, char *out, size_t outsz)
{
    const bool cap = (r == HS_RULE_CAP) || (r == HS_RULE_CAP_ONE) || (r == HS_RULE_CAP_BANG);
    const char *suf = "";
    if ((r == HS_RULE_BANG) || (r == HS_RULE_CAP_BANG)) { suf = "!"; }
    else if ((r == HS_RULE_ONE) || (r == HS_RULE_CAP_ONE)) { suf = "1"; }
    else if (r == HS_RULE_123) { suf = "123"; }

    size_t cap_idx = 0U;
    if (cap)
    {
        /* Capitalize only a real word: the first letter of the first run of two
         * or more consecutive letters. Isolated letters in keyboard-walk keys
         * (e.g. "1q2w3e4r") are left alone, and a key with no such run gets no
         * capitalized variant at all. */
        bool have_word = false;
        for (size_t i = 0U; (w[i] != '\0') && (w[i + 1U] != '\0'); i++)
        {
            if ((w[i] >= 'a') && (w[i] <= 'z') &&
                (w[i + 1U] >= 'a') && (w[i + 1U] <= 'z'))
            {
                cap_idx = i;
                have_word = true;
                break;
            }
        }
        if (!have_word)
        {
            return 0U; /* no multi-letter word to capitalize */
        }
    }

    const size_t wl = strlen(w);
    const size_t sl = strlen(suf);
    const size_t total = wl + sl;
    if ((total < 8U) || (total > 63U) || ((total + 1U) > outsz))
    {
        return 0U;
    }
    memcpy(out, w, wl);
    memcpy(out + wl, suf, sl);
    out[total] = '\0';
    if (cap)
    {
        out[cap_idx] = (char)(out[cap_idx] - 32); /* first a..z -> A..Z */
    }
    return total;
}

/* Try one candidate passphrase against every target (PMK cached across targets
 * that share an ESSID). Returns true and fills out->password on a match. */
static bool hs_try_passphrase_(const char *pw, size_t pwlen,
                               const hs_target_t *targets, size_t ntargets,
                               poom_hs_crack_result_t *out)
{
    uint8_t pmk[HS_PMK_LEN];
    const uint8_t *cache_essid = NULL;
    uint8_t cache_len = 0U;
    bool have_pmk = false;

    for (size_t ti = 0U; ti < ntargets; ti++)
    {
        const hs_target_t *t = &targets[ti];
        if (!have_pmk || (cache_len != t->essid_len) ||
            (memcmp(cache_essid, t->essid, t->essid_len) != 0))
        {
            if (hs_pbkdf2_sha1_((const uint8_t *)pw, pwlen, t->essid, t->essid_len,
                                HS_PBKDF2_ITERS, pmk, HS_PMK_LEN) != 0)
            {
                continue;
            }
            cache_essid = t->essid;
            cache_len = t->essid_len;
            have_pmk = true;
        }
        bool hit = (t->kind == 1U) ? hs_check_pmkid_(pmk, t) : hs_check_mic_(pmk, t);
        if (hit)
        {
            out->found = true;
            (void)snprintf(out->password, sizeof(out->password), "%s", pw);
            return true;
        }
    }
    return false;
}

/* Open `path`, parse up to HS_MAX_TARGETS crackable targets into `targets`
 * (caller-allocated, HS_MAX_TARGETS entries), set *ntargets and the count /
 * essid fields in *out. On failure sets out->error + out->error_msg and leaves
 * *ntargets = 0. Returns ESP_OK unless args are invalid. Shared by the built-in
 * wordlist run and the SD-card wordlist run so both parse identically. */
static esp_err_t hs_load_targets_(const char *path, hs_target_t *targets,
                                  size_t *ntargets, poom_hs_crack_result_t *out)
{
    *ntargets = 0U;

    char *line = (char *)malloc(HS_LINE_MAX);
    if (line == NULL)
    {
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "Out of memory");
        return ESP_OK;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        free(line);
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "Cannot open file");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "open %s", path);

    size_t   n = 0U;
    uint32_t lineno = 0U;
    bool     logged_reject = false;
    while ((n < HS_MAX_TARGETS) && (fgets(line, (int)HS_LINE_MAX, fp) != NULL))
    {
        lineno++;
        size_t ll = strlen(line);
        while ((ll > 0U) && ((line[ll - 1U] == '\n') || (line[ll - 1U] == '\r')))
        {
            line[--ll] = '\0';
        }
        if (ll == 0U)
        {
            continue;
        }
        if (hs_parse_line_(line, &targets[n]))
        {
            if (targets[n].kind == 1U)
            {
                out->pmkid_count++;
            }
            else
            {
                out->eapol_count++;
            }
            n++;
        }
        else
        {
            out->skipped_count++;
            if (!logged_reject)
            {
                ESP_LOGW(TAG, "reject line %u (len=%u): %.48s",
                         (unsigned)lineno, (unsigned)ll, line);
                logged_reject = true;
            }
        }
    }
    (void)fclose(fp);
    free(line);

    ESP_LOGI(TAG, "parsed: lines=%u targets=%u (pmkid=%u eapol=%u) skipped=%u",
             (unsigned)lineno, (unsigned)n,
             (unsigned)out->pmkid_count, (unsigned)out->eapol_count,
             (unsigned)out->skipped_count);

    out->target_count = (uint16_t)n;
    if (n == 0U)
    {
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "No handshake found");
        return ESP_OK;
    }

    /* ESSID of the first target, for the result screen. */
    {
        uint8_t en = targets[0].essid_len;
        if (en > (sizeof(out->essid) - 1U))
        {
            en = (uint8_t)(sizeof(out->essid) - 1U);
        }
        memcpy(out->essid, targets[0].essid, en);
        out->essid[en] = '\0';
    }

    *ntargets = n;
    return ESP_OK;
}

/* Read one line from a wordlist file into buf (NUL-terminated, trailing CR/LF
 * stripped). Returns the trimmed length, or (size_t)-1 at EOF. An overlong line
 * (longer than the buffer) is truncated and the remainder drained so the stream
 * never desyncs; such a fragment is >= buffer size and so fails the 8..63 WPA
 * filter anyway. Used by both the count pass and the run pass so their notion of
 * a candidate line is identical. */
static size_t hs_wl_readline_(FILE *fp, char *buf, size_t sz)
{
    if (fgets(buf, (int)sz, fp) == NULL)
    {
        return (size_t)-1;
    }
    size_t l = strlen(buf);
    const bool had_nl = (l > 0U) && (buf[l - 1U] == '\n');
    while ((l > 0U) && ((buf[l - 1U] == '\n') || (buf[l - 1U] == '\r')))
    {
        buf[--l] = '\0';
    }
    if (!had_nl)
    {
        int c;
        while (((c = fgetc(fp)) != EOF) && (c != '\n'))
        {
            /* drain the rest of an overlong line */
        }
    }
    return l;
}

/* ----- public: peek AP identity ------------------------------------------ */

esp_err_t poom_hs_crack_peek(const char *path, uint8_t bssid[6],
                             char *essid, size_t essid_sz)
{
    if ((path == NULL) || (bssid == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    hs_target_t *targets = (hs_target_t *)calloc(HS_MAX_TARGETS, sizeof(hs_target_t));
    if (targets == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    poom_hs_crack_result_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t ntargets = 0U;
    (void)hs_load_targets_(path, targets, &ntargets, &tmp);

    esp_err_t rc = ESP_FAIL;
    if (!tmp.error && (ntargets > 0U))
    {
        memcpy(bssid, targets[0].mac_ap, 6);
        if ((essid != NULL) && (essid_sz > 0U))
        {
            size_t n = targets[0].essid_len;
            if (n > (essid_sz - 1U))
            {
                n = essid_sz - 1U;
            }
            memcpy(essid, targets[0].essid, n);
            essid[n] = '\0';
        }
        rc = ESP_OK;
    }
    free(targets);
    return rc;
}

/* ----- public: run ------------------------------------------------------- */

/* Candidates a QUICK run tries: wordlist x rules ASIS..CAP_BANG (no ESSID).
 * Mirrors the QUICK scope in poom_hs_crack_run_file so the UI can show a true
 * count without a stale constant. */
uint32_t poom_hs_crack_quick_count(void)
{
    char cand[80];
    uint32_t total = 0U;
    for (int r = (int)HS_RULE_ASIS; r <= (int)HS_RULE_CAP_BANG; r++)
    {
        for (uint32_t w = 0U; w < HS_WORDLIST_LEN; w++)
        {
            if (hs_apply_rule_((hs_rule_t)r, s_wordlist[w], cand, sizeof(cand)) != 0U)
            {
                total++;
            }
        }
    }
    return total;
}

esp_err_t poom_hs_crack_run_file(const char               *path,
                                 poom_hs_crack_scope_t      scope,
                                 const volatile bool       *cancel,
                                 poom_hs_crack_progress_fn  cb,
                                 void                      *user,
                                 poom_hs_crack_result_t    *out)
{
    if ((path == NULL) || (out == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Scope -> which rule passes and whether ESSID-derived candidates run.
     * QUICK: the as-is / capitalize / bang / cap+bang family (ASIS..CAP_BANG),
     * no ESSID. FULL: every rule + ESSID. CONTINUE: the remaining rules
     * (ONE..CAP_ONE) + ESSID, i.e. resume after a QUICK run with no overlap. */
    const int first_rule = (scope == POOM_HS_SCOPE_CONTINUE) ? (int)HS_RULE_ONE
                                                             : (int)HS_RULE_ASIS;
    const int last_rule  = (scope == POOM_HS_SCOPE_QUICK) ? (int)HS_RULE_CAP_BANG
                                                          : (int)(HS_RULE_COUNT - 1);
    const bool do_essid  = (scope != POOM_HS_SCOPE_QUICK);

    memset(out, 0, sizeof(*out));
    out->total = (uint32_t)HS_WORDLIST_LEN;

    hs_target_t *targets = (hs_target_t *)calloc(HS_MAX_TARGETS, sizeof(hs_target_t));
    if (targets == NULL)
    {
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "Out of memory");
        return ESP_OK;
    }

    size_t ntargets = 0U;
    (void)hs_load_targets_(path, targets, &ntargets, out);
    if (out->error)
    {
        free(targets);
        return ESP_OK;
    }

    /* ---- count total candidates (base words + ESSID, across the ruleset) --- */
    char cand[80];
    uint32_t total = 0U;
    for (int r = first_rule; r <= last_rule; r++)
    {
        for (uint32_t w = 0U; w < HS_WORDLIST_LEN; w++)
        {
            if (hs_apply_rule_((hs_rule_t)r, s_wordlist[w], cand, sizeof(cand)) != 0U)
            {
                total++;
            }
        }
    }
    if (do_essid)
    {
        for (size_t ti = 0U; ti < ntargets; ti++)
        {
            char essid_str[HS_ESSID_MAX + 1];
            memcpy(essid_str, targets[ti].essid, targets[ti].essid_len);
            essid_str[targets[ti].essid_len] = '\0';
            for (int r = 0; r < HS_RULE_COUNT; r++)
            {
                if (hs_apply_rule_((hs_rule_t)r, essid_str, cand, sizeof(cand)) != 0U)
                {
                    total++;
                }
            }
        }
    }
    out->total = total;

    /* ---- run: base words x rules (rule-major), then ESSID-derived ---- */
    const int64_t t_start = esp_timer_get_time();

    for (int r = first_rule; (r <= last_rule) && !out->found && !out->cancelled; r++)
    {
        for (uint32_t w = 0U; w < HS_WORDLIST_LEN; w++)
        {
            if ((cancel != NULL) && *cancel)
            {
                out->cancelled = true;
                break;
            }
            size_t cl = hs_apply_rule_((hs_rule_t)r, s_wordlist[w], cand, sizeof(cand));
            if (cl == 0U)
            {
                continue;
            }
            out->tried++;
            if (hs_try_passphrase_(cand, cl, targets, ntargets, out))
            {
                break;
            }
            if (cb != NULL)
            {
                cb(out->tried, out->total, user);
            }
        }
    }

    /* ESSID-derived candidates (the SSID is known; people reuse it). Each is
     * tried only against the target it was derived from. Skipped in QUICK. */
    for (size_t ti = 0U; do_essid && (ti < ntargets) && !out->found && !out->cancelled; ti++)
    {
        char essid_str[HS_ESSID_MAX + 1];
        memcpy(essid_str, targets[ti].essid, targets[ti].essid_len);
        essid_str[targets[ti].essid_len] = '\0';
        for (int r = 0; r < HS_RULE_COUNT; r++)
        {
            if ((cancel != NULL) && *cancel)
            {
                out->cancelled = true;
                break;
            }
            size_t cl = hs_apply_rule_((hs_rule_t)r, essid_str, cand, sizeof(cand));
            if (cl == 0U)
            {
                continue;
            }
            out->tried++;
            if (hs_try_passphrase_(cand, cl, &targets[ti], 1U, out))
            {
                break;
            }
            if (cb != NULL)
            {
                cb(out->tried, out->total, user);
            }
        }
    }

    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    free(targets);
    return ESP_OK;
}

/* ----- public: run against an SD-card wordlist --------------------------- */

/* Wordlist line buffer: a WPA PSK is 8..63 chars, so 128 leaves ample room for
 * CR/LF and makes every longer line an automatic (filtered) skip. */
#define HS_WL_LINE_MAX (128U)

esp_err_t poom_hs_crack_run_file_wordlist(const char               *hs_path,
                                          const char               *wordlist_path,
                                          const volatile bool       *cancel,
                                          poom_hs_crack_progress_fn  cb,
                                          void                      *user,
                                          poom_hs_crack_result_t    *out)
{
    if ((hs_path == NULL) || (wordlist_path == NULL) || (out == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    hs_target_t *targets = (hs_target_t *)calloc(HS_MAX_TARGETS, sizeof(hs_target_t));
    char        *wbuf    = (char *)malloc(HS_WL_LINE_MAX);
    if ((targets == NULL) || (wbuf == NULL))
    {
        free(targets);
        free(wbuf);
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "Out of memory");
        return ESP_OK;
    }

    size_t ntargets = 0U;
    (void)hs_load_targets_(hs_path, targets, &ntargets, out);
    if (out->error)
    {
        free(targets);
        free(wbuf);
        return ESP_OK;
    }

    FILE *wf = fopen(wordlist_path, "r");
    if (wf == NULL)
    {
        free(targets);
        free(wbuf);
        out->error = true;
        (void)snprintf(out->error_msg, sizeof(out->error_msg), "Cannot open list");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "wordlist %s", wordlist_path);

    /* Pass 1: count WPA-valid lines so the progress bar has a real total. This
     * is pure file I/O (no crypto) and cheap next to ~16k hashes per guess.
     * Report progress with total==0 as a "still counting" sentinel (the first
     * callback arg carries the running valid-word count) and honour cancel, so
     * a huge wordlist neither looks frozen nor ignores B:CANCEL in this pass. */
    uint32_t total   = 0U;
    uint32_t scanned = 0U;
    size_t   wl;
    while ((wl = hs_wl_readline_(wf, wbuf, HS_WL_LINE_MAX)) != (size_t)-1)
    {
        if ((cancel != NULL) && *cancel)
        {
            out->cancelled = true;
            break;
        }
        scanned++;
        if ((wl >= 8U) && (wl <= 63U))
        {
            total++;
        }
        if ((cb != NULL) && ((scanned & 0x3FFU) == 0U))
        {
            cb(total, 0U, user);
        }
    }
    out->total = total;
    ESP_LOGI(TAG, "wordlist candidates: %u", (unsigned)total);

    /* Pass 2: try each WPA-valid line as-is (rules are not applied to SD-card
     * lists -- a real wordlist is already large / pre-mutated). */
    rewind(wf);
    const int64_t t_start = esp_timer_get_time();
    while (!out->found && !out->cancelled &&
           ((wl = hs_wl_readline_(wf, wbuf, HS_WL_LINE_MAX)) != (size_t)-1))
    {
        if ((cancel != NULL) && *cancel)
        {
            out->cancelled = true;
            break;
        }
        if ((wl < 8U) || (wl > 63U))
        {
            continue;
        }
        out->tried++;
        if (hs_try_passphrase_(wbuf, wl, targets, ntargets, out))
        {
            break;
        }
        if (cb != NULL)
        {
            cb(out->tried, out->total, user);
        }
    }

    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    (void)fclose(wf);
    free(wbuf);
    free(targets);
    return ESP_OK;
}
