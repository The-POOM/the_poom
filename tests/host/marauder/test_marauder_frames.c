#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "poom_marauder_frames.h"

static unsigned checks;
#define CHECK(expression) do { ++checks; assert(expression); } while (0)

static void check_ies(const uint8_t *frame, size_t length, size_t offset)
{
    while (offset < length) {
        CHECK(length - offset >= 2);
        size_t element_length = frame[offset + 1];
        CHECK(element_length <= length - offset - 2);
        offset += 2 + element_length;
    }
    CHECK(offset == length);
}

static void test_bad_message(void)
{
    poom_marauder_frame_target_t ap = {.bssid = {2, 3, 4, 5, 6, 7}, .channel = 6};
    const uint8_t client[6] = {2, 0, 0, 0, 0, 8};
    uint8_t nonce[32], normal[256], modern[256], again[256];
    memset(nonce, 0xa7, sizeof(nonce));
    size_t n = poom_marauder_frame_bad_msg(normal, sizeof(normal), &ap, client, 0x123,
        UINT64_C(0x0102030405060708), nonce);
    CHECK(n == 153);
    CHECK(normal[0] == 8 && normal[1] == 2);
    CHECK(memcmp(normal + 4, client, 6) == 0);
    CHECK(memcmp(normal + 10, ap.bssid, 6) == 0);
    CHECK(memcmp(normal + 16, ap.bssid, 6) == 0);
    CHECK(normal[22] == 0x30 && normal[23] == 0x12);
    CHECK(memcmp(normal + 49, nonce, 32) == 0);
    for (size_t i = 0; i < 8; ++i) CHECK(normal[41 + i] == i + 1);
    CHECK(normal[35] + 36U == n);
    CHECK(normal[130] + 131U == n);
    ap.wpa3 = true;
    n = poom_marauder_frame_bad_msg(modern, sizeof(modern), &ap, client, 0, 1, nonce);
    CHECK(n == 131 && modern[35] == 95 && modern[38] == 0xcb);
    CHECK(modern[39] == 0 && modern[40] == 0);
    CHECK(modern[129] == 0 && modern[130] == 0);
    ap.wpa3 = false;
    n = poom_marauder_frame_bad_msg(again, sizeof(again), &ap, client, 0x123,
        UINT64_C(0x0102030405060708), nonce);
    CHECK(n == 153 && memcmp(normal, again, n) == 0);
    for (size_t cap = 0; cap < 153; ++cap) {
        memset(again, 0xcc, sizeof(again));
        CHECK(poom_marauder_frame_bad_msg(again, cap, &ap, client, 0, 1, nonce) == 0);
        for (size_t i = 0; i < sizeof(again); ++i) CHECK(again[i] == 0xcc);
    }
}

static void test_association_and_beacons(void)
{
    poom_marauder_frame_target_t ap = {.bssid = {2, 3, 4, 5, 6, 7}, .channel = 6, .privacy = true};
    const uint8_t client[6] = {2, 0, 0, 0, 0, 8};
    uint8_t frame[256];
    memset(ap.ssid, 'Z', sizeof(ap.ssid));
    for (size_t ssid_length = 0; ssid_length <= 32; ++ssid_length) {
        ap.ssid_len = ssid_length;
        for (int band = 0; band < 2; ++band) {
            ap.channel = band ? 44 : 6;
            memset(frame, 0xcc, sizeof(frame));
            size_t n = poom_marauder_frame_assoc_sleep(frame, sizeof(frame), &ap, client, 4095);
            CHECK(n != 0 && n < sizeof(frame));
            CHECK(frame[n] == 0xcc);
            CHECK(frame[0] == 0 && frame[1] == 0x10);
            CHECK(frame[22] == 0xf0 && frame[23] == 0xff);
            CHECK(memcmp(frame + 4, ap.bssid, 6) == 0);
            CHECK(memcmp(frame + 10, client, 6) == 0);
            CHECK(memcmp(frame + 16, ap.bssid, 6) == 0);
            CHECK(frame[29] == ssid_length);
            check_ies(frame, n, 28);
            CHECK(poom_marauder_frame_assoc_sleep(frame, n - 1, &ap, client, 0) == 0);
            for (int quiet = 0; quiet <= 1; ++quiet) {
                memset(frame, 0xcc, sizeof(frame));
                n = poom_marauder_frame_beacon(frame, sizeof(frame), &ap, 4096,
                    UINT64_C(0x0102030405060708), quiet != 0, band ? 36 : 1);
                CHECK(n != 0 && frame[n] == 0xcc);
                CHECK(frame[0] == 0x80 && frame[22] == 0 && frame[23] == 0);
                CHECK(frame[24] == 8 && frame[31] == 1);
                CHECK(memcmp(frame + 10, ap.bssid, 6) == 0);
                CHECK(memcmp(frame + 16, ap.bssid, 6) == 0);
                CHECK(frame[37] == ssid_length);
                check_ies(frame, n, 36);
                size_t ie = 38 + ssid_length + 10 + 3;
                CHECK(frame[ie] == (quiet ? 0x28 : 0x25));
                CHECK(frame[ie + 1] == (quiet ? 6 : 3));
                CHECK(frame[32] == 100 && frame[33] == 0);
                if (quiet) {
                    CHECK(frame[ie + 2] == 1 && frame[ie + 3] == 0);
                    CHECK(frame[ie + 4] == 100 && frame[ie + 5] == 0);
                    CHECK(frame[ie + 6] == 0 && frame[ie + 7] == 0);
                } else {
                    CHECK(frame[ie + 2] == 1);
                    CHECK(frame[ie + 3] == (band ? 36 : 1));
                    CHECK(frame[ie + 4] == 1);
                }
                CHECK(poom_marauder_frame_beacon(frame, n - 1, &ap, 0, 0, quiet != 0, band ? 36 : 1) == 0);
            }
        }
    }
    ap.ssid_len = 33;
    CHECK(poom_marauder_frame_assoc_sleep(frame, sizeof(frame), &ap, client, 0) == 0);
    CHECK(poom_marauder_frame_beacon(frame, sizeof(frame), &ap, 0, 0, true, 1) == 0);
    ap.ssid_len = 0;
    CHECK(poom_marauder_frame_beacon(frame, sizeof(frame), &ap, 0, 0, false, ap.channel) == 0);
}

static void test_sae_and_invalid(void)
{
    poom_marauder_frame_target_t ap = {.bssid = {2, 3, 4, 5, 6, 7}, .channel = 36};
    const uint8_t client[6] = {2, 0, 0, 0, 0, 8};
    uint8_t scalar[32], point[64], frame[256];
    memset(scalar, 0x12, sizeof(scalar)); memset(point, 0x34, sizeof(point));
    memset(frame, 0xcc, sizeof(frame));
    CHECK(poom_marauder_frame_sae(frame, sizeof(frame), &ap, client, 7, scalar, point) == 128);
    CHECK(frame[128] == 0xcc && frame[0] == 0xb0);
    CHECK(memcmp(frame + 4, ap.bssid, 6) == 0);
    CHECK(memcmp(frame + 10, client, 6) == 0);
    CHECK(frame[24] == 3 && frame[26] == 1 && frame[28] == 0 && frame[30] == 19);
    CHECK(memcmp(frame + 32, scalar, 32) == 0 && memcmp(frame + 64, point, 64) == 0);
    CHECK(poom_marauder_frame_sae(frame, 127, &ap, client, 0, scalar, point) == 0);
    CHECK(poom_marauder_frame_bad_msg(NULL, 256, &ap, client, 0, 0, scalar) == 0);
    CHECK(poom_marauder_frame_bad_msg(frame, 256, NULL, client, 0, 0, scalar) == 0);
    CHECK(poom_marauder_frame_assoc_sleep(frame, 256, &ap, NULL, 0) == 0);
    CHECK(poom_marauder_frame_sae(frame, 256, &ap, client, 0, NULL, point) == 0);
    CHECK(poom_marauder_frame_sae(frame, 256, &ap, client, 0, scalar, NULL) == 0);
}

static void test_probe_and_open_beacon(void)
{
    poom_marauder_frame_target_t ap = {.bssid = {2, 3, 4, 5, 6, 7}, .channel = 6};
    const uint8_t source[6] = {2, 1, 2, 3, 4, 5};
    uint8_t frame[256];
    memset(ap.ssid, 'Q', sizeof(ap.ssid));
    for(size_t len = 1; len <= 32; ++len) for(unsigned band = 0; band < 2; ++band) {
        ap.ssid_len = len; ap.channel = band ? 36 : 6;
        memset(frame, 0xcc, sizeof(frame));
        size_t n = poom_marauder_frame_probe(frame, sizeof(frame), &ap, source, 0xabc);
        CHECK(n == 26 + len + (band ? 10U : 19U) && frame[n] == 0xcc);
        CHECK(frame[0] == 0x40 && frame[1] == 0 && frame[25] == len);
        CHECK(memcmp(frame + 10, source, 6) == 0);
        CHECK(frame[4] == 255 && frame[16] == 255);
        CHECK(frame[22] == 0xc0 && frame[23] == 0xab);
        check_ies(frame, n, 24);
        CHECK(poom_marauder_frame_probe(frame, n - 1, &ap, source, 0) == 0);
        memset(frame, 0xcc, sizeof(frame));
        n = poom_marauder_frame_advert(frame, sizeof(frame), &ap, 4096, UINT64_C(0x0102030405060708));
        CHECK(n == 38 + len + (band ? 10U : 19U) && frame[n] == 0xcc);
        CHECK(frame[0] == 0x80 && frame[34] == 1 && frame[35] == 0);
        CHECK(frame[22] == 0 && frame[23] == 0 && frame[24] == 8 && frame[31] == 1);
        CHECK(memcmp(frame + 10, ap.bssid, 6) == 0 && memcmp(frame + 16, ap.bssid, 6) == 0);
        check_ies(frame, n, 36);
        CHECK(poom_marauder_frame_advert(frame, n - 1, &ap, 0, 0) == 0);
    }
    ap.ssid_len = 0;
    CHECK(poom_marauder_frame_probe(frame, sizeof(frame), &ap, source, 0) == 0);
    ap.ssid_len = 33;
    CHECK(poom_marauder_frame_probe(frame, sizeof(frame), &ap, source, 0) == 0);
    CHECK(poom_marauder_frame_advert(frame, sizeof(frame), &ap, 0, 0) == 0);
}

int main(void)
{
    test_bad_message();
    test_association_and_beacons();
    test_sae_and_invalid();
    test_probe_and_open_beacon();
    printf("Marauder frame builders: %u checks passed; no radio I/O.\n", checks);
    return 0;
}
