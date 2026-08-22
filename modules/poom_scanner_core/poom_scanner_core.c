// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_scanner_core.c
 * @brief Channel occupancy scanner core (Wi-Fi + IEEE 802.15.4).
 *
 * Focuses only on scanning + data collection (no UI).
 */

#include "poom_scanner_core.h"

#include <string.h>

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_coexist.h"
#include "esp_err.h"
#include "esp_ieee802154.h"
#include "esp_mac.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "poom_scanner_core_ieee802154_isr.h"
#include "poom_wifi_ctrl.h"

typedef struct
{
    poom_scanner_core_mode_t mode;
    TimerHandle_t hop_timer;
    uint32_t hop_ms;
    uint8_t current_channel;
    uint8_t wifi_channel_idx;
} poom_scanner_core_state_t;

static poom_scanner_core_state_t s_sc = {
    .mode = POOM_SCANNER_CORE_MODE_NONE,
    .hop_timer = NULL,
    .hop_ms = 0U,
    .current_channel = 0U,
    .wifi_channel_idx = 0U,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Wi-Fi channel model
 *
 * ESP32-C6: 2.4GHz only (1..13).
 * ESP32-C5: dual-band; hop over a curated set of common 5GHz channels too.
 */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t s_wifi_channels[POOM_SCANNER_CORE_WIFI_CH_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
#else
static const uint8_t s_wifi_channels[POOM_SCANNER_CORE_WIFI_CH_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};
#endif

_Static_assert((sizeof(s_wifi_channels) / sizeof(s_wifi_channels[0])) == POOM_SCANNER_CORE_WIFI_CH_COUNT,
               "Wi-Fi channel table mismatch");

#define POOM_SCANNER_CORE_WIFI_CH_LUT_LEN (166U) /* up to channel 165 */
static int8_t s_wifi_ch_to_idx[POOM_SCANNER_CORE_WIFI_CH_LUT_LEN];

static uint32_t s_wifi_count[POOM_SCANNER_CORE_WIFI_CH_COUNT];
static int32_t s_wifi_rssi_sum[POOM_SCANNER_CORE_WIFI_CH_COUNT];
static int8_t s_wifi_rssi_max[POOM_SCANNER_CORE_WIFI_CH_COUNT];

static uint32_t s_ieee_count[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
static int32_t s_ieee_rssi_sum[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
static int8_t s_ieee_rssi_max[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];

static poom_scanner_core_ieee802154_isr_consumer_t s_ieee_consumer = NULL;
static void* s_ieee_consumer_user = NULL;

/**
 * @brief Initializes internal resources for this module.
 *
 * @return void
 */
static void poom_scanner_core_init_stats_nolock_(void)
{
    memset(s_wifi_count, 0, sizeof(s_wifi_count));
    memset(s_wifi_rssi_sum, 0, sizeof(s_wifi_rssi_sum));
    for(size_t i = 0; i < POOM_SCANNER_CORE_WIFI_CH_COUNT; i++)
    {
        s_wifi_rssi_max[i] = (int8_t)-127;
    }

    memset(s_ieee_count, 0, sizeof(s_ieee_count));
    memset(s_ieee_rssi_sum, 0, sizeof(s_ieee_rssi_sum));
    for(size_t i = 0; i < POOM_SCANNER_CORE_IEEE802154_CH_COUNT; i++)
    {
        s_ieee_rssi_max[i] = (int8_t)-127;
    }
}

/**
 * @brief Internal helper for `poom_scanner_core_next_channel`.
 *
 * @param[in] ch_min Parameter passed to the function.
 * @param[in] ch_max Parameter passed to the function.
 * @param[in] current Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t poom_scanner_core_next_channel_(uint8_t ch_min, uint8_t ch_max, uint8_t current)
{
    if(current < ch_min || current > ch_max)
    {
        return ch_min;
    }

    current++;
    if(current > ch_max)
    {
        current = ch_min;
    }
    return current;
}

/**
 * @brief Internal helper for `poom_scanner_core_wifi_idx`.
 *
 * @param[in] channel Parameter passed to the function.
 * @return int
 */
static int poom_scanner_core_wifi_idx_(uint8_t channel)
{
    if(channel >= POOM_SCANNER_CORE_WIFI_CH_LUT_LEN)
    {
        return -1;
    }

    const int8_t idx = s_wifi_ch_to_idx[channel];
    return (idx >= 0) ? (int)idx : -1;
}

/**
 * @brief Internal helper for `poom_scanner_core_ieee_idx`.
 *
 * @param[in] channel Parameter passed to the function.
 * @return int
 */
static int poom_scanner_core_ieee_idx_(uint8_t channel)
{
    if(channel < POOM_SCANNER_CORE_IEEE802154_CH_MIN || channel > POOM_SCANNER_CORE_IEEE802154_CH_MAX)
    {
        return -1;
    }
    return (int)(channel - POOM_SCANNER_CORE_IEEE802154_CH_MIN);
}

/**
 * @brief Internal helper for `poom_scanner_core_wifi_build_lut`.
 *
 * @return void
 */
static void poom_scanner_core_wifi_build_lut_(void)
{
    (void)memset(s_wifi_ch_to_idx, -1, sizeof(s_wifi_ch_to_idx));
    for(uint8_t i = 0; i < (uint8_t)POOM_SCANNER_CORE_WIFI_CH_COUNT; i++)
    {
        const uint8_t ch = s_wifi_channels[i];
        if(ch < POOM_SCANNER_CORE_WIFI_CH_LUT_LEN)
        {
            s_wifi_ch_to_idx[ch] = (int8_t)i;
        }
    }
}

/**
 * @brief Internal helper for `poom_scanner_core_wifi_rssi_dbm`.
 *
 * @param[in] pkt Parameter passed to the function.
 * @return int8_t
 */
static int8_t poom_scanner_core_wifi_rssi_dbm_(const wifi_promiscuous_pkt_t* pkt)
{
    int rssi = (pkt != NULL) ? (int)pkt->rx_ctrl.rssi : -127;
    if(rssi > 0)
    {
        rssi = 0;
    }
    if(rssi < -127)
    {
        rssi = -127;
    }
    return (int8_t)rssi;
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] buf Parameter passed to the function.
 * @param[in] type Parameter passed to the function.
 * @return void
 */
static void poom_scanner_core_wifi_promisc_cb_(void* buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;

    if(s_sc.mode != POOM_SCANNER_CORE_MODE_WIFI)
    {
        return;
    }
    if(pkt == NULL || type == WIFI_PKT_MISC)
    {
        return;
    }

    const uint8_t ch = pkt->rx_ctrl.channel;
    const int idx = poom_scanner_core_wifi_idx_(ch);
    if(idx < 0)
    {
        return;
    }

    const int8_t rssi = poom_scanner_core_wifi_rssi_dbm_(pkt);

    portENTER_CRITICAL(&s_lock);
    s_wifi_count[idx]++;
    s_wifi_rssi_sum[idx] += (int32_t)rssi;
    if(rssi > s_wifi_rssi_max[idx])
    {
        s_wifi_rssi_max[idx] = rssi;
    }
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] tmr Parameter passed to the function.
 * @return void
 */
static void poom_scanner_core_hop_timer_cb_(TimerHandle_t tmr)
{
    (void)tmr;

    const poom_scanner_core_mode_t mode = s_sc.mode;
    if(mode == POOM_SCANNER_CORE_MODE_WIFI)
    {
        s_sc.wifi_channel_idx = (uint8_t)((s_sc.wifi_channel_idx + 1U) % (uint8_t)POOM_SCANNER_CORE_WIFI_CH_COUNT);
        s_sc.current_channel = s_wifi_channels[s_sc.wifi_channel_idx];
        (void)esp_wifi_set_channel(s_sc.current_channel, WIFI_SECOND_CHAN_NONE);
    }
    else if(mode == POOM_SCANNER_CORE_MODE_IEEE802154)
    {
        const uint8_t next = poom_scanner_core_next_channel_(
            POOM_SCANNER_CORE_IEEE802154_CH_MIN,
            POOM_SCANNER_CORE_IEEE802154_CH_MAX,
            s_sc.current_channel);
        s_sc.current_channel = next;
        (void)esp_ieee802154_set_channel(next);
        (void)esp_ieee802154_receive();
    }
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] hop_ms Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_scanner_core_start_hop_timer_(uint32_t hop_ms)
{
    if(hop_ms == 0U)
    {
        hop_ms = 200U;
    }

    if(s_sc.hop_timer == NULL)
    {
        s_sc.hop_timer = xTimerCreate("poom_scan_hop",
                                      pdMS_TO_TICKS(hop_ms),
                                      pdTRUE,
                                      NULL,
                                      poom_scanner_core_hop_timer_cb_);
        if(s_sc.hop_timer == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    s_sc.hop_ms = hop_ms;
    (void)xTimerChangePeriod(s_sc.hop_timer, pdMS_TO_TICKS(hop_ms), 0);
    (void)xTimerStart(s_sc.hop_timer, 0);
    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_scanner_core_stop_hop_timer_(void)
{
    if(s_sc.hop_timer != NULL)
    {
        (void)xTimerStop(s_sc.hop_timer, portMAX_DELAY);
        (void)xTimerDelete(s_sc.hop_timer, portMAX_DELAY);
        s_sc.hop_timer = NULL;
    }
    s_sc.hop_ms = 0U;
}

void poom_scanner_core_reset_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    poom_scanner_core_init_stats_nolock_();
    portEXIT_CRITICAL(&s_lock);
}

poom_scanner_core_mode_t poom_scanner_core_get_mode(void)
{
    return s_sc.mode;
}

bool poom_scanner_core_get_wifi_stats(poom_scanner_core_wifi_stats_t* out)
{
    if(out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->channel_min = s_wifi_channels[0];
    out->channel_max = s_wifi_channels[POOM_SCANNER_CORE_WIFI_CH_COUNT - 1U];
    out->channel_count = (uint8_t)POOM_SCANNER_CORE_WIFI_CH_COUNT;
    memcpy(out->channels, s_wifi_channels, sizeof(s_wifi_channels));

    portENTER_CRITICAL(&s_lock);
    out->current_channel = s_sc.current_channel;
    out->hop_ms = s_sc.hop_ms;
    for(size_t i = 0; i < POOM_SCANNER_CORE_WIFI_CH_COUNT; i++)
    {
        const uint32_t c = s_wifi_count[i];
        out->packet_count[i] = c;
        out->rssi_max_dbm[i] = s_wifi_rssi_max[i];
        out->rssi_avg_dbm[i] = (c > 0U) ? (int16_t)(s_wifi_rssi_sum[i] / (int32_t)c) : 0;
    }
    portEXIT_CRITICAL(&s_lock);

    return true;
}

bool poom_scanner_core_get_ieee802154_stats(poom_scanner_core_ieee802154_stats_t* out)
{
    if(out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->channel_min = POOM_SCANNER_CORE_IEEE802154_CH_MIN;
    out->channel_max = POOM_SCANNER_CORE_IEEE802154_CH_MAX;

    portENTER_CRITICAL(&s_lock);
    out->current_channel = s_sc.current_channel;
    out->hop_ms = s_sc.hop_ms;
    for(size_t i = 0; i < POOM_SCANNER_CORE_IEEE802154_CH_COUNT; i++)
    {
        const uint32_t c = s_ieee_count[i];
        out->packet_count[i] = c;
        out->rssi_max_dbm[i] = s_ieee_rssi_max[i];
        out->rssi_avg_dbm[i] = (c > 0U) ? (int16_t)(s_ieee_rssi_sum[i] / (int32_t)c) : 0;
    }
    portEXIT_CRITICAL(&s_lock);

    return true;
}

/**
 * @brief Internal helper for `poom_scanner_core_fill_top`.
 *
 * @param[in] packet_count Parameter passed to the function.
 * @param[in] rssi_avg_dbm Parameter passed to the function.
 * @param[in] rssi_max_dbm Parameter passed to the function.
 * @param[in] channels Parameter passed to the function.
 * @param[in] ch_min Parameter passed to the function.
 * @param[in] ch_count Parameter passed to the function.
 * @param[in] out_entries Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return size_t
 */
static size_t poom_scanner_core_fill_top_(
    const uint32_t* packet_count,
    const int16_t* rssi_avg_dbm,
    const int8_t* rssi_max_dbm,
    const uint8_t* channels,
    uint8_t ch_min,
    size_t ch_count,
    poom_scanner_core_top_channel_t* out_entries,
    size_t out_len)
{
    if(out_entries == NULL || out_len == 0U || packet_count == NULL || rssi_avg_dbm == NULL || rssi_max_dbm == NULL)
    {
        return 0U;
    }

    const size_t max_ch_count =
        (POOM_SCANNER_CORE_WIFI_CH_COUNT > POOM_SCANNER_CORE_IEEE802154_CH_COUNT)
            ? (size_t)POOM_SCANNER_CORE_WIFI_CH_COUNT
            : (size_t)POOM_SCANNER_CORE_IEEE802154_CH_COUNT;
    if(ch_count > max_ch_count)
    {
        return 0U;
    }

    uint32_t total = 0U;
    for(size_t i = 0; i < ch_count; i++)
    {
        total += packet_count[i];
    }

    size_t written = 0U;
    uint8_t used[POOM_SCANNER_CORE_WIFI_CH_COUNT > POOM_SCANNER_CORE_IEEE802154_CH_COUNT
                     ? POOM_SCANNER_CORE_WIFI_CH_COUNT
                     : POOM_SCANNER_CORE_IEEE802154_CH_COUNT] = {0};

    while(written < out_len)
    {
        int best_i = -1;
        uint32_t best_count = 0U;

        for(size_t i = 0; i < ch_count; i++)
        {
            if(used[i] != 0U)
            {
                continue;
            }

            const uint32_t c = packet_count[i];
            if((c > best_count) || ((c == best_count) && (best_i >= 0) && ((int)i < best_i)))
            {
                best_count = c;
                best_i = (int)i;
            }
            else if((c == best_count) && (best_i < 0) && (c > 0U))
            {
                best_count = c;
                best_i = (int)i;
            }
        }

        if(best_i < 0 || best_count == 0U)
        {
            break;
        }

        used[(size_t)best_i] = 1U;

        uint32_t pct_u32 = 0U;
        if(total > 0U)
        {
            pct_u32 = (uint32_t)(((uint64_t)best_count * 100ULL) / (uint64_t)total);
            if(pct_u32 > 100U)
            {
                pct_u32 = 100U;
            }
        }

        const uint8_t channel = (channels != NULL)
                                    ? channels[(size_t)best_i]
                                    : (uint8_t)(ch_min + (uint8_t)best_i);

        out_entries[written] = (poom_scanner_core_top_channel_t){
            .channel = channel,
            .packet_count = best_count,
            .packet_pct = (uint8_t)pct_u32,
            .rssi_avg_dbm = rssi_avg_dbm[best_i],
            .rssi_max_dbm = rssi_max_dbm[best_i],
        };
        written++;
    }

    return written;
}

size_t poom_scanner_core_get_wifi_top_channels(poom_scanner_core_top_channel_t* out_entries, size_t out_len)
{
    poom_scanner_core_wifi_stats_t stats;
    if(!poom_scanner_core_get_wifi_stats(&stats))
    {
        return 0U;
    }

    return poom_scanner_core_fill_top_(stats.packet_count,
                                       stats.rssi_avg_dbm,
                                       stats.rssi_max_dbm,
                                       stats.channels,
                                       stats.channel_min,
                                       POOM_SCANNER_CORE_WIFI_CH_COUNT,
                                       out_entries,
                                       out_len);
}

size_t poom_scanner_core_get_ieee802154_top_channels(poom_scanner_core_top_channel_t* out_entries, size_t out_len)
{
    poom_scanner_core_ieee802154_stats_t stats;
    if(!poom_scanner_core_get_ieee802154_stats(&stats))
    {
        return 0U;
    }

    return poom_scanner_core_fill_top_(stats.packet_count,
                                       stats.rssi_avg_dbm,
                                       stats.rssi_max_dbm,
                                       NULL,
                                       stats.channel_min,
                                       POOM_SCANNER_CORE_IEEE802154_CH_COUNT,
                                       out_entries,
                                       out_len);
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] hop_ms Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_scanner_core_start_wifi_internal_(uint32_t hop_ms)
{
    esp_err_t status;
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(esp_ieee802154_get_state() != ESP_IEEE802154_RADIO_DISABLE)
    {
        (void)esp_ieee802154_sleep();
        (void)esp_ieee802154_set_rx_when_idle(false);
        (void)esp_ieee802154_set_promiscuous(false);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
        esp_coex_ieee802154_status_disable();
#endif
        (void)esp_ieee802154_disable();
    }
#endif

    status = poom_wifi_ctrl_init_null();
    if(status != ESP_OK)
    {
        return status;
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5)
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    poom_scanner_core_wifi_build_lut_();
    s_sc.wifi_channel_idx = 0U;
    s_sc.current_channel = s_wifi_channels[0];
    (void)esp_wifi_set_channel(s_sc.current_channel, WIFI_SECOND_CHAN_NONE);

    status = esp_wifi_set_promiscuous_filter(&filter);
    if(status != ESP_OK)
    {
        return status;
    }

    status = esp_wifi_set_promiscuous_rx_cb(poom_scanner_core_wifi_promisc_cb_);
    if(status != ESP_OK)
    {
        return status;
    }

    status = esp_wifi_set_promiscuous(true);
    if(status != ESP_OK)
    {
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        return status;
    }

    return poom_scanner_core_start_hop_timer_(hop_ms);
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] hop_ms Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_scanner_core_start_ieee802154_internal_(uint32_t hop_ms)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    esp_err_t status;

    s_sc.current_channel = POOM_SCANNER_CORE_IEEE802154_CH_MIN;

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    (void)esp_coex_wifi_i154_enable();
#endif

    status = esp_ieee802154_enable();
    if(status != ESP_OK)
    {
        return status;
    }

    (void)esp_ieee802154_set_coordinator(false);
    (void)esp_ieee802154_set_promiscuous(true);
    (void)esp_ieee802154_set_rx_when_idle(true);
    (void)esp_ieee802154_set_channel(s_sc.current_channel);

    uint8_t eui64[8] = {0};
    uint8_t eui64_rev[8] = {0};
    (void)esp_read_mac(eui64, ESP_MAC_IEEE802154);
    for(int i = 0; i < 8; i++)
    {
        eui64_rev[7 - i] = eui64[i];
    }
    (void)esp_ieee802154_set_extended_address(eui64_rev);

    (void)esp_ieee802154_receive();
    return poom_scanner_core_start_hop_timer_(hop_ms);
#else
    (void)hop_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t poom_scanner_core_start_wifi(uint32_t hop_ms)
{
    esp_err_t ret;

    if(s_sc.mode != POOM_SCANNER_CORE_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_scanner_core_reset_stats();
    s_sc.mode = POOM_SCANNER_CORE_MODE_WIFI;
    ret = poom_scanner_core_start_wifi_internal_(hop_ms);
    if(ret != ESP_OK)
    {
        (void)poom_scanner_core_stop();
        return ret;
    }
    return ESP_OK;
}

esp_err_t poom_scanner_core_start_ieee802154(uint32_t hop_ms)
{
    esp_err_t ret;

    if(s_sc.mode != POOM_SCANNER_CORE_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_scanner_core_reset_stats();
    s_sc.mode = POOM_SCANNER_CORE_MODE_IEEE802154;
    ret = poom_scanner_core_start_ieee802154_internal_(hop_ms);
    if(ret != ESP_OK)
    {
        (void)poom_scanner_core_stop();
        return ret;
    }
    return ESP_OK;
}

esp_err_t poom_scanner_core_stop(void)
{
    const poom_scanner_core_mode_t prev_mode = s_sc.mode;
    if(prev_mode == POOM_SCANNER_CORE_MODE_NONE)
    {
        return ESP_OK;
    }

    s_sc.mode = POOM_SCANNER_CORE_MODE_NONE;

    poom_scanner_core_stop_hop_timer_();

    if(prev_mode == POOM_SCANNER_CORE_MODE_WIFI)
    {
        (void)esp_wifi_set_promiscuous(false);
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        (void)poom_wifi_ctrl_deinit();
    }
    else if(prev_mode == POOM_SCANNER_CORE_MODE_IEEE802154)
    {
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        (void)esp_ieee802154_sleep();
        (void)esp_ieee802154_set_rx_when_idle(false);
        (void)esp_ieee802154_set_promiscuous(false);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
        esp_coex_ieee802154_status_disable();
#endif
        (void)esp_ieee802154_disable();
#endif
    }

    s_sc.current_channel = 0U;
    return ESP_OK;
}

// ============================================================================
// IEEE 802.15.4 ISR callback + dispatch (global symbol required by driver)
// ============================================================================

esp_err_t poom_scanner_core_ieee802154_register_isr_consumer(
    poom_scanner_core_ieee802154_isr_consumer_t cb,
    void* user)
{
    if(cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if(s_ieee_consumer != NULL)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ieee_consumer = cb;
    s_ieee_consumer_user = user;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void poom_scanner_core_ieee802154_unregister_isr_consumer(
    poom_scanner_core_ieee802154_isr_consumer_t cb)
{
    portENTER_CRITICAL(&s_lock);
    if(s_ieee_consumer == cb)
    {
        s_ieee_consumer = NULL;
        s_ieee_consumer_user = NULL;
    }
    portEXIT_CRITICAL(&s_lock);
}

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t* frame, esp_ieee802154_frame_info_t* frame_info)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    BaseType_t woken = pdFALSE;

    if(frame == NULL)
    {
        return;
    }

    poom_scanner_core_ieee802154_isr_consumer_t consumer;
    void* consumer_user;

    portENTER_CRITICAL_ISR(&s_lock);
    consumer = s_ieee_consumer;
    consumer_user = s_ieee_consumer_user;
    portEXIT_CRITICAL_ISR(&s_lock);
    if(consumer != NULL)
    {
        consumer(frame, frame_info, &woken, consumer_user);
    }

    if(s_sc.mode == POOM_SCANNER_CORE_MODE_IEEE802154)
    {
        const uint8_t ch = s_sc.current_channel;
        const int idx = poom_scanner_core_ieee_idx_(ch);
        if(idx >= 0)
        {
            const uint8_t raw_len = frame[0];
            int8_t rssi = (int8_t)-127;
            if(raw_len >= 2U)
            {
                rssi = (int8_t)frame[1U + raw_len - 2U];
            }

            portENTER_CRITICAL_ISR(&s_lock);
            s_ieee_count[idx]++;
            s_ieee_rssi_sum[idx] += (int32_t)rssi;
            if(rssi > s_ieee_rssi_max[idx])
            {
                s_ieee_rssi_max[idx] = rssi;
            }
            portEXIT_CRITICAL_ISR(&s_lock);
        }
    }

    (void)esp_ieee802154_receive_handle_done(frame);
    if(s_sc.mode == POOM_SCANNER_CORE_MODE_IEEE802154)
    {
        (void)esp_ieee802154_receive();
    }

    if(woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
#else
    (void)frame;
    (void)frame_info;
#endif
}
