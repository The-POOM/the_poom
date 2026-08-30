// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_deauth_detector.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_wifi_ctrl.h"

#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN            (1U)
#define POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX            (POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT)
#define POOM_WIFI_DEAUTH_DETECTOR_HOP_INTERVAL_MS        (250U)
#define POOM_WIFI_DEAUTH_DETECTOR_ATTACK_DWELL_MS        (1000U)
#define POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_STACK         (2048U)
#define POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_PRIO          (4U)
#define POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT          (8U)
#define POOM_WIFI_DEAUTH_DETECTOR_TRACKER_VICTIM_SLOTS   (4U)
#define POOM_WIFI_DEAUTH_DETECTOR_TRACKER_REASON_SLOTS   (3U)

static const char *POOM_WIFI_DEAUTH_DETECTOR_TAG __attribute__((unused)) = "poom_wifi_deauth_detector";

#if CONFIG_POOM_WIFI_DEAUTH_DETECTOR_ENABLE_LOG

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_DETECTOR_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_DETECTOR_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_DETECTOR_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_DEAUTH_DETECTOR_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif

typedef struct
{
    uint16_t code;
    uint32_t count;
} poom_wifi_deauth_detector_reason_count_t;

typedef struct
{
    uint8_t bssid[6];
    uint8_t src[6];
    uint8_t last_dst[6];
    uint8_t last_channel;
    uint8_t victims[POOM_WIFI_DEAUTH_DETECTOR_TRACKER_VICTIM_SLOTS][6];
    uint16_t unique_victims;
    uint32_t window_count;
    uint32_t total_count;
    TickType_t last_seen_tick;
    bool in_use;
    poom_wifi_deauth_detector_reason_count_t reasons[POOM_WIFI_DEAUTH_DETECTOR_TRACKER_REASON_SLOTS];
} poom_wifi_deauth_detector_tracker_t;

static volatile bool s_poom_wifi_deauth_detector_running = false;
static TaskHandle_t s_poom_wifi_deauth_detector_hop_task = NULL;
static poom_wifi_deauth_detector_stats_t s_poom_wifi_deauth_detector_stats;
static poom_wifi_deauth_detector_tracker_t *s_poom_wifi_deauth_detector_trackers = NULL;
static uint32_t s_poom_wifi_deauth_detector_bad_len = 0;
static uint32_t s_poom_wifi_deauth_detector_protected = 0;
static uint32_t s_poom_wifi_deauth_detector_weird_ds = 0;
static TickType_t s_poom_wifi_deauth_detector_window_start = 0;
static uint32_t s_poom_wifi_deauth_detector_window_deauth = 0;
static uint32_t s_poom_wifi_deauth_detector_window_disassoc = 0;
static uint32_t s_poom_wifi_deauth_detector_window_bcast_deauth = 0;
static TickType_t s_poom_wifi_deauth_detector_last_activity_tick = 0;
static uint8_t s_poom_wifi_deauth_detector_last_activity_channel = 0U;
static portMUX_TYPE s_poom_wifi_deauth_detector_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_poom_wifi_deauth_detector_saved_channel = POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_HOP_SENTINEL;

static void poom_wifi_deauth_detector_window_rollover_(TickType_t now_tick);
static poom_wifi_deauth_detector_alert_t poom_wifi_deauth_detector_classify_alert_(uint32_t deauth_pps,
                                                                                    uint32_t disassoc_pps,
                                                                                    uint32_t bcast_deauth,
                                                                                    uint32_t top_pps,
                                                                                    uint16_t top_unique_victims,
                                                                                    bool top_valid);
static bool poom_wifi_deauth_detector_should_extend_dwell_(uint8_t channel, TickType_t now_tick);
static esp_err_t poom_wifi_deauth_detector_alloc_trackers_(void);
static void poom_wifi_deauth_detector_free_trackers_(void);

/**
 * @brief Internal helper for `poom_wifi_deauth_detector_is_broadcast`.
 *
 * @param[in] mac Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_deauth_detector_is_broadcast_(const uint8_t *mac)
{
    uint8_t i;
    bool all_ff = true;

    if(mac == NULL)
    {
        return false;
    }

    for(i = 0; i < 6U; i++)
    {
        if(mac[i] != 0xFFU)
        {
            all_ff = false;
            break;
        }
    }

    return all_ff || ((mac[0] & 0x01U) != 0U);
}

/**
 * @brief Internal helper for `poom_wifi_deauth_detector_reason_update`.
 *
 * @param[in] slots Parameter passed to the function.
 * @param[in] slot_count Parameter passed to the function.
 * @param[in] code Parameter passed to the function.
 * @return void
 */
static void poom_wifi_deauth_detector_reason_update_(poom_wifi_deauth_detector_reason_count_t *slots,
                                                     size_t slot_count,
                                                     uint16_t code)
{
    size_t i;
    size_t empty_idx = slot_count;
    size_t min_idx = 0;

    if((slots == NULL) || (slot_count == 0U))
    {
        return;
    }

    for(i = 0; i < slot_count; i++)
    {
        if(slots[i].count == 0U)
        {
            if(empty_idx == slot_count)
            {
                empty_idx = i;
            }
        }

        if((slots[i].count != 0U) && (slots[i].code == code))
        {
            slots[i].count++;
            return;
        }

        if(slots[i].count < slots[min_idx].count)
        {
            min_idx = i;
        }
    }

    if(empty_idx != slot_count)
    {
        slots[empty_idx].code = code;
        slots[empty_idx].count = 1U;
        return;
    }

    slots[min_idx].code = code;
    slots[min_idx].count = 1U;
}

/**
 * @brief Internal helper for `poom_wifi_deauth_detector_find_or_alloc_tracker`.
 *
 * @param[in] bssid Parameter passed to the function.
 * @param[in] src Parameter passed to the function.
 * @param[in] now_tick Parameter passed to the function.
 * @return int
 */
static int poom_wifi_deauth_detector_find_or_alloc_tracker_(const uint8_t *bssid, const uint8_t *src, TickType_t now_tick)
{
    uint8_t i;
    int free_idx = -1;
    int lru_idx = 0;
    TickType_t oldest_tick = (TickType_t)(~(TickType_t)0);

    if(s_poom_wifi_deauth_detector_trackers == NULL)
    {
        return -1;
    }

    for(i = 0; i < POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT; i++)
    {
        poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[i];
        if(t->in_use)
        {
            if((memcmp(t->bssid, bssid, 6U) == 0) && (memcmp(t->src, src, 6U) == 0))
            {
                return (int)i;
            }

            if(t->last_seen_tick < oldest_tick)
            {
                oldest_tick = t->last_seen_tick;
                lru_idx = (int)i;
            }
        }
        else if(free_idx < 0)
        {
            free_idx = (int)i;
        }
    }

    {
        const int use_idx = (free_idx >= 0) ? free_idx : lru_idx;
        poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[use_idx];
        (void)memset(t, 0, sizeof(*t));
        (void)memcpy(t->bssid, bssid, 6U);
        (void)memcpy(t->src, src, 6U);
        t->in_use = true;
        t->last_seen_tick = now_tick;
        return use_idx;
    }
}

/**
 * @brief Allocates tracker storage only while the detector is active.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_deauth_detector_alloc_trackers_(void)
{
    if(s_poom_wifi_deauth_detector_trackers != NULL)
    {
        return ESP_OK;
    }

    s_poom_wifi_deauth_detector_trackers =
        (poom_wifi_deauth_detector_tracker_t *)calloc(POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT,
                                                      sizeof(*s_poom_wifi_deauth_detector_trackers));
    if(s_poom_wifi_deauth_detector_trackers == NULL)
    {
        POOM_PRINTF_E("Failed to allocate tracker table");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Frees tracker storage when detector stops.
 *
 * @return void
 */
static void poom_wifi_deauth_detector_free_trackers_(void)
{
    free(s_poom_wifi_deauth_detector_trackers);
    s_poom_wifi_deauth_detector_trackers = NULL;
}

/**
 * @brief Advances detector channel in hopping mode.
 * @param[in,out] task_arg Task parameter (unused).
 * @return void
 */
static void poom_wifi_deauth_detector_hop_task_(void *task_arg)
{
    (void)task_arg;

    while(s_poom_wifi_deauth_detector_running)
    {
        uint8_t current_channel;

        taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
        current_channel = s_poom_wifi_deauth_detector_stats.current_channel;
        taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

        (void)poom_wifi_ctrl_set_channel(current_channel);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POOM_WIFI_DEAUTH_DETECTOR_HOP_INTERVAL_MS));
        if(!s_poom_wifi_deauth_detector_running)
        {
            break;
        }

        if(poom_wifi_deauth_detector_should_extend_dwell_(current_channel, xTaskGetTickCount()))
        {
            (void)ulTaskNotifyTake(pdTRUE,
                                   pdMS_TO_TICKS(POOM_WIFI_DEAUTH_DETECTOR_ATTACK_DWELL_MS -
                                                 POOM_WIFI_DEAUTH_DETECTOR_HOP_INTERVAL_MS));
            if(!s_poom_wifi_deauth_detector_running)
            {
                break;
            }
        }

        taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
        s_poom_wifi_deauth_detector_stats.current_channel =
            (s_poom_wifi_deauth_detector_stats.current_channel >= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX)
                ? POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN
                : (uint8_t)(s_poom_wifi_deauth_detector_stats.current_channel + 1U);
        taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    }

    s_poom_wifi_deauth_detector_hop_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Returns whether the hop task should stay longer on a channel.
 *
 * A channel is extended only when activity was seen during the current visit,
 * which keeps normal hopping fast while giving the UI time to show the hit.
 *
 * @param[in] channel Current Wi-Fi channel.
 * @param[in] now_tick Current tick count.
 * @return bool
 */
static bool poom_wifi_deauth_detector_should_extend_dwell_(uint8_t channel, TickType_t now_tick)
{
    bool extend = false;

    if((channel >= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN) && (channel <= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX))
    {
        const TickType_t recent_window = pdMS_TO_TICKS(POOM_WIFI_DEAUTH_DETECTOR_HOP_INTERVAL_MS + 50U);

        taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
        if((s_poom_wifi_deauth_detector_last_activity_channel == channel) &&
           ((now_tick - s_poom_wifi_deauth_detector_last_activity_tick) <= recent_window))
        {
            extend = true;
        }
        taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    }

    return extend;
}

/**
 * @brief Runs the internal task for this module.
 *
 * @return void
 */
static void poom_wifi_deauth_detector_stop_hop_task_(void)
{
    if(s_poom_wifi_deauth_detector_hop_task != NULL)
    {
        uint8_t wait_i;
        TaskHandle_t hop_task = s_poom_wifi_deauth_detector_hop_task;

        xTaskNotifyGive(hop_task);

        for(wait_i = 0; wait_i < 20U; wait_i++)
        {
            if(s_poom_wifi_deauth_detector_hop_task == NULL)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10U));
        }

        if(s_poom_wifi_deauth_detector_hop_task != NULL)
        {
            POOM_PRINTF_W("Hop task did not exit cleanly, force delete");
            s_poom_wifi_deauth_detector_hop_task = NULL;
            vTaskDelete(hop_task);
        }
    }
}

/**
 * @brief Runs the internal task for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_deauth_detector_start_hop_task_(void)
{
    TaskHandle_t created = NULL;

    if(!s_poom_wifi_deauth_detector_running)
    {
        return ESP_OK;
    }

    if(s_poom_wifi_deauth_detector_hop_task != NULL)
    {
        return ESP_OK;
    }

    if(xTaskCreate(poom_wifi_deauth_detector_hop_task_,
                   "poom_deauth_det_hop",
                   POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_STACK,
                   NULL,
                   POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_PRIO,
                   &created) != pdPASS)
    {
        POOM_PRINTF_E("Failed to create hop task");
        return ESP_FAIL;
    }

    s_poom_wifi_deauth_detector_hop_task = created;
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_deauth_detector_window_rollover`.
 *
 * @param[in] now_tick Parameter passed to the function.
 * @return void
 */
static void poom_wifi_deauth_detector_window_rollover_(TickType_t now_tick)
{
    const TickType_t window_ticks = pdMS_TO_TICKS(POOM_WIFI_DEAUTH_DETECTOR_PPS_WINDOW_MS);
    TickType_t elapsed;
    uint8_t i;

    if(s_poom_wifi_deauth_detector_window_start == 0)
    {
        s_poom_wifi_deauth_detector_window_start = now_tick;
        return;
    }

    elapsed = now_tick - s_poom_wifi_deauth_detector_window_start;
    if(elapsed < window_ticks)
    {
        return;
    }

    s_poom_wifi_deauth_detector_window_start = now_tick;
    s_poom_wifi_deauth_detector_window_deauth = 0;
    s_poom_wifi_deauth_detector_window_disassoc = 0;
    s_poom_wifi_deauth_detector_window_bcast_deauth = 0;

    if(s_poom_wifi_deauth_detector_trackers == NULL)
    {
        return;
    }

    for(i = 0; i < POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT; i++)
    {
        poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[i];
        if(t->in_use)
        {
            t->window_count = 0;
            t->unique_victims = 0;
            (void)memset(t->victims, 0, sizeof(t->victims));
            (void)memset(t->reasons, 0, sizeof(t->reasons));
        }
    }
}

/**
 * @brief Classifies detector activity into OLED-friendly alert levels.
 *
 * @param[in] deauth_pps Current deauth rate inside the active window.
 * @param[in] disassoc_pps Current disassoc rate inside the active window.
 * @param[in] bcast_deauth Current broadcast/multicast deauth rate.
 * @param[in] top_pps Highest single-source rate in the active window.
 * @param[in] top_unique_victims Unique victims seen for the top source.
 * @param[in] top_valid Whether the top-source fields are valid.
 * @return poom_wifi_deauth_detector_alert_t
 */
static poom_wifi_deauth_detector_alert_t poom_wifi_deauth_detector_classify_alert_(uint32_t deauth_pps,
                                                                                    uint32_t disassoc_pps,
                                                                                    uint32_t bcast_deauth,
                                                                                    uint32_t top_pps,
                                                                                    uint16_t top_unique_victims,
                                                                                    bool top_valid)
{
    const bool high_volume = (deauth_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_PPS) ||
                             (disassoc_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_PPS) ||
                             (bcast_deauth >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_BCAST_PPS);
    const bool med_volume = (deauth_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_MED_PPS) ||
                            (disassoc_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_MED_PPS) ||
                            (bcast_deauth >= POOM_WIFI_DEAUTH_DETECTOR_TH_MED_BCAST_PPS);
    const bool high_actor = top_valid &&
                            ((top_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_SRC_PPS) ||
                             ((top_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_MULTI_VICTIM_PPS) &&
                              (top_unique_victims >= POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_VICTIMS)));
    const bool med_actor = top_valid &&
                           ((top_pps >= POOM_WIFI_DEAUTH_DETECTOR_TH_MED_SRC_PPS) ||
                            ((top_pps >= 2U) &&
                             (top_unique_victims >= POOM_WIFI_DEAUTH_DETECTOR_TH_MED_VICTIMS)));

    if(high_volume || high_actor)
    {
        return POOM_WIFI_DEAUTH_DETECTOR_ALERT_HIGH;
    }

    if(med_volume || med_actor)
    {
        return POOM_WIFI_DEAUTH_DETECTOR_ALERT_MED;
    }

    return POOM_WIFI_DEAUTH_DETECTOR_ALERT_OK;
}

/**
 * @brief Handles promiscuous packets and updates deauth/disassoc counters.
 * @param[in] buffer Packet payload descriptor.
 * @param[in] type Packet category.
 * @return void
 */
static void poom_wifi_deauth_detector_promisc_cb_(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *packet;
    const uint8_t *payload;
    uint16_t frame_control;
    uint8_t frame_type;
    uint8_t frame_subtype;
    uint8_t rx_channel;
    uint8_t channel_index;
    const uint8_t *dst;
    const uint8_t *src;
    const uint8_t *bssid;
    uint16_t reason = 0U;
    bool is_deauth;
    bool is_bcast;
    uint16_t ds_bits;
    uint16_t prot_bit;
    const uint16_t header_len = 24U;
    const uint16_t need_len = 26U;
    const TickType_t now_tick = xTaskGetTickCountFromISR();
    uint16_t sig_len;
    int tracker_idx = -1;

    if((buffer == NULL) || (!s_poom_wifi_deauth_detector_running) || (type != WIFI_PKT_MGMT))
    {
        return;
    }

    packet = (const wifi_promiscuous_pkt_t *)buffer;
    payload = packet->payload;
    if(payload == NULL)
    {
        return;
    }

    sig_len = packet->rx_ctrl.sig_len;
    if(sig_len < header_len)
    {
        taskENTER_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
        s_poom_wifi_deauth_detector_bad_len++;
        taskEXIT_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
        return;
    }

    frame_control = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    frame_type = (uint8_t)((frame_control >> 2) & 0x3U);
    frame_subtype = (uint8_t)((frame_control >> 4) & 0xFU);

    if(frame_type != 0U)
    {
        return;
    }

    is_deauth = (frame_subtype == 0xCU);
    if((!is_deauth) && (frame_subtype != 0xAU))
    {
        return;
    }

    if(sig_len < need_len)
    {
        taskENTER_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
        s_poom_wifi_deauth_detector_bad_len++;
        taskEXIT_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
        return;
    }

    dst = payload + 4U;
    src = payload + 10U;
    bssid = payload + 16U;
    reason = (uint16_t)payload[24] | ((uint16_t)payload[25] << 8);

    rx_channel = packet->rx_ctrl.channel;
    if((rx_channel < POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN) || (rx_channel > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX))
    {
        taskENTER_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
        rx_channel = s_poom_wifi_deauth_detector_stats.current_channel;
        taskEXIT_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
    }
    channel_index = (uint8_t)(rx_channel - POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN);
    is_bcast = poom_wifi_deauth_detector_is_broadcast_(dst);

    ds_bits = (uint16_t)((frame_control >> 8) & 0x3U);
    prot_bit = (uint16_t)((frame_control >> 14) & 0x1U);

    taskENTER_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);
    poom_wifi_deauth_detector_window_rollover_(now_tick);

    if(ds_bits != 0U)
    {
        s_poom_wifi_deauth_detector_weird_ds++;
    }

    if(prot_bit != 0U)
    {
        s_poom_wifi_deauth_detector_protected++;
    }

    if(is_deauth)
    {
        s_poom_wifi_deauth_detector_stats.deauth_total++;
        s_poom_wifi_deauth_detector_stats.deauth_by_channel[channel_index]++;
        s_poom_wifi_deauth_detector_window_deauth++;
        if(is_bcast)
        {
            s_poom_wifi_deauth_detector_window_bcast_deauth++;
        }
    }
    else
    {
        s_poom_wifi_deauth_detector_stats.disassoc_total++;
        s_poom_wifi_deauth_detector_stats.disassoc_by_channel[channel_index]++;
        s_poom_wifi_deauth_detector_window_disassoc++;
    }

    s_poom_wifi_deauth_detector_last_activity_channel = rx_channel;
    s_poom_wifi_deauth_detector_last_activity_tick = now_tick;

    tracker_idx = poom_wifi_deauth_detector_find_or_alloc_tracker_(bssid, src, now_tick);
    if(tracker_idx >= 0)
    {
        poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[(unsigned)tracker_idx];
        t->last_seen_tick = now_tick;
        t->window_count++;
        t->total_count++;
        (void)memcpy(t->last_dst, dst, 6U);
        t->last_channel = rx_channel;

        if((!is_bcast) && (t->unique_victims < POOM_WIFI_DEAUTH_DETECTOR_TRACKER_VICTIM_SLOTS))
        {
            uint16_t vi;
            bool seen = false;
            for(vi = 0; vi < t->unique_victims; vi++)
            {
                if(memcmp(t->victims[vi], dst, 6U) == 0)
                {
                    seen = true;
                    break;
                }
            }
            if(!seen)
            {
                (void)memcpy(t->victims[t->unique_victims], dst, 6U);
                t->unique_victims++;
            }
        }

        poom_wifi_deauth_detector_reason_update_(t->reasons, POOM_WIFI_DEAUTH_DETECTOR_TRACKER_REASON_SLOTS, reason);
    }
    taskEXIT_CRITICAL_ISR(&s_poom_wifi_deauth_detector_lock);

    POOM_PRINTF_D("%s ch=%u src=%02X:%02X:%02X:%02X:%02X:%02X dst=%02X:%02X:%02X:%02X:%02X:%02X bssid=%02X:%02X:%02X:%02X:%02X:%02X r=%u%s",
                  is_deauth ? "DEAUTH" : "DISASSOC",
                  (unsigned)rx_channel,
                  src[0],
                  src[1],
                  src[2],
                  src[3],
                  src[4],
                  src[5],
                  dst[0],
                  dst[1],
                  dst[2],
                  dst[3],
                  dst[4],
                  dst[5],
                  bssid[0],
                  bssid[1],
                  bssid[2],
                  bssid[3],
                  bssid[4],
                  bssid[5],
                  (unsigned)reason,
                  (prot_bit != 0U) ? " prot" : "");
}

/**
 * @brief Clears all detector counters and sets default channel/mode.
 * @param[in,out] none Not used.
 * @return void
 */
static void poom_wifi_deauth_detector_reset_stats_internal_(void)
{
    uint8_t start_channel;
    bool channel_hopping;

    channel_hopping = (s_poom_wifi_deauth_detector_saved_channel == POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_HOP_SENTINEL);
    start_channel = channel_hopping ? POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN
                                    : (uint8_t)(s_poom_wifi_deauth_detector_saved_channel + 1U);
    if(start_channel < POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN)
    {
        start_channel = POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN;
    }
    if(start_channel > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX)
    {
        start_channel = POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX;
    }

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    (void)memset(&s_poom_wifi_deauth_detector_stats, 0, sizeof(s_poom_wifi_deauth_detector_stats));
    if(s_poom_wifi_deauth_detector_trackers != NULL)
    {
        (void)memset(s_poom_wifi_deauth_detector_trackers,
                     0,
                     sizeof(*s_poom_wifi_deauth_detector_trackers) * POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT);
    }
    s_poom_wifi_deauth_detector_bad_len = 0;
    s_poom_wifi_deauth_detector_protected = 0;
    s_poom_wifi_deauth_detector_weird_ds = 0;
    s_poom_wifi_deauth_detector_window_start = xTaskGetTickCount();
    s_poom_wifi_deauth_detector_window_deauth = 0;
    s_poom_wifi_deauth_detector_window_disassoc = 0;
    s_poom_wifi_deauth_detector_window_bcast_deauth = 0;
    s_poom_wifi_deauth_detector_last_activity_tick = 0;
    s_poom_wifi_deauth_detector_last_activity_channel = 0U;
    s_poom_wifi_deauth_detector_stats.current_channel = start_channel;
    s_poom_wifi_deauth_detector_stats.channel_hopping = channel_hopping;
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);
}

/**
 * @brief Starts passive deauth/disassoc frame detection in promiscuous mode.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_start(void)
{
    esp_err_t status;
    uint8_t start_channel;
    bool channel_hopping;

    if(s_poom_wifi_deauth_detector_running)
    {
        return ESP_OK;
    }

    status = poom_wifi_ctrl_init_sta();
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_init_sta failed: %s", esp_err_to_name(status));
        return status;
    }

    status = poom_wifi_deauth_detector_alloc_trackers_();
    if(status != ESP_OK)
    {
        return status;
    }

    poom_wifi_deauth_detector_reset_stats_internal_();

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    start_channel = s_poom_wifi_deauth_detector_stats.current_channel;
    channel_hopping = s_poom_wifi_deauth_detector_stats.channel_hopping;
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    status = poom_wifi_ctrl_set_channel(start_channel);
    if(status != ESP_OK)
    {
        poom_wifi_deauth_detector_free_trackers_();
        POOM_PRINTF_E("poom_wifi_ctrl_set_channel(%u) failed: %s",
                      (unsigned)start_channel,
                      esp_err_to_name(status));
        return status;
    }

    status = poom_wifi_ctrl_set_promiscuous_rx_cb(poom_wifi_deauth_detector_promisc_cb_);
    if(status != ESP_OK)
    {
        poom_wifi_deauth_detector_free_trackers_();
        POOM_PRINTF_E("poom_wifi_ctrl_set_promiscuous_rx_cb failed: %s", esp_err_to_name(status));
        return status;
    }

    status = poom_wifi_ctrl_set_promiscuous(true);
    if(status != ESP_OK)
    {
        (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
        poom_wifi_deauth_detector_free_trackers_();
        POOM_PRINTF_E("poom_wifi_ctrl_set_promiscuous(true) failed: %s", esp_err_to_name(status));
        return status;
    }

    s_poom_wifi_deauth_detector_running = true;

    if(channel_hopping &&
       (xTaskCreate(poom_wifi_deauth_detector_hop_task_,
                    "poom_deauth_det_hop",
                    POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_STACK,
                    NULL,
                    POOM_WIFI_DEAUTH_DETECTOR_HOP_TASK_PRIO,
                    &s_poom_wifi_deauth_detector_hop_task) != pdPASS))
    {
        s_poom_wifi_deauth_detector_running = false;
        (void)poom_wifi_ctrl_set_promiscuous(false);
        (void)poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
        s_poom_wifi_deauth_detector_hop_task = NULL;
        poom_wifi_deauth_detector_free_trackers_();
        POOM_PRINTF_E("Failed to create hop task");
        return ESP_FAIL;
    }

    POOM_PRINTF_I("Deauth detector started in passive mode (%s, ch=%u)",
                  channel_hopping ? "hopping" : "fixed",
                  (unsigned)start_channel);
    return ESP_OK;
}

/**
 * @brief Stops passive detection and disables promiscuous capture callback.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_stop(void)
{
    esp_err_t status_promisc;
    esp_err_t status_cb;

    s_poom_wifi_deauth_detector_running = false;

    poom_wifi_deauth_detector_stop_hop_task_();

    status_cb = poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
    status_promisc = poom_wifi_ctrl_set_promiscuous(false);

    if((status_promisc != ESP_OK) && (status_promisc != ESP_ERR_WIFI_NOT_INIT))
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_promiscuous(false) failed: %s", esp_err_to_name(status_promisc));
    }

    if((status_cb != ESP_OK) && (status_cb != ESP_ERR_WIFI_NOT_INIT))
    {
        POOM_PRINTF_W("poom_wifi_ctrl_set_promiscuous_rx_cb(NULL) failed: %s", esp_err_to_name(status_cb));
    }

    poom_wifi_deauth_detector_free_trackers_();

    if((status_promisc != ESP_OK) && (status_promisc != ESP_ERR_WIFI_NOT_INIT))
    {
        return status_promisc;
    }
    if((status_cb != ESP_OK) && (status_cb != ESP_ERR_WIFI_NOT_INIT))
    {
        return status_cb;
    }

    return ESP_OK;
}

/**
 * @brief Returns whether detector runtime is currently active.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_deauth_detector_is_running(void)
{
    return s_poom_wifi_deauth_detector_running;
}

/**
 * @brief Reads detector runtime counters and mode/channel status.
 * @param[out] out_stats Output stats structure.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_get_stats(poom_wifi_deauth_detector_stats_t *out_stats)
{
    if(out_stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    *out_stats = s_poom_wifi_deauth_detector_stats;
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    return ESP_OK;
}

esp_err_t poom_wifi_deauth_detector_get_report(poom_wifi_deauth_detector_report_t *out_report)
{
    TickType_t now_tick;
    uint8_t i;
    uint8_t top_idx = 0;
    uint32_t top_count = 0;
    bool found = false;
    uint16_t top_reason = 0;
    uint32_t top_reason_count = 0;

    if(out_report == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)memset(out_report, 0, sizeof(*out_report));

    now_tick = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    poom_wifi_deauth_detector_window_rollover_(now_tick);

    out_report->deauth_pps = s_poom_wifi_deauth_detector_window_deauth;
    out_report->disassoc_pps = s_poom_wifi_deauth_detector_window_disassoc;
    out_report->bad_len = s_poom_wifi_deauth_detector_bad_len;
    out_report->protected_frames = s_poom_wifi_deauth_detector_protected;
    out_report->weird_ds = s_poom_wifi_deauth_detector_weird_ds;

    for(i = 0; (s_poom_wifi_deauth_detector_trackers != NULL) && (i < POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT); i++)
    {
        const poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[i];
        if(t->in_use && (t->window_count > 0U))
        {
            if((!found) || (t->window_count > top_count))
            {
                found = true;
                top_count = t->window_count;
                top_idx = i;
            }
        }
    }

    if(found)
    {
        const poom_wifi_deauth_detector_tracker_t *t = &s_poom_wifi_deauth_detector_trackers[top_idx];
        size_t ri;
        for(ri = 0; ri < POOM_WIFI_DEAUTH_DETECTOR_TRACKER_REASON_SLOTS; ri++)
        {
            if(t->reasons[ri].count > top_reason_count)
            {
                top_reason_count = t->reasons[ri].count;
                top_reason = t->reasons[ri].code;
            }
        }

        (void)memcpy(out_report->top_src, t->src, 6U);
        (void)memcpy(out_report->top_dst, t->last_dst, 6U);
        (void)memcpy(out_report->top_bssid, t->bssid, 6U);
        out_report->top_channel = t->last_channel;
        out_report->top_pps = t->window_count;
        out_report->top_unique_victims = t->unique_victims;
        out_report->top_reason = top_reason;
        out_report->top_reason_count = top_reason_count;
        out_report->top_valid = true;
    }

    out_report->alert = poom_wifi_deauth_detector_classify_alert_(out_report->deauth_pps,
                                                                  out_report->disassoc_pps,
                                                                  s_poom_wifi_deauth_detector_window_bcast_deauth,
                                                                  out_report->top_pps,
                                                                  out_report->top_unique_victims,
                                                                  out_report->top_valid);

    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    return ESP_OK;
}

esp_err_t poom_wifi_deauth_detector_set_fixed_channel(uint8_t channel)
{
    esp_err_t err = ESP_OK;

    if((channel < POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN) || (channel > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    s_poom_wifi_deauth_detector_stats.channel_hopping = false;
    s_poom_wifi_deauth_detector_stats.current_channel = channel;
    s_poom_wifi_deauth_detector_saved_channel = (uint8_t)(channel - 1U);
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    if(s_poom_wifi_deauth_detector_running)
    {
        poom_wifi_deauth_detector_stop_hop_task_();
        err = poom_wifi_ctrl_set_channel(channel);
    }

    return err;
}

esp_err_t poom_wifi_deauth_detector_set_channel_hopping(bool enabled)
{
    esp_err_t err = ESP_OK;
    uint8_t current_channel;

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    s_poom_wifi_deauth_detector_stats.channel_hopping = enabled;
    current_channel = s_poom_wifi_deauth_detector_stats.current_channel;
    if((current_channel < POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN) || (current_channel > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX))
    {
        current_channel = POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN;
        s_poom_wifi_deauth_detector_stats.current_channel = current_channel;
    }
    s_poom_wifi_deauth_detector_saved_channel =
        enabled ? POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_HOP_SENTINEL : (uint8_t)(current_channel - 1U);
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    if(s_poom_wifi_deauth_detector_running)
    {
        if(enabled)
        {
            err = poom_wifi_deauth_detector_start_hop_task_();
        }
        else
        {
            poom_wifi_deauth_detector_stop_hop_task_();
        }
    }

    return err;
}

/**
 * @brief Clears detector counters while keeping current runtime state.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_reset_stats(void)
{
    uint8_t keep_channel;
    bool keep_hopping;
    TickType_t now_tick = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_poom_wifi_deauth_detector_lock);
    keep_channel = s_poom_wifi_deauth_detector_stats.current_channel;
    keep_hopping = s_poom_wifi_deauth_detector_stats.channel_hopping;
    (void)memset(&s_poom_wifi_deauth_detector_stats, 0, sizeof(s_poom_wifi_deauth_detector_stats));
    if(s_poom_wifi_deauth_detector_trackers != NULL)
    {
        (void)memset(s_poom_wifi_deauth_detector_trackers,
                     0,
                     sizeof(*s_poom_wifi_deauth_detector_trackers) * POOM_WIFI_DEAUTH_DETECTOR_TRACKER_COUNT);
    }
    s_poom_wifi_deauth_detector_bad_len = 0;
    s_poom_wifi_deauth_detector_protected = 0;
    s_poom_wifi_deauth_detector_weird_ds = 0;
    s_poom_wifi_deauth_detector_window_start = now_tick;
    s_poom_wifi_deauth_detector_window_deauth = 0;
    s_poom_wifi_deauth_detector_window_disassoc = 0;
    s_poom_wifi_deauth_detector_window_bcast_deauth = 0;
    s_poom_wifi_deauth_detector_last_activity_tick = 0;
    s_poom_wifi_deauth_detector_last_activity_channel = 0U;
    s_poom_wifi_deauth_detector_stats.current_channel = keep_channel;
    s_poom_wifi_deauth_detector_stats.channel_hopping = keep_hopping;
    taskEXIT_CRITICAL(&s_poom_wifi_deauth_detector_lock);

    return ESP_OK;
}

/**
 * @brief Prints current detector counters to console.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_deauth_detector_print_stats(void)
{
    poom_wifi_deauth_detector_stats_t stats;
    uint8_t channel;

    (void)poom_wifi_deauth_detector_get_stats(&stats);

    printf("\n==== Deauth Detector Stats ====\n");
    printf("Running: %s\n", s_poom_wifi_deauth_detector_running ? "YES" : "NO");
    printf("Mode:    %s\n", stats.channel_hopping ? "HOP" : "FIXED");
    printf("Channel: %u\n", (unsigned)stats.current_channel);
    printf("Deauth total:   %lu\n", (unsigned long)stats.deauth_total);
    printf("Disassoc total: %lu\n", (unsigned long)stats.disassoc_total);

    for(channel = POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN; channel <= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MAX; channel++)
    {
        const uint8_t index = (uint8_t)(channel - POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_MIN);
        if((stats.deauth_by_channel[index] != 0U) || (stats.disassoc_by_channel[index] != 0U))
        {
            printf("CH%02u -> deauth=%lu disassoc=%lu\n",
                   (unsigned)channel,
                   (unsigned long)stats.deauth_by_channel[index],
                   (unsigned long)stats.disassoc_by_channel[index]);
        }
    }

    printf("===============================\n");
    return ESP_OK;
}

/**
 * @brief Sets detector saved-channel value following draft logic.
 * @param[in] saved_channel `99` enables hopping, otherwise uses `saved_channel + 1`.
 * @return void
 */
void poom_wifi_deauth_detector_set_saved_channel(uint8_t saved_channel)
{
    s_poom_wifi_deauth_detector_saved_channel = saved_channel;
}
