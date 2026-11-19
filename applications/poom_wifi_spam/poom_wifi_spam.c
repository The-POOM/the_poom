// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_spam.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_marauder.h"
#include "esp_random.h"

#if POOM_WIFI_SPAM_ENABLE_LOG
    static const char *POOM_WIFI_SPAM_TAG = "poom_wifi_spam";

    #define POOM_WIFI_SPAM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WIFI_SPAM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_WIFI_SPAM_DEBUG_LOG_ENABLED
        #define POOM_WIFI_SPAM_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_SPAM_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_WIFI_SPAM_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_WIFI_SPAM_PRINTF_E(...) do { } while (0)
    #define POOM_WIFI_SPAM_PRINTF_W(...) do { } while (0)
    #define POOM_WIFI_SPAM_PRINTF_D(...) do { } while (0)
#endif

#define POOM_WIFI_SPAM_SSIDS_KEY "wifi_spam_ssids"
#define POOM_WIFI_SPAM_SSIDS_MAGIC (0x57535331UL)
#define POOM_WIFI_SPAM_SSIDS_VERSION (1U)

static const char *const s_poom_wifi_spam_default_ssids[] = {
    "Starbucks_Free_WiFi",
    "Coffee_Shop_Guest",
    "Airport_Public_HighSpeed",
    "Hotel_Lobby_FreeWiFi",
    "McDonalds_Free_Internet",
    "Free_Public_WiFi",
    "Guest_Access_Point",
    "Coffee_Break_Network",
    "Open_WiFi_Connect",
    "Free_City_WiFi_Portal",
    "Welcome_Guest_Access",
    "Digital_Nomad_Hotspot",
    "Premium_Coffee_Link",
    "Downtown_Guest_WiFi",
    "Fast_Internet_No_Pass",
    "Wireless_Internet_Guest"
};

static bool poom_wifi_spam_is_running_(void)
{
    poom_marauder_status_t status;
    poom_wifi_marauder_get_status(&status);
    return status.mode == POOM_MARAUDER_BEACON_LIST && status.running;
}

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    char ssids[POOM_WIFI_SPAM_SSIDS_MAX][POOM_WIFI_SPAM_SSID_MAX_LEN + 1U];
} poom_wifi_spam_ssids_blob_v1_t;

static bool s_poom_wifi_spam_ssids_loaded = false;
static poom_wifi_spam_ssid_list_t s_poom_wifi_spam_ssids;

/**
 * @brief Internal helper for `poom_wifi_spam_ssids_set_defaults`.
 *
 * @return void
 */
static void poom_wifi_spam_ssids_set_defaults_(void)
{
    (void)memset(&s_poom_wifi_spam_ssids, 0, sizeof(s_poom_wifi_spam_ssids));

    const size_t def_cnt =
        sizeof(s_poom_wifi_spam_default_ssids) / sizeof(s_poom_wifi_spam_default_ssids[0]);
    const size_t n = (def_cnt > (size_t)POOM_WIFI_SPAM_SSIDS_MAX) ? (size_t)POOM_WIFI_SPAM_SSIDS_MAX : def_cnt;

    s_poom_wifi_spam_ssids.count = (uint8_t)n;
    for(size_t i = 0; i < n; i++)
    {
        (void)snprintf(s_poom_wifi_spam_ssids.ssids[i],
                       sizeof(s_poom_wifi_spam_ssids.ssids[i]),
                       "%s",
                       (s_poom_wifi_spam_default_ssids[i] != NULL) ? s_poom_wifi_spam_default_ssids[i] : "");
    }
}

/**
 * @brief Saves internal data used by this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_ssids_save_(void)
{
    poom_wifi_spam_ssids_blob_v1_t blob;
    (void)memset(&blob, 0, sizeof(blob));
    blob.magic = POOM_WIFI_SPAM_SSIDS_MAGIC;
    blob.version = POOM_WIFI_SPAM_SSIDS_VERSION;
    blob.count = s_poom_wifi_spam_ssids.count;
    (void)memcpy(blob.ssids, s_poom_wifi_spam_ssids.ssids, sizeof(blob.ssids));

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }

    return poom_secrets_set_blob(POOM_WIFI_SPAM_SSIDS_KEY, &blob, sizeof(blob));
}

/**
 * @brief Loads internal data used by this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_spam_ssids_load_(void)
{
    poom_wifi_spam_ssids_blob_v1_t blob;
    size_t blob_len = sizeof(blob);

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return err;
    }

    (void)memset(&blob, 0, sizeof(blob));
    err = poom_secrets_get_blob(POOM_WIFI_SPAM_SSIDS_KEY, &blob, &blob_len);
    if(err == ESP_ERR_NVS_NOT_FOUND)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return ESP_OK;
    }
    if(err != ESP_OK)
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return err;
    }

    if((blob_len != sizeof(blob)) || (blob.magic != POOM_WIFI_SPAM_SSIDS_MAGIC) ||
       (blob.version != POOM_WIFI_SPAM_SSIDS_VERSION))
    {
        poom_wifi_spam_ssids_set_defaults_();
        s_poom_wifi_spam_ssids_loaded = true;
        return ESP_OK;
    }

    (void)memset(&s_poom_wifi_spam_ssids, 0, sizeof(s_poom_wifi_spam_ssids));
    s_poom_wifi_spam_ssids.count = blob.count;
    if(s_poom_wifi_spam_ssids.count > POOM_WIFI_SPAM_SSIDS_MAX)
    {
        s_poom_wifi_spam_ssids.count = POOM_WIFI_SPAM_SSIDS_MAX;
    }
    (void)memcpy(s_poom_wifi_spam_ssids.ssids, blob.ssids, sizeof(blob.ssids));

    for(uint8_t i = 0; i < s_poom_wifi_spam_ssids.count; i++)
    {
        s_poom_wifi_spam_ssids.ssids[i][POOM_WIFI_SPAM_SSID_MAX_LEN] = '\0';
    }

    s_poom_wifi_spam_ssids_loaded = true;
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_spam_ssids_ensure_loaded`.
 *
 * @return void
 */
static void poom_wifi_spam_ssids_ensure_loaded_(void)
{
    if(s_poom_wifi_spam_ssids_loaded)
    {
        return;
    }
    (void)poom_wifi_spam_ssids_load_();
}

/* Beacon sources share the checked native TX worker. There is no separate
 * AP/forced-delete task and no mutation of the persisted SSID list. */
esp_err_t poom_wifi_spam_start_mode(poom_wifi_spam_mode_t mode, uint8_t channel, uint32_t duration_ms)
{
    if(mode < POOM_WIFI_SPAM_SAVED || mode >= POOM_WIFI_SPAM_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    if(poom_wifi_spam_is_running_()) return ESP_ERR_INVALID_STATE;
    poom_wifi_spam_ssid_list_t *list = calloc(1, sizeof(*list));
    poom_marauder_config_t *config = calloc(1, sizeof(*config));
    if(!list || !config) { free(list); free(config); return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_OK;
    if(mode == POOM_WIFI_SPAM_SAVED) err = poom_wifi_spam_ssids_get(list);
    else if(!poom_wifi_spam_preset(mode, list)) err = ESP_ERR_INVALID_ARG;
    if(err == ESP_OK && !list->count) err = ESP_ERR_INVALID_STATE;
    if(err == ESP_OK) {
        config->mode = POOM_MARAUDER_BEACON_LIST;
        config->ap.primary = channel;
        esp_fill_random(config->ap.bssid, sizeof(config->ap.bssid));
        config->ap.bssid[0] = (config->ap.bssid[0] & 0xfcU) | 0x02U;
        config->ap.authmode = WIFI_AUTH_OPEN;
        config->beacon_count = list->count;
        memcpy(config->beacon_ssids, list->ssids, sizeof(config->beacon_ssids));
        config->random_beacon_cycles = mode == POOM_WIFI_SPAM_RANDOM;
        config->duration_ms = duration_ms;
        config->interval_ms = 10U;
        err = poom_wifi_marauder_start(config);
    }
    free(config);
    free(list);
    return err;
}

esp_err_t poom_wifi_spam_start(void)
{
    if(poom_wifi_spam_is_running_()) return ESP_OK;
    return poom_wifi_spam_start_mode(POOM_WIFI_SPAM_SAVED, 1U, 60000U);
}

esp_err_t poom_wifi_spam_stop(void)
{
    poom_marauder_status_t status;
    poom_wifi_marauder_get_status(&status);
    if(status.mode == POOM_MARAUDER_BEACON_LIST) return poom_wifi_marauder_stop();
    return ESP_OK;
}

esp_err_t poom_wifi_spam_get_running(bool *out_running)
{
    if(!out_running) return ESP_ERR_INVALID_ARG;
    *out_running = poom_wifi_spam_is_running_();
    return ESP_OK;
}

void poom_wifi_spam_get_status(poom_wifi_spam_status_t *out)
{
    if(!out) return;
    poom_marauder_status_t status;
    poom_wifi_marauder_get_status(&status);
    memset(out, 0, sizeof(*out));
    if(status.mode != POOM_MARAUDER_BEACON_LIST) return;
    out->running = status.running;
    out->sent = status.sent;
    out->errors = status.errors;
    out->last_error = status.last_error;
    out->elapsed_ms = status.elapsed_ms;
}

esp_err_t poom_wifi_spam_ssids_get(poom_wifi_spam_ssid_list_t* out_list)
{
    if(out_list == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    (void)memcpy(out_list, &s_poom_wifi_spam_ssids, sizeof(*out_list));
    return ESP_OK;
}

esp_err_t poom_wifi_spam_ssids_set(const poom_wifi_spam_ssid_list_t* list)
{
    if(list == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if(poom_wifi_spam_is_running_())
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_ensure_loaded_();

    if(list->count > POOM_WIFI_SPAM_SSIDS_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    poom_wifi_spam_ssid_list_t tmp;
    (void)memset(&tmp, 0, sizeof(tmp));
    tmp.count = list->count;

    for(uint8_t i = 0; i < tmp.count; i++)
    {
        const char* s = list->ssids[i];
        if((s == NULL) || (s[0] == '\0'))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if(strlen(s) > POOM_WIFI_SPAM_SSID_MAX_LEN)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        (void)snprintf(tmp.ssids[i], sizeof(tmp.ssids[i]), "%s", s);
    }

    s_poom_wifi_spam_ssids = tmp;
    s_poom_wifi_spam_ssids_loaded = true;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_remove(uint8_t index)
{
    if(poom_wifi_spam_is_running_())
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    if(index >= s_poom_wifi_spam_ssids.count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for(uint8_t i = index; (i + 1U) < s_poom_wifi_spam_ssids.count; i++)
    {
        (void)memcpy(s_poom_wifi_spam_ssids.ssids[i],
                     s_poom_wifi_spam_ssids.ssids[i + 1U],
                     sizeof(s_poom_wifi_spam_ssids.ssids[i]));
    }
    (void)memset(s_poom_wifi_spam_ssids.ssids[s_poom_wifi_spam_ssids.count - 1U], 0,
                 sizeof(s_poom_wifi_spam_ssids.ssids[0]));
    s_poom_wifi_spam_ssids.count--;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_add(const char* ssid)
{
    if(poom_wifi_spam_is_running_())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    poom_wifi_spam_ssids_ensure_loaded_();
    if(s_poom_wifi_spam_ssids.count >= POOM_WIFI_SPAM_SSIDS_MAX)
    {
        return ESP_ERR_NO_MEM;
    }
    if(strlen(ssid) > POOM_WIFI_SPAM_SSID_MAX_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)snprintf(s_poom_wifi_spam_ssids.ssids[s_poom_wifi_spam_ssids.count],
                   sizeof(s_poom_wifi_spam_ssids.ssids[0]),
                   "%s",
                   ssid);
    s_poom_wifi_spam_ssids.count++;
    return poom_wifi_spam_ssids_save_();
}

esp_err_t poom_wifi_spam_ssids_reset_defaults(void)
{
    if(poom_wifi_spam_is_running_())
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_wifi_spam_ssids_set_defaults_();
    s_poom_wifi_spam_ssids_loaded = true;

    esp_err_t err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }
    (void)poom_secrets_erase_key(POOM_WIFI_SPAM_SSIDS_KEY);
    return ESP_OK;
}
