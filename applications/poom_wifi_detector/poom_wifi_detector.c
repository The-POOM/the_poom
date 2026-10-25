// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM
// Signature data derived from Eye Spy (Apache-2.0) and flock-you (MIT).

#include "poom_wifi_detector.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "poom_wifi_ctrl.h"
#include "poom_wifi_scanner.h"

#define POOM_WIFI_DETECTOR_ACTIVE_MIN_MS (15U)
#define POOM_WIFI_DETECTOR_ACTIVE_MAX_MS (60U)
#define POOM_WIFI_DETECTOR_HOME_DWELL_MS (30U)
#define POOM_WIFI_DETECTOR_MONITOR_DWELL_MS (140U)
#define POOM_WIFI_DETECTOR_STALE_MS (30000U)
#define POOM_WIFI_DETECTOR_UNKNOWN_AUTH ((uint8_t)WIFI_AUTH_MAX)

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t s_monitor_channels[] = {
    1, 6, 11, 2, 3,
    1, 6, 11, 4, 5,
    1, 6, 11, 7, 8,
    1, 6, 11, 9, 10,
    1, 6, 11, 12, 13,
    36, 40, 44, 48,
    1, 6, 11,
    149, 153, 157, 161, 165,
};
#else
static const uint8_t s_monitor_channels[] = {
    1, 6, 11, 2, 3,
    1, 6, 11, 4, 5,
    1, 6, 11, 7, 8,
    1, 6, 11, 9, 10,
    1, 6, 11, 12, 13,
};
#endif

typedef struct
{
    uint8_t oui[3];
    poom_wifi_detector_class_t device_class;
    poom_wifi_detector_confidence_t confidence;
    const char *vendor;
} poom_wifi_detector_oui_t;

typedef struct
{
    const char *text;
    poom_wifi_detector_class_t device_class;
    const char *vendor;
} poom_wifi_detector_keyword_t;

typedef struct
{
    poom_wifi_detector_record_t **records;
    poom_wifi_detector_record_t *record_storage;
    size_t count;
    size_t capacity;
    poom_wifi_detector_filter_t filter;
    volatile bool running;
    uint8_t fixed_channel;
    uint8_t protected_mac[6];
    bool protected_mac_valid;
    uint8_t selected_ap[6];
    bool selected_ap_valid;
    TaskHandle_t task;
} poom_wifi_detector_monitor_t;

static poom_wifi_detector_monitor_t s_monitor;
static portMUX_TYPE s_monitor_lock = portMUX_INITIALIZER_UNLOCKED;

static const poom_wifi_detector_oui_t s_ouis[] = {
    {{0x70, 0xC9, 0x4E}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x3C, 0x91, 0x80}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xD8, 0xF3, 0xBC}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x80, 0x30, 0x49}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xB8, 0x35, 0x32}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x14, 0x5A, 0xFC}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x74, 0x4C, 0xA1}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x08, 0x3A, 0x88}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x9C, 0x2F, 0x9D}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xC0, 0x35, 0x32}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x94, 0x08, 0x53}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xE4, 0xAA, 0xEA}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x24, 0xB2, 0xB9}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xB8, 0x1E, 0xA4}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x70, 0x08, 0x94}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x58, 0x8E, 0x81}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xEC, 0x1B, 0xBD}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x3C, 0x71, 0xBF}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x58, 0x00, 0xE3}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x90, 0x35, 0xEA}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x5C, 0x93, 0xA2}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x64, 0x6E, 0x69}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x48, 0x27, 0xEA}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xA4, 0xCF, 0x12}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xE0, 0x4F, 0x43}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x82, 0x6B, 0xF2}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0xB4, 0x1E, 0x52}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x04, 0x0D, 0x84}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0xF0, 0x82, 0xC0}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0x1C, 0x34, 0xF1}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0x38, 0x5B, 0x44}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0x94, 0x34, 0x69}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0xB4, 0xE3, 0xF9}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Battery"},
    {{0xD4, 0xBB, 0xE6}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},
    {{0x3C, 0x61, 0x05}, POOM_WIFI_DETECTOR_CLASS_FLOCK, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Flock Safety"},

    {{0xF4, 0x6A, 0xDD}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Liteon"},
    {{0xF8, 0xA2, 0xD6}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Liteon"},
    {{0x00, 0xF4, 0x8D}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "USI"},
    {{0xD0, 0x39, 0x57}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "USI"},
    {{0xE8, 0xD0, 0xFC}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "USI"},
    {{0xE0, 0x0A, 0xF6}, POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "USI"},
    {{0xD4, 0x11, 0xD6}, POOM_WIFI_DETECTOR_CLASS_SOUNDTHINKING, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "SoundThinking"},
    {{0x00, 0x0E, 0x58}, POOM_WIFI_DETECTOR_CLASS_ALPR, POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE, "Vigilant ALPR"},

    {{0x00, 0x40, 0x8C}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Axis"},
    {{0xAC, 0xCC, 0x8E}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Axis"},
    {{0xB8, 0xA4, 0x4F}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Axis"},
    {{0x4C, 0xBD, 0x8F}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hikvision"},
    {{0xBC, 0xAD, 0x28}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hikvision"},
    {{0x44, 0x19, 0xB6}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hikvision"},
    {{0xC4, 0x2F, 0x90}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hikvision"},
    {{0x28, 0x57, 0xBE}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hikvision"},
    {{0x90, 0x02, 0xA9}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Dahua"},
    {{0x3C, 0xEF, 0x8C}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Dahua"},
    {{0xE0, 0x50, 0x8B}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Dahua"},
    {{0x4C, 0x11, 0xBF}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Dahua"},
    {{0xEC, 0x71, 0xDB}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Reolink"},
    {{0x2C, 0xAA, 0x8E}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Wyze"},
    {{0xD0, 0x3F, 0x27}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Wyze"},
    {{0x3C, 0x37, 0x86}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Arlo"},
    {{0x14, 0xB4, 0x84}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Arlo"},
    {{0xF0, 0x27, 0x65}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Ring/Nest"},
    {{0x18, 0xB4, 0x30}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Ring/Nest"},
    {{0x64, 0x16, 0x66}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Nest"},
    {{0x9C, 0x8E, 0xCD}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Amcrest"},
    {{0x90, 0xC7, 0xD8}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Amcrest"},
    {{0x00, 0x2B, 0x67}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Vivotek"},
    {{0x34, 0x40, 0xB5}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hanwha"},
    {{0x00, 0x09, 0x6C}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Hanwha"},
    {{0x00, 0x40, 0x48}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "FLIR"},
    {{0xAC, 0x3A, 0x7A}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "FLIR"},
    {{0x00, 0x1B, 0xC5}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Mobotix"},
    {{0xA8, 0x9F, 0xBA}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Ubiquiti"},
    {{0xFC, 0xEC, 0xDA}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Ubiquiti"},
    {{0x24, 0xA4, 0x3C}, POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE, "Ubiquiti"},
};

static const poom_wifi_detector_keyword_t s_keywords[] = {
    {"flocksafety", POOM_WIFI_DETECTOR_CLASS_FLOCK, "Flock Safety"},
    {"flock", POOM_WIFI_DETECTOR_CLASS_FLOCK, "Flock Safety"},
    {"pigvision", POOM_WIFI_DETECTOR_CLASS_FLOCK, "Flock Safety"},
    {"penguin", POOM_WIFI_DETECTOR_CLASS_FLOCK, "Flock Safety"},
    {"fs ext", POOM_WIFI_DETECTOR_CLASS_FLOCK, "Flock Battery"},
    {"licenseplat", POOM_WIFI_DETECTOR_CLASS_ALPR, "ALPR"},
    {"plateread", POOM_WIFI_DETECTOR_CLASS_ALPR, "ALPR"},
    {"vigilant", POOM_WIFI_DETECTOR_CLASS_ALPR, "Vigilant ALPR"},
    {"automate", POOM_WIFI_DETECTOR_CLASS_ALPR, "ALPR"},
    {"alpr", POOM_WIFI_DETECTOR_CLASS_ALPR, "ALPR"},
    {"hikvision", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Hikvision"},
    {"doorbell", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Doorbell camera"},
    {"amcrest", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Amcrest"},
    {"reolink", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Reolink"},
    {"vivotek", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Vivotek"},
    {"mobotix", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Mobotix"},
    {"genetec", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Genetec"},
    {"hanwha", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Hanwha"},
    {"dahua", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Dahua"},
    {"lorex", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Lorex"},
    {"blink", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Blink"},
    {"ipcam", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "IP camera"},
    {"arlo", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Arlo"},
    {"wyze", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Wyze"},
    {"axis", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "Axis"},
    {"flir", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "FLIR"},
    {"cctv", POOM_WIFI_DETECTOR_CLASS_IP_CAMERA, "CCTV"},
};

static bool poom_wifi_detector_contains_ci_(const char *text, const char *needle)
{
    size_t i;

    if((text == NULL) || (needle == NULL) || (needle[0] == '\0'))
    {
        return false;
    }

    for(i = 0U; text[i] != '\0'; ++i)
    {
        size_t j = 0U;
        while((needle[j] != '\0') && (text[i + j] != '\0') &&
              (tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j])))
        {
            ++j;
        }
        if(needle[j] == '\0')
        {
            return true;
        }
    }

    return false;
}

static bool poom_wifi_detector_class_in_filter_(poom_wifi_detector_class_t device_class,
                                                 poom_wifi_detector_filter_t filter)
{
    if(filter == POOM_WIFI_DETECTOR_FILTER_DEVICES)
    {
        return true;
    }
    if(filter == POOM_WIFI_DETECTOR_FILTER_FLOCK_ALPR)
    {
        return (device_class == POOM_WIFI_DETECTOR_CLASS_FLOCK) ||
               (device_class == POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER) ||
               (device_class == POOM_WIFI_DETECTOR_CLASS_ALPR) ||
               (device_class == POOM_WIFI_DETECTOR_CLASS_SOUNDTHINKING);
    }
    if(filter == POOM_WIFI_DETECTOR_FILTER_IP_CAMERAS)
    {
        return device_class == POOM_WIFI_DETECTOR_CLASS_IP_CAMERA;
    }
    return false;
}

static bool poom_wifi_detector_is_duplicate_(
    const poom_wifi_detector_record_t *candidate,
    const wifi_ap_record_t *source_ap,
    poom_wifi_detector_record_t *const *records,
    size_t count,
    bool group_by_ssid)
{
    size_t i;

    if((candidate == NULL) || (source_ap == NULL) || (records == NULL))
    {
        return false;
    }

    for(i = 0U; i < count; ++i)
    {
        if(records[i] == NULL)
        {
            continue;
        }
        if(memcmp(records[i]->bssid, candidate->bssid, sizeof(candidate->bssid)) == 0)
        {
            return true;
        }
        if(group_by_ssid && (source_ap->ssid[0] != 0U) &&
           !records[i]->ssid_hidden &&
           (strncmp(records[i]->ssid, candidate->ssid, sizeof(candidate->ssid)) == 0))
        {
            return true;
        }
    }

    return false;
}

static void poom_wifi_detector_classify_(const wifi_ap_record_t *ap,
                                          poom_wifi_detector_record_t *record)
{
    const poom_wifi_detector_oui_t *oui_match = NULL;
    const poom_wifi_detector_keyword_t *keyword_match = NULL;
    size_t i;
    size_t ssid_len;

    (void)memset(record, 0, sizeof(*record));
    ssid_len = strnlen((const char *)ap->ssid, sizeof(ap->ssid));
    record->ssid_hidden = (ssid_len == 0U);
    if(ssid_len >= sizeof(record->ssid))
    {
        ssid_len = sizeof(record->ssid) - 1U;
    }
    if(ssid_len > 0U)
    {
        (void)memcpy(record->ssid, ap->ssid, ssid_len);
        record->ssid[ssid_len] = '\0';
    }
    else
    {
        (void)snprintf(record->ssid, sizeof(record->ssid), "HIDDEN");
    }
    (void)memcpy(record->bssid, ap->bssid, sizeof(record->bssid));
    record->rssi = ap->rssi;
    record->rssi_known = true;
    record->channel = ap->primary;
    record->auth_mode = (uint8_t)ap->authmode;
    record->is_access_point = true;
    record->auth_known = true;
    record->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    record->device_class = POOM_WIFI_DETECTOR_CLASS_UNKNOWN;
    record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_INFO;
    (void)snprintf(record->vendor, sizeof(record->vendor), "Unknown");

    for(i = 0U; i < (sizeof(s_ouis) / sizeof(s_ouis[0])); ++i)
    {
        if(memcmp(ap->bssid, s_ouis[i].oui, 3U) == 0)
        {
            oui_match = &s_ouis[i];
            break;
        }
    }

    for(i = 0U; i < (sizeof(s_keywords) / sizeof(s_keywords[0])); ++i)
    {
        if(poom_wifi_detector_contains_ci_(record->ssid, s_keywords[i].text))
        {
            keyword_match = &s_keywords[i];
            break;
        }
    }

    if(oui_match != NULL)
    {
        record->device_class = oui_match->device_class;
        record->confidence = oui_match->confidence;
        (void)snprintf(record->vendor, sizeof(record->vendor), "%s", oui_match->vendor);
    }

    if(keyword_match != NULL)
    {
        if(oui_match == NULL)
        {
            record->device_class = keyword_match->device_class;
            record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE;
            (void)snprintf(record->vendor, sizeof(record->vendor), "%s", keyword_match->vendor);
        }
        else if(oui_match->device_class == keyword_match->device_class)
        {
            record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_HIGH;
        }
    }
}

void poom_wifi_detector_classify_ap(const wifi_ap_record_t *ap,
                                    poom_wifi_detector_record_t *out_record)
{
    if((ap == NULL) || (out_record == NULL))
    {
        return;
    }
    poom_wifi_detector_classify_(ap, out_record);
}

static const poom_wifi_detector_oui_t *poom_wifi_detector_match_oui_(const uint8_t mac[6])
{
    size_t i;

    if(mac == NULL)
    {
        return NULL;
    }
    for(i = 0U; i < (sizeof(s_ouis) / sizeof(s_ouis[0])); ++i)
    {
        if(memcmp(mac, s_ouis[i].oui, sizeof(s_ouis[i].oui)) == 0)
        {
            return &s_ouis[i];
        }
    }
    return NULL;
}

static const poom_wifi_detector_keyword_t *poom_wifi_detector_match_keyword_(const char *ssid)
{
    size_t i;

    if((ssid == NULL) || (ssid[0] == '\0'))
    {
        return NULL;
    }
    for(i = 0U; i < (sizeof(s_keywords) / sizeof(s_keywords[0])); ++i)
    {
        if(poom_wifi_detector_contains_ci_(ssid, s_keywords[i].text))
        {
            return &s_keywords[i];
        }
    }
    return NULL;
}

static bool poom_wifi_detector_extract_ap_ssid_(const uint8_t *frame,
                                                size_t frame_len,
                                                uint8_t subtype,
                                                char out_ssid[POOM_WIFI_DETECTOR_SSID_LEN])
{
    size_t offset;

    if((frame == NULL) || (out_ssid == NULL) ||
       ((subtype != 5U) && (subtype != 8U)) || (frame_len < 38U))
    {
        return false;
    }

    out_ssid[0] = '\0';
    offset = 36U; /* 24-byte header plus beacon/probe-response fixed fields. */
    while((offset + 2U) <= frame_len)
    {
        uint8_t id = frame[offset];
        uint8_t ie_len = frame[offset + 1U];
        size_t next = offset + 2U + (size_t)ie_len;

        if(next > frame_len)
        {
            return false;
        }
        if(id == 0U)
        {
            size_t copy_len = ie_len;
            if(copy_len >= POOM_WIFI_DETECTOR_SSID_LEN)
            {
                copy_len = POOM_WIFI_DETECTOR_SSID_LEN - 1U;
            }
            if(copy_len > 0U)
            {
                (void)memcpy(out_ssid, &frame[offset + 2U], copy_len);
            }
            out_ssid[copy_len] = '\0';
            return true;
        }
        offset = next;
    }
    return false;
}

static bool poom_wifi_detector_is_wildcard_probe_(const uint8_t *frame,
                                                   size_t frame_len)
{
    size_t offset = 24U;

    if((frame == NULL) || (frame_len < 26U))
    {
        return false;
    }
    while((offset + 2U) <= frame_len)
    {
        uint8_t id = frame[offset];
        uint8_t ie_len = frame[offset + 1U];
        size_t next = offset + 2U + (size_t)ie_len;

        if(next > frame_len)
        {
            return false;
        }
        if(id == 0U)
        {
            return ie_len == 0U;
        }
        offset = next;
    }
    return false;
}

static bool poom_wifi_detector_ie_vendor_matches_(const uint8_t *data,
                                                   size_t data_len,
                                                   const uint8_t signature[7])
{
    return (data_len == 7U) && (memcmp(data, signature, 7U) == 0);
}

static bool poom_wifi_detector_matches_flock_ie_(const uint8_t *ies,
                                                  size_t ies_len)
{
    static const uint8_t first_vendor[7] = {
        0x50, 0x6F, 0x9A, 0x16, 0x03, 0x01, 0x03,
    };
    static const uint8_t second_vendor[7] = {
        0x00, 0x50, 0xF2, 0x08, 0x00, 0x00, 0x00,
    };
    size_t offset = 0U;
    uint8_t state = 0U;

    while((offset + 2U) <= ies_len)
    {
        uint8_t id = ies[offset];
        uint8_t ie_len = ies[offset + 1U];
        size_t next = offset + 2U + (size_t)ie_len;
        const uint8_t *data = &ies[offset + 2U];

        if(next > ies_len)
        {
            return false;
        }
        if(id == 0U)
        {
            offset = next;
            continue;
        }

        if((id == 221U) &&
           poom_wifi_detector_ie_vendor_matches_(data, ie_len, first_vendor))
        {
            state = 1U;
        }
        else if((state == 1U) && (id == 45U))
        {
            state = 2U;
        }
        else if((state == 2U) && (id == 191U))
        {
            state = 3U;
        }
        else if((state == 3U) && (id == 221U) &&
                poom_wifi_detector_ie_vendor_matches_(data, ie_len, second_vendor))
        {
            return true;
        }
        else
        {
            state = 0U;
        }
        offset = next;
    }
    return false;
}

static int poom_wifi_detector_monitor_find_locked_(const uint8_t mac[6])
{
    size_t i;

    for(i = 0U; i < s_monitor.count; ++i)
    {
        if(memcmp(s_monitor.records[i]->bssid, mac, 6U) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static void poom_wifi_detector_monitor_update_rssi_(poom_wifi_detector_record_t *record,
                                                     int8_t rssi)
{
    if(!record->rssi_known)
    {
        record->rssi = rssi;
        record->rssi_known = true;
    }
    else
    {
        record->rssi = (int8_t)(((int)record->rssi * 3 + (int)rssi) / 4);
    }
}

static bool poom_wifi_detector_mac_is_station_(const uint8_t mac[6],
                                               const uint8_t ap[6])
{
    static const uint8_t zero_mac[6] = {0};

    return (mac != NULL) && (ap != NULL) &&
           ((mac[0] & 0x01U) == 0U) &&
           (memcmp(mac, zero_mac, sizeof(zero_mac)) != 0) &&
           (memcmp(mac, ap, 6U) != 0);
}

static void poom_wifi_detector_monitor_clear_locked_(void)
{
    size_t i;

    for(i = 0U; i < s_monitor.capacity; ++i)
    {
        (void)memset(s_monitor.records[i], 0, sizeof(*s_monitor.records[i]));
    }
    s_monitor.count = 0U;
    (void)memset(s_monitor.protected_mac, 0, sizeof(s_monitor.protected_mac));
    s_monitor.protected_mac_valid = false;
}

static void poom_wifi_detector_monitor_handle_ap_clients_(
    const wifi_promiscuous_pkt_t *packet,
    wifi_promiscuous_pkt_type_t packet_type,
    const uint8_t *frame,
    size_t frame_len,
    uint8_t frame_type,
    uint8_t subtype)
{
    poom_wifi_detector_record_t *record;
    const uint8_t *mac;
    char ssid[POOM_WIFI_DETECTOR_SSID_LEN] = {0};
    bool ap_selected;
    bool update_rssi = true;
    int index;

    portENTER_CRITICAL(&s_monitor_lock);
    ap_selected = s_monitor.selected_ap_valid;
    portEXIT_CRITICAL(&s_monitor_lock);

    if(!ap_selected)
    {
        if((packet_type != WIFI_PKT_MGMT) || (frame_type != 0U) ||
           (subtype != 8U) ||
           !poom_wifi_detector_extract_ap_ssid_(frame, frame_len, subtype, ssid))
        {
            return;
        }
        mac = &frame[10]; /* Beacon transmitter/BSSID. */
        if(!poom_wifi_detector_mac_is_station_(mac, (const uint8_t[6]){0xFFU}))
        {
            return;
        }
    }
    else
    {
        bool to_ds;
        bool from_ds;

        if((packet_type != WIFI_PKT_DATA) || (frame_type != 2U))
        {
            return;
        }
        to_ds = (frame[1] & 0x01U) != 0U;
        from_ds = (frame[1] & 0x02U) != 0U;

        portENTER_CRITICAL(&s_monitor_lock);
        if(!s_monitor.running || !s_monitor.selected_ap_valid)
        {
            portEXIT_CRITICAL(&s_monitor_lock);
            return;
        }
        if(to_ds && !from_ds &&
           (memcmp(&frame[4], s_monitor.selected_ap, 6U) == 0))
        {
            mac = &frame[10]; /* addr2: station transmitting to AP. */
        }
        else if(!to_ds && from_ds &&
                (memcmp(&frame[10], s_monitor.selected_ap, 6U) == 0))
        {
            mac = &frame[4]; /* addr1: station receiving from AP. */
            update_rssi = false; /* Packet RSSI belongs to the AP transmitter. */
        }
        else
        {
            portEXIT_CRITICAL(&s_monitor_lock);
            return;
        }
        if(!poom_wifi_detector_mac_is_station_(mac, s_monitor.selected_ap))
        {
            portEXIT_CRITICAL(&s_monitor_lock);
            return;
        }

        index = poom_wifi_detector_monitor_find_locked_(mac);
        if(index < 0)
        {
            if(s_monitor.count >= s_monitor.capacity)
            {
                portEXIT_CRITICAL(&s_monitor_lock);
                return;
            }
            index = (int)s_monitor.count++;
            record = s_monitor.records[index];
            (void)memset(record, 0, sizeof(*record));
            (void)memcpy(record->bssid, mac, sizeof(record->bssid));
            (void)snprintf(record->ssid,
                           sizeof(record->ssid),
                           "%02X:%02X:%02X:%02X:%02X:%02X",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            (void)snprintf(record->vendor, sizeof(record->vendor), "WiFi client");
            record->auth_mode = POOM_WIFI_DETECTOR_UNKNOWN_AUTH;
            record->device_class = POOM_WIFI_DETECTOR_CLASS_UNKNOWN;
            record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_INFO;
        }
        else
        {
            record = s_monitor.records[index];
        }

        if(update_rssi)
        {
            poom_wifi_detector_monitor_update_rssi_(record,
                                                     (int8_t)packet->rx_ctrl.rssi);
        }
        record->channel = packet->rx_ctrl.channel;
        record->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        portEXIT_CRITICAL(&s_monitor_lock);
        return;
    }

    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running || s_monitor.selected_ap_valid ||
       (s_monitor.records == NULL))
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return;
    }
    index = poom_wifi_detector_monitor_find_locked_(mac);
    if(index < 0)
    {
        if(s_monitor.count >= s_monitor.capacity)
        {
            portEXIT_CRITICAL(&s_monitor_lock);
            return;
        }
        index = (int)s_monitor.count++;
        record = s_monitor.records[index];
        (void)memset(record, 0, sizeof(*record));
        (void)memcpy(record->bssid, mac, sizeof(record->bssid));
        record->auth_mode = POOM_WIFI_DETECTOR_UNKNOWN_AUTH;
        record->ssid_hidden = (ssid[0] == '\0');
        record->is_access_point = true;
        record->device_class = POOM_WIFI_DETECTOR_CLASS_UNKNOWN;
        record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_INFO;
        (void)snprintf(record->vendor, sizeof(record->vendor), "Access point");
        (void)snprintf(record->ssid,
                       sizeof(record->ssid),
                       "%s",
                       (ssid[0] != '\0') ? ssid : "HIDDEN");
    }
    else
    {
        record = s_monitor.records[index];
        if(ssid[0] != '\0')
        {
            record->ssid_hidden = false;
            (void)snprintf(record->ssid, sizeof(record->ssid), "%s", ssid);
        }
    }
    poom_wifi_detector_monitor_update_rssi_(record, (int8_t)packet->rx_ctrl.rssi);
    record->channel = packet->rx_ctrl.channel;
    record->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    portEXIT_CRITICAL(&s_monitor_lock);
}

static void poom_wifi_detector_monitor_observe_receiver_locked_(
    const uint8_t mac[6],
    const poom_wifi_detector_oui_t *oui_match,
    uint8_t channel,
    uint32_t now_ms)
{
    poom_wifi_detector_record_t *record;
    int index;

    if((mac == NULL) || (oui_match == NULL) || ((mac[0] & 0x01U) != 0U) ||
       !poom_wifi_detector_class_in_filter_(oui_match->device_class, s_monitor.filter))
    {
        return;
    }

    index = poom_wifi_detector_monitor_find_locked_(mac);
    if(index < 0)
    {
        if(s_monitor.count >= s_monitor.capacity)
        {
            return;
        }
        index = (int)s_monitor.count++;
        record = s_monitor.records[index];
        (void)memset(record, 0, sizeof(*record));
        (void)memcpy(record->bssid, mac, sizeof(record->bssid));
        record->auth_mode = POOM_WIFI_DETECTOR_UNKNOWN_AUTH;
        record->ssid_hidden = true;
        record->is_access_point = false;
        (void)snprintf(record->ssid, sizeof(record->ssid), "%s", oui_match->vendor);
    }
    else
    {
        record = s_monitor.records[index];
    }

    if((record->device_class == POOM_WIFI_DETECTOR_CLASS_UNKNOWN) ||
       (record->confidence <= oui_match->confidence))
    {
        record->device_class = oui_match->device_class;
        record->confidence = oui_match->confidence;
        (void)snprintf(record->vendor, sizeof(record->vendor), "%s", oui_match->vendor);
    }
    record->channel = channel;
    record->last_seen_ms = now_ms;
}

static void poom_wifi_detector_promisc_cb_(void *buffer,
                                           wifi_promiscuous_pkt_type_t packet_type)
{
    const wifi_promiscuous_pkt_t *packet;
    const uint8_t *frame;
    const uint8_t *receiver;
    const uint8_t *transmitter;
    const poom_wifi_detector_oui_t *receiver_oui;
    const poom_wifi_detector_oui_t *oui_match;
    const poom_wifi_detector_keyword_t *keyword_match = NULL;
    poom_wifi_detector_record_t *record;
    char ssid[POOM_WIFI_DETECTOR_SSID_LEN] = {0};
    size_t frame_len;
    uint8_t frame_type;
    uint8_t subtype;
    bool ap_announcement = false;
    bool wildcard_probe = false;
    bool flock_ie_signature = false;
    uint32_t now_ms;
    int index;

    if((buffer == NULL) || !s_monitor.running || (packet_type == WIFI_PKT_MISC))
    {
        return;
    }

    packet = (const wifi_promiscuous_pkt_t *)buffer;
    if(packet->rx_ctrl.rx_state != 0U)
    {
        return;
    }
    frame = packet->payload;
    frame_len = packet->rx_ctrl.sig_len;
    if(frame_len < 24U)
    {
        return;
    }

    receiver = &frame[4];     /* 802.11 addr1 is the immediate receiver. */
    transmitter = &frame[10]; /* 802.11 addr2 is the transmitter address. */
    if((transmitter[0] & 0x01U) != 0U)
    {
        return;
    }
    receiver_oui = poom_wifi_detector_match_oui_(receiver);
    oui_match = poom_wifi_detector_match_oui_(transmitter);

    frame_type = (uint8_t)((frame[0] >> 2U) & 0x03U);
    subtype = (uint8_t)((frame[0] >> 4U) & 0x0FU);
    if(s_monitor.filter == POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS)
    {
        poom_wifi_detector_monitor_handle_ap_clients_(packet,
                                                       packet_type,
                                                       frame,
                                                       frame_len,
                                                       frame_type,
                                                       subtype);
        return;
    }
    if(frame_type == 0U)
    {
        ap_announcement = poom_wifi_detector_extract_ap_ssid_(frame,
                                                               frame_len,
                                                               subtype,
                                                               ssid);
        if(ap_announcement && (ssid[0] != '\0'))
        {
            keyword_match = poom_wifi_detector_match_keyword_(ssid);
        }
        if(subtype == 4U)
        {
            wildcard_probe = poom_wifi_detector_is_wildcard_probe_(frame, frame_len);
            if((oui_match != NULL) &&
               (oui_match->device_class == POOM_WIFI_DETECTOR_CLASS_FLOCK))
            {
                flock_ie_signature = poom_wifi_detector_matches_flock_ie_(
                    &frame[24], frame_len - 24U);
            }
        }
    }

    now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running || (s_monitor.records == NULL))
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return;
    }

    if(memcmp(receiver, transmitter, 6U) != 0)
    {
        poom_wifi_detector_monitor_observe_receiver_locked_(receiver,
                                                            receiver_oui,
                                                            packet->rx_ctrl.channel,
                                                            now_ms);
    }

    index = poom_wifi_detector_monitor_find_locked_(transmitter);
    if(index < 0)
    {
        bool oui_allowed = (oui_match != NULL) &&
                           poom_wifi_detector_class_in_filter_(oui_match->device_class,
                                                               s_monitor.filter);
        bool keyword_allowed = (keyword_match != NULL) &&
                               poom_wifi_detector_class_in_filter_(keyword_match->device_class,
                                                                   s_monitor.filter);

        if((!oui_allowed && !keyword_allowed) || (s_monitor.count >= s_monitor.capacity))
        {
            portEXIT_CRITICAL(&s_monitor_lock);
            return;
        }
        index = (int)s_monitor.count++;
        record = s_monitor.records[index];
        (void)memset(record, 0, sizeof(*record));
        (void)memcpy(record->bssid, transmitter, sizeof(record->bssid));
        record->auth_mode = POOM_WIFI_DETECTOR_UNKNOWN_AUTH;
        record->auth_known = false;
        record->is_access_point = ap_announcement;
        record->ssid_hidden = (ssid[0] == '\0');

        if(oui_allowed)
        {
            record->device_class = oui_match->device_class;
            record->confidence = oui_match->confidence;
            (void)snprintf(record->vendor, sizeof(record->vendor), "%s", oui_match->vendor);
        }
        else
        {
            record->device_class = keyword_match->device_class;
            record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE;
            (void)snprintf(record->vendor, sizeof(record->vendor), "%s", keyword_match->vendor);
        }

        (void)snprintf(record->ssid,
                       sizeof(record->ssid),
                       "%s",
                       (ssid[0] != '\0') ? ssid : record->vendor);
    }
    else
    {
        record = s_monitor.records[index];
    }

    poom_wifi_detector_monitor_update_rssi_(record, (int8_t)packet->rx_ctrl.rssi);
    record->channel = packet->rx_ctrl.channel;
    record->last_seen_ms = now_ms;

    if(ap_announcement)
    {
        record->is_access_point = true;
        if(ssid[0] != '\0')
        {
            record->ssid_hidden = false;
            (void)snprintf(record->ssid, sizeof(record->ssid), "%s", ssid);
        }
    }
    if(oui_match != NULL)
    {
        (void)snprintf(record->vendor, sizeof(record->vendor), "%s", oui_match->vendor);
        if(record->confidence < oui_match->confidence)
        {
            record->confidence = oui_match->confidence;
        }
    }
    if((oui_match != NULL) && (keyword_match != NULL) &&
       (oui_match->device_class == keyword_match->device_class))
    {
        record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_HIGH;
    }
    else if(flock_ie_signature)
    {
        record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_HIGH;
    }
    else if(wildcard_probe && (oui_match != NULL) &&
            (oui_match->device_class == POOM_WIFI_DETECTOR_CLASS_FLOCK) &&
            (record->confidence < POOM_WIFI_DETECTOR_CONFIDENCE_HIGH))
    {
        record->confidence = POOM_WIFI_DETECTOR_CONFIDENCE_HIGH;
    }
    portEXIT_CRITICAL(&s_monitor_lock);
}

static void poom_wifi_detector_monitor_task_(void *arg)
{
    size_t channel_index = 0U;
    uint8_t last_channel = 0U;
    (void)arg;

    while(true)
    {
        bool running;
        uint8_t fixed_channel;
        uint8_t channel;

        portENTER_CRITICAL(&s_monitor_lock);
        running = s_monitor.running;
        fixed_channel = s_monitor.fixed_channel;
        portEXIT_CRITICAL(&s_monitor_lock);
        if(!running)
        {
            break;
        }

        if(fixed_channel != 0U)
        {
            channel = fixed_channel;
        }
        else
        {
            channel = s_monitor_channels[channel_index];
            channel_index = (channel_index + 1U) %
                            (sizeof(s_monitor_channels) / sizeof(s_monitor_channels[0]));
        }
        if(channel != last_channel)
        {
            if(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK)
            {
                last_channel = channel;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(POOM_WIFI_DETECTOR_MONITOR_DWELL_MS));
    }

    portENTER_CRITICAL(&s_monitor_lock);
    s_monitor.task = NULL;
    portEXIT_CRITICAL(&s_monitor_lock);
    vTaskDelete(NULL);
}

static void poom_wifi_detector_monitor_free_records_(
    poom_wifi_detector_record_t **records,
    poom_wifi_detector_record_t *record_storage)
{
    free(record_storage);
    free(records);
}

esp_err_t poom_wifi_detector_monitor_start(
    poom_wifi_detector_filter_t filter,
    poom_wifi_detector_record_t *const *seed_records,
    size_t seed_count,
    size_t capacity)
{
    poom_wifi_detector_record_t **records;
    poom_wifi_detector_record_t *record_storage;
    wifi_promiscuous_filter_t promisc_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_err_t status;
    size_t i;

    if((filter != POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS) &&
       (filter != POOM_WIFI_DETECTOR_FILTER_FLOCK_ALPR) &&
       (filter != POOM_WIFI_DETECTOR_FILTER_IP_CAMERAS))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if((capacity == 0U) || (seed_count > capacity) ||
       ((seed_count > 0U) && (seed_records == NULL)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)poom_wifi_detector_monitor_stop();
    records = heap_caps_malloc(capacity * sizeof(*records),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    record_storage = heap_caps_malloc(capacity * sizeof(*record_storage),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if((records == NULL) || (record_storage == NULL))
    {
        free(record_storage);
        free(records);
        return ESP_ERR_NO_MEM;
    }
    (void)memset(records, 0, capacity * sizeof(*records));
    (void)memset(record_storage, 0, capacity * sizeof(*record_storage));
    for(i = 0U; i < capacity; ++i)
    {
        records[i] = &record_storage[i];
        if(i < seed_count)
        {
            if(seed_records[i] == NULL)
            {
                poom_wifi_detector_monitor_free_records_(records, record_storage);
                return ESP_ERR_INVALID_ARG;
            }
            *records[i] = *seed_records[i];
        }
    }

    status = poom_wifi_ctrl_init_null();
    if(status != ESP_OK)
    {
        poom_wifi_detector_monitor_free_records_(records, record_storage);
        (void)poom_wifi_ctrl_deinit();
        return status;
    }
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    (void)esp_wifi_set_promiscuous(false);
    status = esp_wifi_set_promiscuous_filter(&promisc_filter);
    if(status != ESP_OK)
    {
        poom_wifi_detector_monitor_free_records_(records, record_storage);
        (void)poom_wifi_ctrl_deinit();
        return status;
    }
    status = esp_wifi_set_promiscuous_rx_cb(poom_wifi_detector_promisc_cb_);
    if(status != ESP_OK)
    {
        poom_wifi_detector_monitor_free_records_(records, record_storage);
        (void)poom_wifi_ctrl_deinit();
        return status;
    }

    portENTER_CRITICAL(&s_monitor_lock);
    s_monitor.records = records;
    s_monitor.record_storage = record_storage;
    s_monitor.count = seed_count;
    s_monitor.capacity = capacity;
    s_monitor.filter = filter;
    s_monitor.fixed_channel = 0U;
    (void)memset(s_monitor.selected_ap, 0, sizeof(s_monitor.selected_ap));
    s_monitor.selected_ap_valid = false;
    s_monitor.running = true;
    portEXIT_CRITICAL(&s_monitor_lock);

    if(xTaskCreate(poom_wifi_detector_monitor_task_,
                   "wifi_det_mon",
                   3072U,
                   NULL,
                   4U,
                   &s_monitor.task) != pdPASS)
    {
        portENTER_CRITICAL(&s_monitor_lock);
        s_monitor.running = false;
        s_monitor.records = NULL;
        s_monitor.record_storage = NULL;
        s_monitor.count = 0U;
        s_monitor.capacity = 0U;
        portEXIT_CRITICAL(&s_monitor_lock);
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        poom_wifi_detector_monitor_free_records_(records, record_storage);
        (void)poom_wifi_ctrl_deinit();
        return ESP_ERR_NO_MEM;
    }

    status = esp_wifi_set_promiscuous(true);
    if(status != ESP_OK)
    {
        (void)poom_wifi_detector_monitor_stop();
        (void)poom_wifi_ctrl_deinit();
        return status;
    }
    return ESP_OK;
}

static bool poom_wifi_detector_monitor_is_protected_locked_(
    const poom_wifi_detector_record_t *record)
{
    return s_monitor.protected_mac_valid && (record != NULL) &&
           (memcmp(record->bssid, s_monitor.protected_mac, 6U) == 0);
}

static void poom_wifi_detector_monitor_prune_locked_(uint32_t now_ms)
{
    size_t i = 0U;

    while(i < s_monitor.count)
    {
        poom_wifi_detector_record_t *record = s_monitor.records[i];
        bool stale = (record->last_seen_ms != 0U) &&
                     ((now_ms - record->last_seen_ms) > POOM_WIFI_DETECTOR_STALE_MS);

        if(stale && !poom_wifi_detector_monitor_is_protected_locked_(record))
        {
            size_t last = --s_monitor.count;
            if(i != last)
            {
                *s_monitor.records[i] = *s_monitor.records[last];
            }
            (void)memset(s_monitor.records[last], 0, sizeof(*s_monitor.records[last]));
            continue;
        }
        ++i;
    }
}

esp_err_t poom_wifi_detector_monitor_snapshot(
    poom_wifi_detector_record_t *const *out_records,
    size_t capacity,
    size_t *out_count)
{
    size_t count;
    size_t i;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    if((out_records == NULL) || (capacity == 0U) || (out_count == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for(i = 0U; i < capacity; ++i)
    {
        if(out_records[i] == NULL)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }

    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running || (s_monitor.records == NULL))
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    poom_wifi_detector_monitor_prune_locked_(now_ms);
    count = (s_monitor.count < capacity) ? s_monitor.count : capacity;
    for(i = 0U; i < count; ++i)
    {
        *out_records[i] = *s_monitor.records[i];
    }
    portEXIT_CRITICAL(&s_monitor_lock);

    *out_count = count;
    return ESP_OK;
}

esp_err_t poom_wifi_detector_monitor_set_channel(uint8_t channel,
                                                 const uint8_t target_mac[6])
{
    TaskHandle_t task;

    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running)
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_monitor.fixed_channel = channel;
    if((channel != 0U) && (target_mac != NULL))
    {
        (void)memcpy(s_monitor.protected_mac, target_mac, sizeof(s_monitor.protected_mac));
        s_monitor.protected_mac_valid = true;
    }
    else
    {
        (void)memset(s_monitor.protected_mac, 0, sizeof(s_monitor.protected_mac));
        s_monitor.protected_mac_valid = false;
    }
    task = s_monitor.task;
    portEXIT_CRITICAL(&s_monitor_lock);

    if(task != NULL)
    {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t poom_wifi_detector_monitor_select_ap(uint8_t channel,
                                               const uint8_t bssid[6])
{
    TaskHandle_t task;

    if((channel == 0U) || (bssid == NULL) || ((bssid[0] & 0x01U) != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running ||
       (s_monitor.filter != POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS))
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    poom_wifi_detector_monitor_clear_locked_();
    (void)memcpy(s_monitor.selected_ap, bssid, sizeof(s_monitor.selected_ap));
    s_monitor.selected_ap_valid = true;
    s_monitor.fixed_channel = channel;
    task = s_monitor.task;
    portEXIT_CRITICAL(&s_monitor_lock);

    if(task != NULL)
    {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t poom_wifi_detector_monitor_discover_aps(void)
{
    TaskHandle_t task;

    portENTER_CRITICAL(&s_monitor_lock);
    if(!s_monitor.running ||
       (s_monitor.filter != POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS))
    {
        portEXIT_CRITICAL(&s_monitor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    poom_wifi_detector_monitor_clear_locked_();
    (void)memset(s_monitor.selected_ap, 0, sizeof(s_monitor.selected_ap));
    s_monitor.selected_ap_valid = false;
    s_monitor.fixed_channel = 0U;
    task = s_monitor.task;
    portEXIT_CRITICAL(&s_monitor_lock);

    if(task != NULL)
    {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t poom_wifi_detector_monitor_stop(void)
{
    poom_wifi_detector_record_t **records;
    poom_wifi_detector_record_t *record_storage;
    TaskHandle_t task;

    portENTER_CRITICAL(&s_monitor_lock);
    s_monitor.running = false;
    task = s_monitor.task;
    portEXIT_CRITICAL(&s_monitor_lock);

    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);
    if(task != NULL)
    {
        xTaskNotifyGive(task);
        while(true)
        {
            portENTER_CRITICAL(&s_monitor_lock);
            task = s_monitor.task;
            portEXIT_CRITICAL(&s_monitor_lock);
            if(task == NULL)
            {
                break;
            }
            vTaskDelay(1U);
        }
    }

    portENTER_CRITICAL(&s_monitor_lock);
    records = s_monitor.records;
    record_storage = s_monitor.record_storage;
    (void)memset(&s_monitor, 0, sizeof(s_monitor));
    portEXIT_CRITICAL(&s_monitor_lock);
    poom_wifi_detector_monitor_free_records_(records, record_storage);
    return ESP_OK;
}

esp_err_t poom_wifi_detector_scan_channel(poom_wifi_detector_filter_t filter,
                                          uint8_t channel,
                                          poom_wifi_detector_record_t *const *out_records,
                                          size_t capacity,
                                          size_t *out_count)
{
    poom_wifi_scanner_ap_records_t *scan_records;
    size_t written = 0U;
    uint16_t i;
    esp_err_t status;
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = channel,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = POOM_WIFI_DETECTOR_ACTIVE_MIN_MS,
                .max = POOM_WIFI_DETECTOR_ACTIVE_MAX_MS,
            },
            .passive = 0U,
        },
        .home_chan_dwell_time = POOM_WIFI_DETECTOR_HOME_DWELL_MS,
        .channel_bitmap = {0},
        .coex_background_scan = false,
    };

    if((filter >= POOM_WIFI_DETECTOR_FILTER_COUNT) ||
       (out_records == NULL) || (capacity == 0U) || (out_count == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0U;
    status = poom_wifi_scanner_scan_configured(&scan_config);
    if(status != ESP_OK)
    {
        return status;
    }

    scan_records = poom_wifi_scanner_get_ap_records();
    if(scan_records == NULL)
    {
        return ESP_FAIL;
    }

    for(i = 0U; (i < scan_records->count) && (written < capacity); ++i)
    {
        poom_wifi_detector_record_t candidate;
        poom_wifi_detector_classify_(&scan_records->records[i], &candidate);
        if(poom_wifi_detector_class_in_filter_(candidate.device_class, filter) &&
           !poom_wifi_detector_is_duplicate_(&candidate,
                                              &scan_records->records[i],
                                              out_records,
                                              written,
                                              channel == 0U))
        {
            if(out_records[written] == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }
            *out_records[written++] = candidate;
        }
    }

    *out_count = written;
    return ESP_OK;
}

esp_err_t poom_wifi_detector_stop(void)
{
    (void)poom_wifi_detector_monitor_stop();
    esp_err_t clear_status = poom_wifi_scanner_clear_ap_records();
    esp_err_t stop_status = poom_wifi_ctrl_deinit();

    if((clear_status != ESP_OK) &&
       (clear_status != ESP_ERR_WIFI_NOT_INIT) &&
       (clear_status != ESP_ERR_WIFI_NOT_STARTED))
    {
        return clear_status;
    }
    return stop_status;
}

const char *poom_wifi_detector_filter_label(poom_wifi_detector_filter_t filter)
{
    switch(filter)
    {
        case POOM_WIFI_DETECTOR_FILTER_DEVICES: return "WIFI DEVICES";
        case POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS: return "AP CLIENTS";
        case POOM_WIFI_DETECTOR_FILTER_FLOCK_ALPR: return "FLOCK/ALPR";
        case POOM_WIFI_DETECTOR_FILTER_IP_CAMERAS: return "IP CAMERAS";
        default: return "WIFI DETECT";
    }
}

const char *poom_wifi_detector_auth_label(uint8_t auth_mode)
{
    switch((wifi_auth_mode_t)auth_mode)
    {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "SECURED";
    }
}
