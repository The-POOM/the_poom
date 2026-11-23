// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "poom_wifi_scanner.h"

static esp_err_t s_init_result;
static esp_err_t s_scan_result;
static esp_err_t s_read_result;
static uint16_t s_result_count = 2U;
static unsigned s_reads;
static const wifi_scan_config_t *s_expected_config;

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "TEST_ERROR";
}

esp_err_t poom_wifi_ctrl_init_sta(void) { return s_init_result; }
esp_err_t esp_wifi_clear_ap_list(void) { return ESP_OK; }

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking)
{
    assert(config == s_expected_config);
    assert(blocking);
    return s_scan_result;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    assert(*count == POOM_WIFI_SCANNER_MAX_AP);
    ++s_reads;
    /* Emulate a driver that touches output before returning an error. */
    memset(records, 0x5A, sizeof(*records));
    *count = s_result_count;
    return s_read_result;
}

static void expect_empty(void)
{
    assert(poom_wifi_scanner_get_ap_records()->count == 0U);
    assert(poom_wifi_scanner_get_ap_record(0U) == NULL);
}

static void seed_success(void)
{
    s_init_result = ESP_OK;
    s_scan_result = ESP_OK;
    s_read_result = ESP_OK;
    s_expected_config = NULL;
    s_result_count = 2U;
    assert(poom_wifi_scanner_scan() == ESP_OK);
    assert(poom_wifi_scanner_get_ap_records()->count == 2U);
    assert(poom_wifi_scanner_get_ap_record(1U) != NULL);
    assert(poom_wifi_scanner_get_ap_record(2U) == NULL);
}

int main(void)
{
    unsigned reads_before;
    wifi_scan_config_t config = {6U};

    seed_success();
    s_init_result = ESP_FAIL;
    reads_before = s_reads;
    assert(poom_wifi_scanner_scan() == ESP_FAIL);
    assert(s_reads == reads_before);
    expect_empty();

    seed_success();
    s_scan_result = ESP_FAIL; /* Includes a cancelled scan. */
    reads_before = s_reads;
    assert(poom_wifi_scanner_scan() == ESP_FAIL);
    assert(s_reads == reads_before);
    expect_empty();

    seed_success();
    s_read_result = ESP_FAIL;
    assert(poom_wifi_scanner_scan() == ESP_FAIL);
    expect_empty();

    seed_success();
    s_result_count = 0U;
    assert(poom_wifi_scanner_scan() == ESP_OK);
    expect_empty();

    seed_success();
    s_expected_config = &config;
    assert(poom_wifi_scanner_scan_configured(&config) == ESP_OK);
    assert(poom_wifi_scanner_get_ap_records()->count == 2U);
    assert(poom_wifi_scanner_clear_ap_records() == ESP_OK);
    expect_empty();

    puts("Wi-Fi scan cache: init/scan/read failures, empty scan and success recovery passed");
    return 0;
}
