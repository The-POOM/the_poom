// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "poom_pcap_sae_filter.h"

int main(void)
{
    uint8_t frame[64] = {0};
    frame[0] = 0xB0U;
    frame[24] = 3U; /* SAE */
    frame[26] = 1U; /* Commit */

    assert(!poom_pcap_wifi_is_sae_auth(NULL, sizeof(frame)));
    for(size_t len = 0U; len < 30U; ++len)
    {
        assert(!poom_pcap_wifi_is_sae_auth(frame, len));
    }
    assert(poom_pcap_wifi_is_sae_auth(frame, 30U));
    assert(poom_pcap_wifi_is_sae_auth(frame, sizeof(frame)));

    /* Confirm and anti-clogging/error responses are authentication traffic too. */
    frame[26] = 2U;
    assert(poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[26] = 1U;
    frame[28] = 76U;
    assert(poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[28] = 0U;

    /* Similar-looking bytes in data, beacon, deauth, or a nonzero version
     * must not be classified as an SAE Authentication frame. */
    for(unsigned fc0 = 0U; fc0 < 256U; ++fc0)
    {
        frame[0] = (uint8_t)fc0;
        assert(poom_pcap_wifi_is_sae_auth(frame, 30U) == (fc0 == 0xB0U));
    }
    frame[0] = 0xB0U;
    for(unsigned algorithm = 0U; algorithm < 256U; ++algorithm)
    {
        frame[24] = (uint8_t)algorithm;
        assert(poom_pcap_wifi_is_sae_auth(frame, 30U) == (algorithm == 3U));
    }
    frame[24] = 3U;
    frame[25] = 1U; /* Must check both bytes of the LE16 algorithm. */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[25] = 0U;

    frame[1] = 0x40U; /* protected */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[1] = 1U; /* ToDS */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[1] = 2U; /* FromDS */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[1] = 8U; /* retry is valid */
    assert(poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[22] = 1U; /* non-initial fragment */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    frame[22] = 0U;

    frame[1] = 0x80U; /* +HTC extends the header by four bytes. */
    assert(!poom_pcap_wifi_is_sae_auth(frame, 30U));
    assert(!poom_pcap_wifi_is_sae_auth(frame, 34U)); /* algorithm is in HTC */
    memset(frame + 24U, 0, 10U);
    frame[28] = 3U;
    frame[30] = 1U;
    for(size_t len = 24U; len < 34U; ++len)
    {
        assert(!poom_pcap_wifi_is_sae_auth(frame, len));
    }
    assert(poom_pcap_wifi_is_sae_auth(frame, 34U));

    puts("SAE filter: truncated, wrong type/algorithm, flags, fragments and HTC cases passed");
    return 0;
}
