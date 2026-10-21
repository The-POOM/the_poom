// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

// Shared saved-Wi-Fi-profile store (see poom_wifi_profile_store.h). The logic
// here was factored out of menu_wifi_profiles.c verbatim so Connection Test can
// reuse it; the NVS layout is unchanged.

#include "poom_wifi_profile_store.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "poom_secrets_store.h"

#define POOM_WIFI_PROFILE_COUNT_KEY "wifi_prof_n"

static void profile_record_id_(char *out, size_t out_len, uint16_t slot)
{
    (void)snprintf(out, out_len, "wifiprof/%u", (unsigned)slot);
}

/* Persist every live entry, drop the trailing leftover slot (post-delete), and
 * write the count last so a torn write can never report more than it stored. */
static esp_err_t profile_save_all_(const poom_wifi_profile_t *arr, uint16_t count)
{
    esp_err_t status;
    char id[24];

    (void)poom_secrets_init();

    for (uint16_t i = 0U; i < count; i++)
    {
        profile_record_id_(id, sizeof(id), i);
        status = poom_secrets_set_record_blob(id, &arr[i], sizeof(arr[i]));
        if (status != ESP_OK)
        {
            return status;
        }
    }

    profile_record_id_(id, sizeof(id), count);
    (void)poom_secrets_erase_record(id);

    return poom_secrets_set_u32(POOM_WIFI_PROFILE_COUNT_KEY, (uint32_t)count);
}

void poom_wifi_profile_store_load(poom_wifi_profile_t *arr, uint16_t cap,
                                  uint16_t *out_count)
{
    uint32_t count = 0U;
    uint16_t loaded = 0U;

    if ((arr == NULL) || (out_count == NULL) || (cap == 0U))
    {
        if (out_count != NULL)
        {
            *out_count = 0U;
        }
        return;
    }

    (void)poom_secrets_init();
    *out_count = 0U;

    if (poom_secrets_get_u32(POOM_WIFI_PROFILE_COUNT_KEY, &count) != ESP_OK)
    {
        return;
    }
    if (count > cap)
    {
        count = cap;
    }

    for (uint16_t i = 0U; i < (uint16_t)count; i++)
    {
        char id[24];
        poom_wifi_profile_t prof;
        size_t len = sizeof(prof);

        profile_record_id_(id, sizeof(id), i);
        (void)memset(&prof, 0, sizeof(prof));
        if (poom_secrets_get_record_blob(id, &prof, &len) == ESP_OK)
        {
            prof.ssid[POOM_WIFI_PROFILE_SSID_CAP - 1U] = '\0';
            prof.pass[POOM_WIFI_PROFILE_PASS_CAP - 1U] = '\0';
            arr[loaded] = prof;
            loaded++;
        }
    }

    *out_count = loaded;
}

esp_err_t poom_wifi_profile_store_add_or_update(poom_wifi_profile_t *arr,
                                                uint16_t *count, uint16_t cap,
                                                const char *ssid,
                                                const char *pass)
{
    int found = -1;

    if ((arr == NULL) || (count == NULL) || (ssid == NULL) || (cap == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t i = 0U; i < *count; i++)
    {
        if (strncmp(arr[i].ssid, ssid, POOM_WIFI_PROFILE_SSID_CAP) == 0)
        {
            found = (int)i;
            break;
        }
    }

    if (found < 0)
    {
        if (*count >= cap)
        {
            return ESP_ERR_NO_MEM;
        }
        found = (int)(*count);
        (*count)++;
    }

    (void)memset(&arr[found], 0, sizeof(arr[found]));
    (void)snprintf(arr[found].ssid, POOM_WIFI_PROFILE_SSID_CAP, "%.*s",
                   (int)(POOM_WIFI_PROFILE_SSID_CAP - 1U), ssid);
    (void)snprintf(arr[found].pass, POOM_WIFI_PROFILE_PASS_CAP, "%.*s",
                   (int)(POOM_WIFI_PROFILE_PASS_CAP - 1U),
                   (pass != NULL) ? pass : "");

    return profile_save_all_(arr, *count);
}

esp_err_t poom_wifi_profile_store_delete(poom_wifi_profile_t *arr,
                                         uint16_t *count, uint16_t slot)
{
    if ((arr == NULL) || (count == NULL) || (slot >= *count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t i = slot; (i + 1U) < *count; i++)
    {
        arr[i] = arr[i + 1U];
    }
    (*count)--;
    (void)memset(&arr[*count], 0, sizeof(arr[*count]));

    return profile_save_all_(arr, *count);
}
