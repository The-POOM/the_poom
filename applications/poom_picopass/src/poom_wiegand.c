// SPDX-License-Identifier: GPL-3.0-or-later
// Wiegand credential decode. See poom_wiegand.h.

#include "poom_wiegand.h"
#include <string.h>

typedef struct
{
    uint8_t length;  // number of encoded bits (excludes sentinel)
    uint32_t top;    // bits 64..
    uint32_t mid;    // bits 32..63
    uint32_t bot;    // bits 0..31
} wmo_t;

// Parity bit to append (HID Wiegand convention):
//   oddparity32(x)  -> bit that makes the total number of 1s odd
//   evenparity32(x) -> bit that makes the total number of 1s even
static uint8_t oddparity32(uint32_t x)
{
    return (uint8_t)(__builtin_parity(x) ^ 1u);  // 1 when x has an even count
}
static uint8_t evenparity32(uint32_t x)
{
    return (uint8_t)__builtin_parity(x);  // 1 when x has an odd count
}

// Bit at ordinal position `pos` (0 = highest transmitted bit).
static uint8_t get_bit(const wmo_t* w, uint8_t pos)
{
    if(pos >= w->length)
        return 0;
    pos = (uint8_t)((w->length - pos) - 1);  // invert to bit weight
    if(pos > 63)
        return (uint8_t)((w->top >> (pos - 64)) & 1);
    else if(pos > 31)
        return (uint8_t)((w->mid >> (pos - 32)) & 1);
    else
        return (uint8_t)((w->bot >> pos) & 1);
}

static uint64_t get_field(const wmo_t* w, uint8_t first_bit, uint8_t len)
{
    uint64_t r = 0;
    for(uint8_t i = 0; i < len; i++)
    {
        r = (r << 1) | get_bit(w, (uint8_t)(first_bit + i));
    }
    return r;
}

// Each unpacker returns: 0 = length doesn't match this format, 1 = length
// matches but parity failed, 2 = length matches and parity is valid. On 1 or 2
// it fills `out` with the (attempted) format/FC/CN.

static int unpack_h10301(const wmo_t* w, poom_wiegand_result_t* out)
{
    if(w->length != 26)
        return 0;
    out->format        = "H10301";
    out->card_number   = (w->bot >> 1) & 0xFFFF;
    out->facility_code = (w->bot >> 17) & 0xFF;
    bool ok            = (oddparity32((w->bot >> 1) & 0xFFF) == (w->bot & 1)) &&
              (evenparity32((w->bot >> 13) & 0xFFF) == ((w->bot >> 25) & 1));
    return ok ? 2 : 1;
}

static int unpack_c1k35s(const wmo_t* w, poom_wiegand_result_t* out)
{
    if(w->length != 35)
        return 0;
    out->format        = "C1k35s";
    out->card_number   = (w->bot >> 1) & 0x000FFFFF;
    out->facility_code = ((w->mid & 1) << 11) | (w->bot >> 21);
    bool ok =
        (evenparity32((w->mid & 0x1) ^ (w->bot & 0xB6DB6DB6)) ==
         ((w->mid >> 1) & 1)) &&
        (oddparity32((w->mid & 0x3) ^ (w->bot & 0x6DB6DB6C)) == (w->bot & 1)) &&
        (oddparity32((w->mid & 0x3) ^ (w->bot & 0xFFFFFFFF)) ==
         ((w->mid >> 2) & 1));
    return ok ? 2 : 1;
}

static int unpack_h10302(const wmo_t* w, poom_wiegand_result_t* out)
{
    if(w->length != 37)
        return 0;
    out->format        = "H10302";
    out->card_number   = get_field(w, 1, 35);
    out->facility_code = 0;  // no facility code in this format
    bool ok            = (get_bit(w, 0) == evenparity32(get_field(w, 1, 18))) &&
              (get_bit(w, 36) == oddparity32(get_field(w, 18, 18)));
    return ok ? 2 : 1;
}

static int unpack_h10304(const wmo_t* w, poom_wiegand_result_t* out)
{
    if(w->length != 37)
        return 0;
    out->format        = "H10304";
    out->facility_code = (uint32_t)get_field(w, 1, 16);
    out->card_number   = get_field(w, 17, 19);
    bool ok            = (get_bit(w, 0) == evenparity32(get_field(w, 1, 18))) &&
              (get_bit(w, 36) == oddparity32(get_field(w, 18, 18)));
    return ok ? 2 : 1;
}

int poom_wiegand_decode(const uint8_t credential[8],
                        uint8_t bit_length,
                        poom_wiegand_result_t* out,
                        int max,
                        bool* any_length_match)
{
    // credential is big-endian on the wire; low 64 bits hold the field.
    uint64_t v = 0;
    for(int i = 0; i < 8; i++)
        v = (v << 8) | credential[i];

    wmo_t w = {
        .length = bit_length,
        .top    = 0,
        .mid    = (uint32_t)(v >> 32),
        .bot    = (uint32_t)(v & 0xFFFFFFFF),
    };

    static int (*const unpackers[])(const wmo_t*, poom_wiegand_result_t*) = {
        unpack_h10301, unpack_c1k35s, unpack_h10302, unpack_h10304};

    if(any_length_match)
        *any_length_match = false;
    int n = 0;
    for(size_t i = 0; i < sizeof(unpackers) / sizeof(unpackers[0]); i++)
    {
        poom_wiegand_result_t r;
        int s = unpackers[i](&w, &r);
        if(s >= 1 && any_length_match)
            *any_length_match = true;
        if(s == 2 && n < max)
            out[n++] = r;
    }
    return n;
}
