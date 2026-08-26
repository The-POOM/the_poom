// SPDX-License-Identifier: GPL-3.0-or-later
// Wiegand credential decode for common HID formats (H10301, C1k35s, H10302,
// H10304), operating on a sentinel-stripped PicoPass credential.

#ifndef POOM_WIEGAND_H
#define POOM_WIEGAND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char* format;      // e.g. "H10301"
    uint32_t facility_code;  // 0 if the format carries none
    uint64_t card_number;
} poom_wiegand_result_t;

// Decode every known format whose length matches AND whose parity validates,
// writing up to `max` results into `out` and returning the count. Some lengths
// (37-bit H10302/H10304) are ambiguous and yield more than one match. If a
// format's length matched but none passed parity, `*any_length_match` is set
// true (caller can flag a parity error). Pass NULL for `any_length_match` to
// ignore it.
int poom_wiegand_decode(const uint8_t credential[8],
                        uint8_t bit_length,
                        poom_wiegand_result_t* out,
                        int max,
                        bool* any_length_match);

#ifdef __cplusplus
}
#endif

#endif  // POOM_WIEGAND_H
