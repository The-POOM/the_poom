// SPDX-License-Identifier: GPL-3.0-or-later
/* Production detector/parser with no BLE controller or radio access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "poom_ble_detector.h"
#include "poom_ble_scan.h"

static uint64_t now_ms = 1000U;
static poom_ble_scan_result_cb_t scan_cb;
static poom_ble_detector_record_t received;
static unsigned callbacks, scan_starts, scan_stops;
static esp_err_t start_result;
static const uint8_t mac[6] = {2,3,4,5,6,7};

int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000LL; }
void vTaskDelay(unsigned ticks) { (void)ticks; assert(!"Unexpected in-flight callback in sequential host test"); }
void poom_ble_scan_register_cb(poom_ble_scan_result_cb_t cb) { scan_cb = cb; }
void poom_ble_scan_set_filter_type(esp_ble_scan_filter_t type) { assert(type == BLE_SCAN_FILTER_ALLOW_ALL); }
void poom_ble_scan_set_scan_type(esp_ble_scan_type_t type) { assert(type == BLE_SCAN_TYPE_PASSIVE); }
void poom_ble_scan_set_uart_forward_enabled(bool enabled) { assert(!enabled); }
esp_err_t poom_ble_scan_start(void) { ++scan_starts; return start_result; }
esp_err_t poom_ble_scan_stop(void) { ++scan_stops; return ESP_OK; }
static void record_cb(const poom_ble_detector_record_t *r) { received = *r; ++callbacks; }

static size_t ad(uint8_t *out, uint8_t type, const void *data, size_t n)
{
    assert(n <= 29U); out[0] = (uint8_t)(n + 1U); out[1] = type;
    if(n) memcpy(out + 2U, data, n);
    return n + 2U;
}
static poom_ble_detector_record_t parse(const uint8_t *adv, size_t n, const uint8_t *rsp, size_t m)
{
    poom_ble_detector_record_t r;
    assert(poom_ble_detector_parse_parts(adv, n, rsp, m, mac, -57, &r));
    assert(memcmp(r.mac_address, mac, 6) == 0 && r.rssi == -57);
    assert(r.adv_data_length == n + m && r.primary_adv_length == n);
    return r;
}
static void assert_filter(const poom_ble_detector_record_t *r, poom_ble_detector_filter_t f, bool expected)
{ assert(poom_ble_detector_record_matches_filter(r, f) == expected); }

static void test_ibeacon(void)
{
    uint8_t payload[25] = {0x4c,0x00,0x02,0x15};
    for(unsigned i = 0; i < 16; ++i) payload[4 + i] = (uint8_t)i;
    payload[20] = 0x12; payload[21] = 0x34; payload[22] = 0xab; payload[23] = 0xcd; payload[24] = 0xc5;
    uint8_t adv[31]; const size_t n = ad(adv, 0xff, payload, sizeof(payload));
    poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
    assert_filter(&r, POOM_BLE_DETECTOR_FILTER_IBEACON, true);
    assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FINDMY, false);
    assert(r.ibeacon_major == 0x1234 && r.ibeacon_minor == 0xabcd && r.ibeacon_power == -59);
    for(unsigned i = 0; i < 16; ++i) assert(r.ibeacon_uuid[i] == i);
    /* A signature inside another AD field is not manufacturer data. */
    adv[1] = 0x09; r = parse(adv, n, NULL, 0); assert(!r.signatures);
    adv[1] = 0xff;
    for(size_t size = 0; size < n; ++size) { r = parse(adv, size, NULL, 0); assert(!r.signatures); }
    payload[3] = 0x14; ad(adv, 0xff, payload, sizeof(payload));
    r = parse(adv, n, NULL, 0); assert_filter(&r, POOM_BLE_DETECTOR_FILTER_IBEACON, false);
}

static void test_findmy(void)
{
    uint8_t payload[29] = {0x4c,0,0x12,0x19}, adv[31], rsp[31];
    size_t n = ad(adv, 0xff, payload, sizeof(payload));
    poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
    assert(r.signatures & POOM_BLE_SIGNATURE_FINDMY);
    assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FINDMY, true);
    assert(r.confidence == POOM_DETECT_CONFIDENCE_POSSIBLE);
    for(size_t size = 0; size < n; ++size) {
        r = parse(adv, size, NULL, 0); assert(!(r.signatures & POOM_BLE_SIGNATURE_FINDMY));
    }
    /* Truncated Apple data and unrelated Apple subtypes do not match. */
    n = ad(adv, 0xff, payload, 4); r = parse(adv, n, NULL, 0); assert(!r.signatures);
    payload[2] = 0x1e; n = ad(adv, 0xff, payload, sizeof(payload));
    r = parse(adv, n, NULL, 0); assert(!r.signatures);
    const uint8_t fmna[] = {0x44,0xfd}; n = ad(adv, 3, fmna, 2);
    r = parse(adv, n, NULL, 0); assert(r.signatures & POOM_BLE_SIGNATURE_FMNA);
    assert_filter(&r, POOM_BLE_DETECTOR_FILTER_TRACKERS, true);
    const uint8_t dult[16] = {0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,0x26,0xc2,0xf4,0x12,0x01,0,0x19,0x15};
    const size_t m = ad(rsp, 7, dult, 16); r = parse(adv, n, rsp, m);
    assert((r.signatures & (POOM_BLE_SIGNATURE_FMNA | POOM_BLE_SIGNATURE_DULT)) ==
           (POOM_BLE_SIGNATURE_FMNA | POOM_BLE_SIGNATURE_DULT));
    rsp[17] ^= 1U; r = parse(NULL, 0, rsp, m); assert(!r.signatures);
    uint8_t base[16] = {0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0,0x44,0xfd,0,0};
    n = ad(adv, 7, base, 16); r = parse(adv, n, NULL, 0); assert(r.signatures & POOM_BLE_SIGNATURE_FMNA);
    adv[2] ^= 1U; r = parse(adv, n, NULL, 0); assert(!r.signatures);
}

static void test_flipper_meta_hc(void)
{
    uint8_t adv[31], rsp[31], id[2];
    for(unsigned i = 0x3081; i <= 0x3083; ++i) {
        id[0] = (uint8_t)i; id[1] = (uint8_t)(i >> 8);
        size_t n = ad(adv, 3, id, 2);
        poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FLIPPER, true); assert(r.identifier == i);
        adv[1] = 0xff; r = parse(adv, n, NULL, 0); assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FLIPPER, false);
    }
    const uint16_t meta[] = {0xfd5f,0xfeb7,0xfeb8,0x01ab,0x058e,0x0d53};
    for(size_t i = 0; i < sizeof(meta)/sizeof(meta[0]); ++i) {
        id[0] = (uint8_t)meta[i]; id[1] = (uint8_t)(meta[i] >> 8);
        const size_t n = ad(adv, i & 1U ? 0xff : 0x16, id, 2);
        poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_META, true);
        const uint8_t apple[] = {0x4c,0}; const size_t m = ad(rsp, 0xff, apple, 2);
        r = parse(adv, n, rsp, m); assert_filter(&r, POOM_BLE_DETECTOR_FILTER_META, false);
    }
    const char *names[] = {"HC-03", "HC-05", "HC-06"};
    for(size_t i = 0; i < 3; ++i) {
        size_t n = ad(adv, 9, names[i], strlen(names[i]));
        poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_HC_NAMES, true);
        assert(r.device_class == POOM_BLE_DETECTOR_CLASS_UNKNOWN);
    }
}

static void test_flock_and_boundaries(void)
{
    uint8_t adv[31], rsp[31];
    const uint8_t vendor[] = {0xc8,0x09,'T','N','1','2','#','3','-', '4','!','9'};
    const size_t n = ad(adv, 0xff, vendor, sizeof(vendor));
    const char *valid[] = {"Penguin-0123456789", "FS Ext Battery", "0123456789", ""};
    for(size_t i = 0; i < 4; ++i) {
        const size_t m = ad(rsp, 9, valid[i], strlen(valid[i]));
        poom_ble_detector_record_t r = parse(adv, n, rsp, m);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FLOCK, true);
        assert(strcmp(r.flock_serial, "TN1234") == 0);
    }
    const char *invalid[] = {"Penguin-012345678x", "Other vendor", "01234567890"};
    for(size_t i = 0; i < 3; ++i) {
        size_t m = ad(rsp, 9, invalid[i], strlen(invalid[i]));
        poom_ble_detector_record_t r = parse(adv, n, rsp, m);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_FLOCK, false); assert(!r.flock_serial[0]);
    }
    /* Advertising padding cannot hide a valid scan response. */
    const uint8_t fmna[] = {0x44,0xfd}; const size_t m = ad(rsp, 3, fmna, 2);
    memset(adv, 0, sizeof(adv)); poom_ble_detector_record_t r = parse(adv, 31, rsp, m);
    assert(r.signatures & POOM_BLE_SIGNATURE_FMNA);
    /* Nor can an incomplete AD field borrow bytes from that response. */
    adv[0] = 6; adv[1] = 3; r = parse(adv, 2, rsp, m);
    assert(r.signatures == POOM_BLE_SIGNATURE_FMNA);
    adv[0] = 3; adv[1] = 3; adv[2] = 0x81; rsp[0] = 0x30;
    r = parse(adv, 3, rsp, 1); assert(!r.signatures);
    assert(!poom_ble_detector_parse_parts(NULL, 1, NULL, 0, mac, 0, &r));
    assert(!poom_ble_detector_parse_parts(adv, 32, NULL, 0, mac, 0, &r));
    assert(!poom_ble_detector_parse_parts(adv, 1, NULL, 1, mac, 0, &r));
    /* Complete names beat shortened names in either part; control bytes are escaped. */
    size_t a = ad(adv, 9, "Full\nName", 9); size_t b = ad(rsp, 8, "Short", 5);
    r = parse(adv, a, rsp, b); assert(strcmp(r.name, "Full?Name") == 0);
}

static void test_lifecycle_and_rates(void)
{
    poom_ble_detector_stats_t stats;
    poom_ble_detector_register_cb(record_cb);
    assert(poom_ble_detector_start((poom_ble_detector_filter_t)-1) == ESP_ERR_INVALID_ARG);
    assert(poom_ble_detector_start(POOM_BLE_DETECTOR_FILTER_COUNT) == ESP_ERR_INVALID_ARG);
    assert(scan_starts == 0);
    assert(poom_ble_detector_start(POOM_BLE_DETECTOR_FILTER_FINDMY) == ESP_OK && scan_cb);
    assert(poom_ble_detector_start(POOM_BLE_DETECTOR_FILTER_DEVICES) == ESP_ERR_INVALID_STATE);
    esp_ble_gap_cb_param_t event = {0};
    memcpy(event.scan_rst.bda, mac, 6); event.scan_rst.rssi = -70;
    scan_cb(&event); assert(callbacks == 0);
    const uint8_t fmna[] = {0x44,0xfd}; event.scan_rst.adv_data_len = (uint8_t)ad(event.scan_rst.ble_adv, 3, fmna, 2);
    event.scan_rst.rssi = -40; scan_cb(&event); scan_cb(&event);
    assert(callbacks == 2 && received.reports == 1 && received.last_seen_ms == now_ms);
    now_ms += 1000; poom_ble_detector_get_stats(&stats);
    assert(stats.reports == 3 && stats.matched_reports == 2 && stats.rate_reports == 3 && stats.rate_window_ms == 1000);
    assert(stats.rssi_min == -70 && stats.rssi_max == -40 && stats.rssi_last == -40);
    now_ms += 1200; poom_ble_detector_get_stats(&stats); assert(stats.rate_reports == 0 && stats.rate_window_ms == 1200);
    assert(poom_ble_detector_stop() == ESP_OK && !scan_cb && scan_stops == 1);
    now_ms += 5000; poom_ble_detector_get_stats(&stats); assert(stats.elapsed_ms == 2200);
    start_result = ESP_FAIL;
    assert(poom_ble_detector_start(POOM_BLE_DETECTOR_FILTER_DEVICES) == ESP_FAIL && !scan_cb);
    start_result = ESP_OK;
    assert(poom_ble_detector_start(POOM_BLE_DETECTOR_FILTER_ANALYZER) == ESP_OK);
    scan_cb(&event); poom_ble_detector_get_stats(&stats);
    assert(stats.reports == 1 && stats.matched_reports == 1);
    assert(poom_ble_detector_stop() == ESP_OK);
    poom_ble_detector_register_cb(NULL);
}

static void test_existing_profiles(void)
{
    const uint8_t services[][2] = {{0x5a,0xfd}, {0xed,0xfe}, {0xec,0xfe}, {0x2c,0xfe}};
    uint8_t adv[31];
    for(size_t i = 0; i < sizeof(services)/sizeof(services[0]); ++i) {
        const size_t n = ad(adv, 3, services[i], 2);
        poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_TRACKERS, true);
        assert_filter(&r, POOM_BLE_DETECTOR_FILTER_DEVICES, true);
    }
    const size_t n = ad(adv, 9, "Axon Body 3", 11);
    poom_ble_detector_record_t r = parse(adv, n, NULL, 0);
    assert_filter(&r, POOM_BLE_DETECTOR_FILTER_WEARABLES, true);
    assert(!poom_ble_detector_record_matches_filter(NULL, POOM_BLE_DETECTOR_FILTER_DEVICES));
    for(unsigned i = 0; i < POOM_BLE_DETECTOR_FILTER_COUNT; ++i)
        assert(strlen(poom_ble_detector_filter_label((poom_ble_detector_filter_t)i)) <= 21U);
}

int main(void)
{
    test_ibeacon(); test_findmy(); test_flipper_meta_hc(); test_flock_and_boundaries();
    test_existing_profiles(); test_lifecycle_and_rates();
    puts("BLE recon: bounded AD fields, split reports, profile filters, iBeacon byte order and measured rates passed");
    return 0;
}
