// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_154_recon.c
 * @brief Active IEEE 802.15.4 / Thread network recon built on the integrated
 *        OpenThread stack (otLinkActiveScan / otThreadDiscover / otLinkEnergyScan).
 */

#include "poom_154_recon.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/thread.h"

/* POOM OpenThread wrapper: used only to guarantee the stack is initialized. */
#include "open_thread.h"

static const char* TAG = "154_recon";

/* Wait at most this long for a full-spectrum scan to finish. */
#define POOM_154_RECON_SCAN_TIMEOUT_MS (20000U)

/* IEEE 802.15.4 broadcast PAN ID (OT_PANID_BROADCAST). */
#define POOM_154_RECON_PANID_BROADCAST (0xFFFFU)

static bool                 s_initialized = false;
static SemaphoreHandle_t    s_scan_done   = NULL;

static poom_154_recon_net_t s_results[POOM_154_RECON_MAX_RESULTS];
static volatile uint8_t     s_result_count = 0U;

/**
 * @brief Internal helper: clamp a caller dwell to a sane default.
 *
 * @param[in] per_channel_ms Requested dwell (0 = use default).
 * @return Dwell in ms to pass to OpenThread.
 */
static uint16_t poom_154_recon_dwell_(uint16_t per_channel_ms)
{
    return (per_channel_ms == 0U) ? (uint16_t)POOM_154_RECON_DEFAULT_DWELL_MS : per_channel_ms;
}

/**
 * @brief Internal helper: OpenThread active-scan / discovery result callback.
 *
 * Invoked from the OpenThread task with the OT lock held, once per network and
 * finally once with aResult == NULL to signal completion. Must not re-acquire
 * the OT lock; it only copies into the static cache and releases the waiter.
 * Shared by otLinkActiveScan and otThreadDiscover (same callback type).
 *
 * @param[in] aResult  One network, or NULL on completion.
 * @param[in] aContext Unused.
 * @return void
 */
static void poom_154_recon_active_cb_(otActiveScanResult* aResult, void* aContext)
{
    (void)aContext;

    if (aResult == NULL)
    {
        /* Scan complete. */
        (void)xSemaphoreGive(s_scan_done);
        return;
    }

    if (s_result_count >= POOM_154_RECON_MAX_RESULTS)
    {
        return;
    }

    poom_154_recon_net_t* slot = &s_results[s_result_count];
    (void)memset(slot, 0, sizeof(*slot));

    (void)memcpy(slot->ext_address, aResult->mExtAddress.m8, sizeof(slot->ext_address));
    (void)memcpy(slot->ext_pan_id, aResult->mExtendedPanId.m8, sizeof(slot->ext_pan_id));

    /* otNetworkName.m8 is a NUL-terminated C string. */
    (void)strncpy(slot->network_name, aResult->mNetworkName.m8, sizeof(slot->network_name) - 1U);
    slot->network_name[sizeof(slot->network_name) - 1U] = '\0';

    slot->pan_id      = aResult->mPanId;
    slot->channel     = aResult->mChannel;
    slot->rssi        = aResult->mRssi;
    slot->lqi         = aResult->mLqi;
    slot->is_joinable = aResult->mIsJoinable;
    slot->is_discover = aResult->mDiscover;

    s_result_count++;
}

/**
 * @brief Internal helper: OpenThread energy-scan result callback.
 *
 * @param[in] aResult  One channel measurement, or NULL on completion.
 * @param[in] aContext Unused.
 * @return void
 */
static void poom_154_recon_energy_cb_(otEnergyScanResult* aResult, void* aContext)
{
    (void)aContext;

    if (aResult == NULL)
    {
        (void)xSemaphoreGive(s_scan_done);
        return;
    }

    printf("  ch %2u : %4d dBm\n", (unsigned)aResult->mChannel, (int)aResult->mMaxRssi);
}

esp_err_t poom_154_recon_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    if (s_scan_done == NULL)
    {
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_done == NULL)
        {
            ESP_LOGE(TAG, "failed to allocate scan semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Bring the OpenThread stack up (idempotent in practice; guarded here). */
    poom_ot_init();

    /*
     * Leave the node a non-participant so the radio is free for scanning and we
     * do not beacon as a router/end-device on some arbitrary channel.
     */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();
    if (instance != NULL)
    {
        (void)otThreadSetEnabled(instance, false);
    }
    esp_openthread_lock_release();

    if (instance == NULL)
    {
        ESP_LOGE(TAG, "OpenThread instance unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "802.15.4 recon ready (channels %u..%u)",
             (unsigned)POOM_154_RECON_CHANNEL_MIN, (unsigned)POOM_154_RECON_CHANNEL_MAX);
    return ESP_OK;
}

esp_err_t poom_154_recon_active_scan(uint32_t channel_mask,
                                     uint16_t per_channel_ms,
                                     uint8_t* out_count)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Reset the cache before the OT task starts filling it. */
    s_result_count = 0U;
    (void)xSemaphoreTake(s_scan_done, 0); /* drain any stale signal */

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();
    otError err = otLinkActiveScan(instance,
                                   channel_mask,
                                   poom_154_recon_dwell_(per_channel_ms),
                                   &poom_154_recon_active_cb_,
                                   NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "otLinkActiveScan failed: %d", (int)err);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(POOM_154_RECON_SCAN_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "active scan timed out");
        return ESP_ERR_TIMEOUT;
    }

    if (out_count != NULL)
    {
        *out_count = s_result_count;
    }
    return ESP_OK;
}

esp_err_t poom_154_recon_discover(uint32_t channel_mask,
                                  uint16_t per_channel_ms,
                                  uint8_t* out_count)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_result_count = 0U;
    (void)xSemaphoreTake(s_scan_done, 0);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();
    /* Broadcast PAN, not a joiner, no EUI64 filtering. Reuses the scan callback. */
    otError err = otThreadDiscover(instance,
                                   channel_mask,
                                   POOM_154_RECON_PANID_BROADCAST,
                                   false,
                                   false,
                                   &poom_154_recon_active_cb_,
                                   NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "otThreadDiscover failed: %d", (int)err);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(POOM_154_RECON_SCAN_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "discovery timed out");
        return ESP_ERR_TIMEOUT;
    }

    if (out_count != NULL)
    {
        *out_count = s_result_count;
    }
    return ESP_OK;
}

esp_err_t poom_154_recon_energy_scan(uint32_t channel_mask, uint16_t per_channel_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_scan_done, 0);

    printf("Energy scan (noise floor, %u ms/ch):\n", (unsigned)poom_154_recon_dwell_(per_channel_ms));

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();
    otError err = otLinkEnergyScan(instance,
                                   channel_mask,
                                   poom_154_recon_dwell_(per_channel_ms),
                                   &poom_154_recon_energy_cb_,
                                   NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "otLinkEnergyScan failed: %d", (int)err);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(POOM_154_RECON_SCAN_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "energy scan timed out");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool poom_154_recon_get_result(uint8_t index, poom_154_recon_net_t* out)
{
    if ((out == NULL) || (index >= s_result_count))
    {
        return false;
    }
    *out = s_results[index];
    return true;
}

uint8_t poom_154_recon_result_count(void)
{
    return s_result_count;
}
