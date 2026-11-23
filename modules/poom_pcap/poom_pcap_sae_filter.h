// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_PCAP_SAE_FILTER_H
#define POOM_PCAP_SAE_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* frame_len excludes the FCS. Capture SAE authentication exchanges, including
 * commit, confirm and rejection/token responses, without decoding their body.
 * No ESP-IDF types are used so malformed/truncated packets can be host tested. */
static inline bool poom_pcap_wifi_is_sae_auth(const uint8_t *frame, size_t frame_len)
{
    size_t body_offset = 24U;

    if ((frame == NULL) || (frame_len < body_offset))
    {
        return false;
    }

    /* Version 0, management type, Authentication subtype. Do not interpret
     * encrypted payloads, DS data headers or non-initial fragments as auth. */
    if ((frame[0] != 0xB0U) || ((frame[1] & 0x43U) != 0U) ||
        ((frame[22] & 0x0FU) != 0U))
    {
        return false;
    }

    if ((frame[1] & 0x80U) != 0U)
    {
        body_offset += 4U; /* +HTC/Order: skip the HT Control field. */
    }

    /* Fixed authentication fields: algorithm, transaction and status (LE16).
     * Require all six bytes; a trailing FCS is never part of these fields. */
    if (frame_len < body_offset + 6U)
    {
        return false;
    }
    return (frame[body_offset] == 3U) && (frame[body_offset + 1U] == 0U);
}

#endif /* POOM_PCAP_SAE_FILTER_H */
