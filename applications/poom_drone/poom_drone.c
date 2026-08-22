// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_drone.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "odid_wifi.h"

#include "poom_ble_scan.h"
#include "poom_pcap_manager.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"

#define POOM_DRONE_TAG "poom_drone"

#define POOM_DRONE_DEFAULT_WIFI_PHASE_MS (8000U)
#define POOM_DRONE_DEFAULT_BLE_PHASE_MS (8000U)
#define POOM_DRONE_DEFAULT_HOP_MS (300U)

#define POOM_DRONE_PARSE_QUEUE_LENGTH (16U)
#define POOM_DRONE_FRAME_MAX_LENGTH (512U)
#define POOM_DRONE_MIN_HEADER_LENGTH (16U)
#define POOM_DRONE_NAN_DEST_OFFSET (4U)
#define POOM_DRONE_NAN_DEST_SIZE (6U)
#define POOM_DRONE_BEACON_OFFSET (36U)
#define POOM_DRONE_BEACON_PACKET_OFFSET (7U)
#define POOM_DRONE_BEACON_ELEMENT_HDR_SIZE (2U)
#define POOM_DRONE_VENDOR_OUI_SIZE (3U)

#define POOM_DRONE_MAX_CHANNELS (64U)
#define POOM_DRONE_MAX_DEVICES (8U)

#define POOM_DRONE_BLE_AD_TYPE_SERVICE_DATA_16 (0x16U)
#define POOM_DRONE_BLE_SERVICE_UUID_ODID (0xFFFAU)
#define POOM_DRONE_BLE_AD_CODE_ODID (0x0DU)

#if POOM_DRONE_ENABLE_LOG
    static const char *POOM_DRONE_TAG = "poom_drone";

    #define POOM_DRONE_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_DRONE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_DRONE_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_DRONE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_DRONE_DEBUG_LOG_ENABLED
        #define POOM_DRONE_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_DRONE_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_DRONE_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_DRONE_PRINTF_E(...) do { } while (0)
    #define POOM_DRONE_PRINTF_I(...) do { } while (0)
    #define POOM_DRONE_PRINTF_D(...) do { } while (0)
#endif

typedef enum
{
    POOM_DRONE_SRC_WIFI = 0,
    POOM_DRONE_SRC_BLE,
} poom_drone_src_t;

typedef struct
{
    poom_drone_src_t src;
    uint16_t length;
    int8_t rssi;
    uint16_t channel;
    uint8_t mac[6];
    uint8_t payload[POOM_DRONE_FRAME_MAX_LENGTH];
} poom_drone_item_t;

typedef struct
{
    bool used;
    uint8_t mac[6];
    uint32_t last_seen_ms;
    ODID_UAS_Data uas;
} poom_drone_device_t;

static poom_drone_config_t s_cfg;
static poom_drone_report_cb_t s_report_cb = NULL;
static void *s_report_cb_ctx = NULL;

static TaskHandle_t s_ctrl_task_handle = NULL;
static TaskHandle_t s_parser_task_handle = NULL;
static QueueHandle_t s_parse_queue = NULL;
static volatile bool s_scanner_running = false;

static bool s_wifi_active = false;
static bool s_ble_active = false;
static bool s_pcap_active = false;

static uint8_t s_allowed_channels[POOM_DRONE_MAX_CHANNELS];
static uint8_t s_allowed_channel_count = 0U;
static uint8_t s_channel_index = 0U;

static poom_drone_device_t *s_devices = NULL;

static const uint8_t s_nan_destination[POOM_DRONE_NAN_DEST_SIZE] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
static const uint8_t s_astm_1_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0x50, 0x6f, 0x9a};
static const uint8_t s_astm_2_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0x90, 0x3a, 0xe6};
static const uint8_t s_astm_3_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0xfa, 0x0b, 0xbc};
static const uint8_t s_dji_1_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0x60, 0x60, 0x1f};
static const uint8_t s_dji_2_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0x48, 0x1c, 0xb9};
static const uint8_t s_dji_3_oui[POOM_DRONE_VENDOR_OUI_SIZE] = {0x34, 0xd2, 0x62};

/**
 * @brief Returns monotonic time in milliseconds (wraps at ~49 days).
 */
static uint32_t poom_drone_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * @brief Checks whether a vendor OUI belongs to supported DroneID formats.
 *
 * @param[in/out] vendor_data Vendor data pointer.
 * @param[in/out] vendor_data_len Vendor data length in bytes.
 * @return bool
 */
static bool poom_drone_is_supported_vendor_oui_(const uint8_t* vendor_data, size_t vendor_data_len) {
    if((vendor_data == NULL) || (vendor_data_len < POOM_DRONE_VENDOR_OUI_SIZE)) {
        return false;
    }

    if(memcmp(vendor_data, s_astm_1_oui, sizeof(s_astm_1_oui)) == 0) {
        return true;
    }
    if(memcmp(vendor_data, s_astm_2_oui, sizeof(s_astm_2_oui)) == 0) {
        return true;
    }
    if(memcmp(vendor_data, s_astm_3_oui, sizeof(s_astm_3_oui)) == 0) {
        return true;
    }
    if(memcmp(vendor_data, s_dji_1_oui, sizeof(s_dji_1_oui)) == 0) {
        return true;
    }
    if(memcmp(vendor_data, s_dji_2_oui, sizeof(s_dji_2_oui)) == 0) {
        return true;
    }
    if(memcmp(vendor_data, s_dji_3_oui, sizeof(s_dji_3_oui)) == 0) {
        return true;
    }

    return false;
}

/**
 * @brief Internal helper for `poom_drone_build_channel_list`.
 *
 * @return void
 */
static void poom_drone_build_channel_list_(void)
{
    wifi_country_t country = {0};
    esp_err_t ret = esp_wifi_get_country(&country);

    s_allowed_channel_count = 0U;
    s_channel_index = 0U;

    if (ret != ESP_OK)
    {
        s_allowed_channels[s_allowed_channel_count++] = 1;
        s_allowed_channels[s_allowed_channel_count++] = 6;
        s_allowed_channels[s_allowed_channel_count++] = 11;

#ifdef CONFIG_IDF_TARGET_ESP32C5
        s_allowed_channels[s_allowed_channel_count++] = 36;
        s_allowed_channels[s_allowed_channel_count++] = 40;
        s_allowed_channels[s_allowed_channel_count++] = 44;
        s_allowed_channels[s_allowed_channel_count++] = 48;
#endif

        return;
    }

    uint8_t start_24 = country.schan;
    uint8_t max_24 = country.nchan;
    if ((start_24 == 0U) || (max_24 == 0U))
    {
        start_24 = 1U;
        max_24 = 13U;
    }

    uint8_t end_24 = (uint8_t)(start_24 + max_24 - 1U);
    if (end_24 > 14U)
    {
        end_24 = 14U;
    }

    for (uint8_t ch = start_24; ch <= end_24; ch++)
    {
        if ((ch == 1U) || (ch == 6U) || (ch == 11U))
        {
            if (s_allowed_channel_count < POOM_DRONE_MAX_CHANNELS)
            {
                s_allowed_channels[s_allowed_channel_count++] = ch;
            }
        }
    }

    for (uint8_t ch = start_24; ch <= end_24; ch++)
    {
        if ((ch == 1U) || (ch == 6U) || (ch == 11U))
        {
            continue;
        }
        if (s_allowed_channel_count < POOM_DRONE_MAX_CHANNELS)
        {
            s_allowed_channels[s_allowed_channel_count++] = ch;
        }
    }

#ifdef CONFIG_IDF_TARGET_ESP32C5
    const char *cc = (const char *)country.cc;
    if ((cc != NULL) && (cc[0] != '\0'))
    {
        static const uint8_t us_5ghz[] = {
            36, 40, 44, 48, 52, 56, 60, 64,
            100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
            149, 153, 157, 161, 165,
        };
        static const uint8_t jp_5ghz[] = {
            36, 40, 44, 48, 52, 56, 60, 64,
            100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
        };
        static const uint8_t cn_5ghz[] = {
            36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165,
        };
        static const uint8_t eu_5ghz[] = {
            36, 40, 44, 48, 52, 56, 60, 64,
            100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
        };
        static const uint8_t default_5ghz[] = {36, 40, 44, 48};

        const uint8_t *list = default_5ghz;
        size_t count = sizeof(default_5ghz) / sizeof(default_5ghz[0]);

        if ((strcmp(cc, "US") == 0) || (strcmp(cc, "CA") == 0))
        {
            list = us_5ghz;
            count = sizeof(us_5ghz) / sizeof(us_5ghz[0]);
        }
        else if (strcmp(cc, "JP") == 0)
        {
            list = jp_5ghz;
            count = sizeof(jp_5ghz) / sizeof(jp_5ghz[0]);
        }
        else if (strcmp(cc, "CN") == 0)
        {
            list = cn_5ghz;
            count = sizeof(cn_5ghz) / sizeof(cn_5ghz[0]);
        }
        else if ((strcmp(cc, "EU") == 0) || (strcmp(cc, "GB") == 0) ||
                 (strcmp(cc, "DE") == 0) || (strcmp(cc, "FR") == 0))
        {
            list = eu_5ghz;
            count = sizeof(eu_5ghz) / sizeof(eu_5ghz[0]);
        }

        for (size_t i = 0; i < count; i++)
        {
            if (s_allowed_channel_count >= POOM_DRONE_MAX_CHANNELS)
            {
                break;
            }
            s_allowed_channels[s_allowed_channel_count++] = list[i];
        }
    }
#endif
}

/**
 * @brief Internal helper for `poom_drone_get_device`.
 *
 * @param[in] mac Parameter passed to the function.
 * @return poom_drone_device_t *
 */
static poom_drone_device_t *poom_drone_get_device_(const uint8_t mac[6])
{
    poom_drone_device_t *oldest = NULL;
    uint32_t oldest_ts = UINT32_MAX;

    if ((mac == NULL) || (s_devices == NULL))
    {
        return NULL;
    }

    for (size_t i = 0; i < POOM_DRONE_MAX_DEVICES; i++)
    {
        if (s_devices[i].used && (memcmp(s_devices[i].mac, mac, 6) == 0))
        {
            return &s_devices[i];
        }
    }

    for (size_t i = 0; i < POOM_DRONE_MAX_DEVICES; i++)
    {
        if (!s_devices[i].used)
        {
            oldest = &s_devices[i];
            break;
        }
        if (s_devices[i].last_seen_ms < oldest_ts)
        {
            oldest_ts = s_devices[i].last_seen_ms;
            oldest = &s_devices[i];
        }
    }

    if (oldest == NULL)
    {
        return NULL;
    }

    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    memcpy(oldest->mac, mac, 6);
    oldest->last_seen_ms = poom_drone_now_ms_();
    odid_initUasData(&oldest->uas);
    return oldest;
}

/**
 * @brief Fills one UAV report from parsed OpenDroneID data.
 *
 * @param[in/out] uav Output UAV report.
 * @param[in/out] uas_data Parsed OpenDroneID source data.
 * @param[in/out] channel Wi-Fi channel where packet was received.
 * @param[in/out] rssi Received signal strength.
 * @return bool
 */
static bool poom_drone_fill_uav_report_(
    poom_drone_uav_data_t* uav,
    const ODID_UAS_Data* uas_data,
    uint8_t channel,
    int8_t rssi) {
    if((uav == NULL) || (uas_data == NULL)) {
        return false;
    }

    bool has_any = false;
    for(size_t i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++) {
        if(uas_data->BasicIDValid[i]) {
            has_any = true;
            break;
        }
    }
    if(uas_data->LocationValid || uas_data->SystemValid || uas_data->SelfIDValid || uas_data->OperatorIDValid ||
       uas_data->AuthValid[0]) {
        has_any = true;
    }
    if(!has_any) {
        return false;
    }

    memset(uav->op_id, 0, sizeof(uav->op_id));
    memset(uav->uav_id, 0, sizeof(uav->uav_id));
    memset(uav->description, 0, sizeof(uav->description));
    memset(uav->auth_data, 0, sizeof(uav->auth_data));

    if(uas_data->BasicIDValid[0]) {
        strncpy(uav->uav_id, (const char*)uas_data->BasicID[0].UASID, ODID_ID_SIZE);
    }

    if(uas_data->LocationValid) {
        uav->lat_d = uas_data->Location.Latitude;
        uav->long_d = uas_data->Location.Longitude;
        uav->altitude_msl = (int)uas_data->Location.AltitudeGeo;
        uav->height_agl = (int)uas_data->Location.Height;
        uav->speed = (int)uas_data->Location.SpeedHorizontal;
        uav->heading = (int)uas_data->Location.Direction;
        uav->speed_vertical = (int)uas_data->Location.SpeedVertical;
        uav->altitude_pressure = (int)uas_data->Location.AltitudeBaro;
        uav->height_type = uas_data->Location.HeightType;
        uav->horizontal_accuracy = uas_data->Location.HorizAccuracy;
        uav->vertical_accuracy = uas_data->Location.VertAccuracy;
        uav->baro_accuracy = uas_data->Location.BaroAccuracy;
        uav->speed_accuracy = uas_data->Location.SpeedAccuracy;
        uav->timestamp = (int)uas_data->Location.TimeStamp;
        uav->status = uas_data->Location.Status;
    }

    if(uas_data->SystemValid) {
        uav->base_lat_d = uas_data->System.OperatorLatitude;
        uav->base_long_d = uas_data->System.OperatorLongitude;
        uav->operator_location_type = uas_data->System.OperatorLocationType;
        uav->classification_type = uas_data->System.ClassificationType;
        uav->area_count = uas_data->System.AreaCount;
        uav->area_radius = uas_data->System.AreaRadius;
        uav->area_ceiling = uas_data->System.AreaCeiling;
        uav->area_floor = uas_data->System.AreaFloor;
        uav->operator_altitude_geo = uas_data->System.OperatorAltitudeGeo;
        uav->system_timestamp = uas_data->System.Timestamp;
    }

    if(uas_data->AuthValid[0]) {
        uav->auth_type = uas_data->Auth[0].AuthType;
        uav->auth_page = uas_data->Auth[0].DataPage;
        uav->auth_length = uas_data->Auth[0].Length;
        uav->auth_timestamp = uas_data->Auth[0].Timestamp;
        memcpy(uav->auth_data, uas_data->Auth[0].AuthData, sizeof(uav->auth_data) - 1U);
    }

    if(uas_data->SelfIDValid) {
        uav->desc_type = uas_data->SelfID.DescType;
        strncpy(uav->description, uas_data->SelfID.Desc, ODID_STR_SIZE);
    }

    if(uas_data->OperatorIDValid) {
        uav->operator_id_type = uas_data->OperatorID.OperatorIdType;
        strncpy(uav->op_id, (const char*)uas_data->OperatorID.OperatorId, ODID_ID_SIZE);
    }

    uav->rssi = rssi;
    uav->channel = channel;
    return true;
}

/**
 * @brief Internal helper for `poom_drone_emit_report`.
 *
 * @param[in] uav Parameter passed to the function.
 * @return void
 */
static void poom_drone_emit_report_(const poom_drone_uav_data_t *uav)
{
    if (uav == NULL)
    {
        return;
    }

    if (s_cfg.enable_cli_print)
    {
        printf("[DRONEID] %02X:%02X:%02X:%02X:%02X:%02X RSSI=%d CH=%u LAT=%.6f LON=%.6f ID=%s\n",
               uav->mac[0], uav->mac[1], uav->mac[2], uav->mac[3], uav->mac[4], uav->mac[5],
               (int)uav->rssi,
               (unsigned)uav->channel,
               uav->lat_d,
               uav->long_d,
               uav->uav_id);
    }

    if (s_report_cb != NULL)
    {
        s_report_cb(uav, s_report_cb_ctx);
    }
}

/**
 * @brief Internal helper for `poom_drone_decode_message_pack`.
 *
 * @param[in] uas Parameter passed to the function.
 * @param[in] pack Parameter passed to the function.
 * @param[in] pack_len Parameter passed to the function.
 * @return bool
 */
static bool poom_drone_decode_message_pack_(ODID_UAS_Data *uas, const uint8_t *pack, size_t pack_len)
{
    if ((uas == NULL) || (pack == NULL))
    {
        return false;
    }

    const size_t min_hdr = sizeof(ODID_MessagePack_encoded) - ((size_t)ODID_MESSAGE_SIZE * (size_t)ODID_PACK_MAX_MESSAGES);
    if (pack_len < min_hdr)
    {
        return false;
    }

    const ODID_MessagePack_encoded *enc = (const ODID_MessagePack_encoded *)pack;
    if (enc->MessageType != ODID_MESSAGETYPE_PACKED)
    {
        return false;
    }

    if ((enc->MsgPackSize == 0) || (enc->MsgPackSize > ODID_PACK_MAX_MESSAGES))
    {
        return false;
    }

    size_t size = sizeof(*enc) - (size_t)ODID_MESSAGE_SIZE * (size_t)(ODID_PACK_MAX_MESSAGES - enc->MsgPackSize);
    if ((size == 0U) || (size > pack_len))
    {
        return false;
    }

    if (decodeMessagePack(uas, (ODID_MessagePack_encoded *)enc) != ODID_SUCCESS)
    {
        return false;
    }

    return true;
}

/**
 * @brief Parses beacon vendor elements to extract OpenDroneID message packs.
 *
 * @param[in/out] frame Input frame metadata and payload.
 * @param[in/out] dev Destination device record (UAS data accumulator).
 * @return void
 */
static bool poom_drone_process_beacon_frame_(
    const poom_drone_item_t *frame,
    poom_drone_device_t *dev)
{
    const uint8_t* payload;
    size_t packet_len;
    size_t offset;
    bool any_ok = false;

    if ((frame == NULL) || (dev == NULL))
    {
        return false;
    }

    payload = frame->payload;
    packet_len = frame->length;

    if ((packet_len == 0U) || (payload[0] != 0x80U))
    {
        return false;
    }

    offset = POOM_DRONE_BEACON_OFFSET;
    while ((offset + 1U) < packet_len)
    {
        uint8_t element_type = payload[offset];
        uint8_t element_len = payload[offset + 1U];
        size_t element_end = offset + POOM_DRONE_BEACON_ELEMENT_HDR_SIZE + element_len;

        if (element_end > packet_len)
        {
            break;
        }

        if (element_type == 0xddU)
        {
            if (element_len < (POOM_DRONE_VENDOR_OUI_SIZE + 1U + 1U))
            {
                offset = element_end;
                continue;
            }

            const uint8_t *oui = &payload[offset + 2U];
            const uint8_t oui_type = payload[offset + 2U + POOM_DRONE_VENDOR_OUI_SIZE];

            if ((oui_type == 0x0DU) && poom_drone_is_supported_vendor_oui_(oui, element_len))
            {
                size_t packed_start = offset + POOM_DRONE_BEACON_PACKET_OFFSET;
                if (packed_start >= element_end)
                {
                    offset = element_end;
                    continue;
                }

                const size_t packed_len = element_end - packed_start;
                if (poom_drone_decode_message_pack_(&dev->uas, &payload[packed_start], packed_len))
                {
                    any_ok = true;
                    break;
                }
            }
        }

        offset = element_end;
    }

    return any_ok;
}

/**
 * @brief Internal helper for `poom_drone_extract_nan_message_pack`.
 *
 * @param[in] payload Parameter passed to the function.
 * @param[in] payload_len Parameter passed to the function.
 * @param[in] out_pack Parameter passed to the function.
 * @param[in] out_pack_len Parameter passed to the function.
 * @return bool
 */
static bool poom_drone_extract_nan_message_pack_(const uint8_t *payload,
                                                 size_t payload_len,
                                                 const uint8_t **out_pack,
                                                 size_t *out_pack_len)
{
    const struct ieee80211_mgmt *mgmt;
    const struct nan_service_discovery *nsd;
    const struct nan_service_descriptor_attribute *nsda;
    size_t offset = 0U;

    if ((payload == NULL) || (out_pack == NULL) || (out_pack_len == NULL))
    {
        return false;
    }

    if (payload_len < sizeof(*mgmt))
    {
        return false;
    }

    mgmt = (const struct ieee80211_mgmt *)payload;
    if (memcmp(mgmt->da, s_nan_destination, POOM_DRONE_NAN_DEST_SIZE) != 0)
    {
        return false;
    }

    offset += sizeof(*mgmt);
    if ((offset + sizeof(*nsd)) > payload_len)
    {
        return false;
    }

    nsd = (const struct nan_service_discovery *)(payload + offset);
    offset += sizeof(*nsd);

    if ((nsd->category != 0x04U) || (nsd->action_code != 0x09U))
    {
        return false;
    }

    if ((offset + sizeof(*nsda)) > payload_len)
    {
        return false;
    }

    nsda = (const struct nan_service_descriptor_attribute *)(payload + offset);
    offset += sizeof(*nsda);

    if (nsda->header.attribute_id != 0x03U)
    {
        return false;
    }

    if (nsda->service_info_length < 1U)
    {
        return false;
    }

    if ((offset + (size_t)nsda->service_info_length) > payload_len)
    {
        return false;
    }

    *out_pack = payload + offset + 1U; /* skip message_counter */
    *out_pack_len = (size_t)nsda->service_info_length - 1U;
    return true;
}

/**
 * @brief Parses NAN action frames for OpenDroneID payloads.
 *
 * @param[in/out] frame Input frame metadata and payload.
 * @param[in/out] dev Destination device record (UAS data accumulator).
 * @return void
 */
static bool poom_drone_process_nan_frame_(
    const poom_drone_item_t *frame,
    poom_drone_device_t *dev)
{
    const uint8_t *pack = NULL;
    size_t pack_len = 0U;

    if ((frame == NULL) || (dev == NULL))
    {
        return false;
    }

    if (!poom_drone_extract_nan_message_pack_(frame->payload, frame->length, &pack, &pack_len))
    {
        return false;
    }

    return poom_drone_decode_message_pack_(&dev->uas, pack, pack_len);
}

/**
 * @brief Processes one queued item (Wi-Fi or BLE) and emits reports.
 * @return void
 */
static void poom_drone_process_item_(const poom_drone_item_t *item)
{
    poom_drone_device_t *dev;
    poom_drone_uav_data_t uav;
    bool updated = false;

    if (item == NULL)
    {
        return;
    }

    dev = poom_drone_get_device_(item->mac);
    if (dev == NULL)
    {
        return;
    }

    dev->last_seen_ms = poom_drone_now_ms_();

    if (item->src == POOM_DRONE_SRC_WIFI)
    {
        const bool nan_ok = poom_drone_process_nan_frame_(item, dev);
        const bool beacon_ok = poom_drone_process_beacon_frame_(item, dev);
        updated = nan_ok || beacon_ok;

        if (s_pcap_active && poom_pcap_manager_is_active())
        {
            (void)poom_pcap_manager_write_packet(item->payload, item->length, POOM_PCAP_CAPTURE_WIFI);
        }
    }
    else if (item->src == POOM_DRONE_SRC_BLE)
    {
        if (item->length >= sizeof(ODID_MessagePack_encoded) &&
            ((item->payload[0] >> 4) == ODID_MESSAGETYPE_PACKED))
        {
            updated = poom_drone_decode_message_pack_(&dev->uas, item->payload, item->length);
        }
        else if (item->length >= ODID_MESSAGE_SIZE)
        {
            (void)decodeOpenDroneID(&dev->uas, (uint8_t *)item->payload);
            updated = true;
        }
    }

    if (!updated)
    {
        return;
    }

    memset(&uav, 0, sizeof(uav));
    memcpy(uav.mac, item->mac, sizeof(uav.mac));

    if (poom_drone_fill_uav_report_(&uav, &dev->uas, (uint8_t)item->channel, item->rssi))
    {
        poom_drone_emit_report_(&uav);
    }
}

/**
 * @brief Runs parser loop that consumes queued management frames.
 *
 * @param[in/out] arg Task argument (unused).
 * @return void
 */
static void poom_drone_parser_task_(void* arg) {
    poom_drone_item_t item;

    (void)arg;

    while(s_scanner_running) {
        if((s_parse_queue != NULL) &&
           (xQueueReceive(s_parse_queue, &item, pdMS_TO_TICKS(200U)) == pdPASS)) {
            poom_drone_process_item_(&item);
        }
    }

    s_parser_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Receives promiscuous Wi-Fi packets and queues management frames.
 *
 * @param[in/out] buf Input buffer from Wi-Fi driver.
 * @param[in/out] type Promiscuous packet type.
 * @return void
 */
static void poom_drone_promiscuous_rx_cb_(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* packet;
    poom_drone_item_t item;
    size_t copy_len;

    if(!s_scanner_running || (type != WIFI_PKT_MGMT) || (buf == NULL) || (s_parse_queue == NULL)) {
        return;
    }

    packet = (const wifi_promiscuous_pkt_t*)buf;
    if(packet->rx_ctrl.sig_len <= 0) {
        return;
    }

    copy_len = (size_t)packet->rx_ctrl.sig_len;
    if(copy_len > POOM_DRONE_FRAME_MAX_LENGTH) {
        copy_len = POOM_DRONE_FRAME_MAX_LENGTH;
    }

    if(copy_len < POOM_DRONE_MIN_HEADER_LENGTH) {
        return;
    }

    memset(&item, 0, sizeof(item));
    item.src = POOM_DRONE_SRC_WIFI;
    item.length = (uint16_t)copy_len;
    item.rssi = packet->rx_ctrl.rssi;
    item.channel = (uint16_t)packet->rx_ctrl.channel;
    memcpy(item.payload, packet->payload, copy_len);
    memcpy(item.mac, &item.payload[10], sizeof(item.mac));

    (void)xQueueSend(s_parse_queue, &item, 0U);
}

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] scan_result Parameter passed to the function.
 * @return void
 */
static void poom_drone_ble_scan_cb_(const esp_ble_gap_cb_param_t *scan_result)
{
    const uint8_t *adv;
    uint8_t adv_len;

    if ((scan_result == NULL) || !s_scanner_running || (s_parse_queue == NULL))
    {
        return;
    }

    adv = scan_result->scan_rst.ble_adv;
    adv_len = (uint8_t)(scan_result->scan_rst.adv_data_len + scan_result->scan_rst.scan_rsp_len);
    if ((adv == NULL) || (adv_len == 0U))
    {
        return;
    }

    size_t i = 0U;
    while (i < adv_len)
    {
        uint8_t field_len = adv[i];
        if (field_len == 0U)
        {
            break;
        }

        size_t end = i + 1U + (size_t)field_len;
        if (end > adv_len)
        {
            break;
        }

        if (field_len >= 4U)
        {
            uint8_t field_type = adv[i + 1U];
            if (field_type == POOM_DRONE_BLE_AD_TYPE_SERVICE_DATA_16)
            {
                uint16_t uuid = (uint16_t)adv[i + 2U] | ((uint16_t)adv[i + 3U] << 8);
                if (uuid == POOM_DRONE_BLE_SERVICE_UUID_ODID)
                {
                    size_t payload_off = i + 4U; /* uuid bytes */
                    if (payload_off < end)
                    {
                        if (adv[payload_off] == POOM_DRONE_BLE_AD_CODE_ODID)
                        {
                            payload_off++;
                        }

                        bool has_counter = false;
                        if ((payload_off + 1U + ODID_MESSAGE_SIZE) <= end)
                        {
                            has_counter = true;
                        }
                        else if ((payload_off + ODID_MESSAGE_SIZE) > end)
                        {
                            continue;
                        }

                        {
                            poom_drone_item_t item = {0};
                            item.src = POOM_DRONE_SRC_BLE;
                            item.rssi = (int8_t)scan_result->scan_rst.rssi;
                            item.channel = 0U;
                            if (has_counter)
                            {
                                payload_off += 1U; /* skip msg_counter */
                            }
                            item.length = ODID_MESSAGE_SIZE;

                            memcpy(item.mac, scan_result->scan_rst.bda, sizeof(item.mac));
                            memcpy(item.payload, &adv[payload_off], item.length);
                            (void)xQueueSend(s_parse_queue, &item, 0U);
                        }
                    }
                }
            }
        }

        i = end;
    }
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_drone_wifi_start_(void)
{
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };

    if (s_wifi_active)
    {
        return ESP_OK;
    }

    esp_err_t err = poom_wifi_ctrl_init_null();
    if (err != ESP_OK)
    {
        return err;
    }

    (void)poom_wifi_ctrl_sta_disconnect();

    err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_promiscuous_rx_cb(poom_drone_promiscuous_rx_cb_);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK)
    {
        (void)esp_wifi_set_promiscuous_rx_cb(NULL);
        return err;
    }

    poom_drone_build_channel_list_();
    if (s_allowed_channel_count > 0U)
    {
        (void)esp_wifi_set_channel(s_allowed_channels[0], WIFI_SECOND_CHAN_NONE);
    }

    s_wifi_active = true;
    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_drone_wifi_stop_(void)
{
    if (!s_wifi_active)
    {
        return;
    }

    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);
    (void)poom_wifi_ctrl_deinit();

    s_wifi_active = false;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_drone_ble_start_(void)
{
    esp_err_t ret;

    if (s_ble_active)
    {
        return ESP_OK;
    }

    poom_ble_scan_set_uart_forward_enabled(false);
    poom_ble_scan_set_scan_type(BLE_SCAN_TYPE_ACTIVE);
    poom_ble_scan_set_filter_type(BLE_SCAN_FILTER_ALLOW_ALL);
    poom_ble_scan_register_cb(poom_drone_ble_scan_cb_);
    ret = poom_ble_scan_start();
    if (ret != ESP_OK)
    {
        poom_ble_scan_register_cb(NULL);
        return ret;
    }

    s_ble_active = true;
    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_drone_ble_stop_(void)
{
    if (!s_ble_active)
    {
        return;
    }

    (void)poom_ble_scan_stop();
    poom_ble_scan_register_cb(NULL);
    s_ble_active = false;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_drone_pcap_start_(void)
{
    if (s_pcap_active)
    {
        return ESP_OK;
    }

    if (!sd_card_is_mounted())
    {
        esp_err_t mount_err = sd_card_mount();
        if (mount_err != ESP_OK)
        {
            return mount_err;
        }
    }

    esp_err_t err = poom_pcap_manager_start_file("droneid", "/pcaps", POOM_PCAP_CAPTURE_WIFI, false);
    if (err != ESP_OK)
    {
        return err;
    }

    s_pcap_active = true;
    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_drone_pcap_stop_(void)
{
    if (!s_pcap_active)
    {
        return;
    }

    (void)poom_pcap_manager_close();
    (void)poom_pcap_manager_deinit();
    s_pcap_active = false;
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_drone_ctrl_task_(void *arg)
{
    (void)arg;

    const bool want_wifi = ((s_cfg.scan_mask & POOM_DRONE_SCAN_WIFI) != 0U);
    const bool want_ble = ((s_cfg.scan_mask & POOM_DRONE_SCAN_BLE) != 0U);
    const bool both = want_wifi && want_ble;

    while (s_scanner_running)
    {
        if (want_wifi)
        {
            if (poom_drone_wifi_start_() == ESP_OK)
            {
                const uint32_t end_ms = both ? (poom_drone_now_ms_() + s_cfg.wifi_phase_ms) : UINT32_MAX;
                while (s_scanner_running && (poom_drone_now_ms_() < end_ms))
                {
                    if (s_allowed_channel_count > 0U)
                    {
                        uint8_t ch = s_allowed_channels[s_channel_index % s_allowed_channel_count];
                        (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                        s_channel_index++;
                    }
                    vTaskDelay(pdMS_TO_TICKS((s_cfg.hop_interval_ms != 0U) ? s_cfg.hop_interval_ms : POOM_DRONE_DEFAULT_HOP_MS));
                }
            }

            if (both)
            {
                poom_drone_wifi_stop_();
            }
        }

        if (!s_scanner_running)
        {
            break;
        }

        if (want_ble)
        {
            if (poom_drone_ble_start_() == ESP_OK)
            {
                const uint32_t end_ms = both ? (poom_drone_now_ms_() + s_cfg.ble_phase_ms) : UINT32_MAX;
                while (s_scanner_running && (poom_drone_now_ms_() < end_ms))
                {
                    vTaskDelay(pdMS_TO_TICKS(200U));
                }
            }

            if (both)
            {
                poom_drone_ble_stop_();
            }
        }

        if (!both)
        {
            vTaskDelay(pdMS_TO_TICKS(200U));
        }
    }

    poom_drone_ble_stop_();
    poom_drone_wifi_stop_();

    s_ctrl_task_handle = NULL;
    vTaskDelete(NULL);
}

void poom_drone_config_default(poom_drone_config_t *out_cfg)
{
    if (out_cfg == NULL)
    {
        return;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->scan_mask = POOM_DRONE_SCAN_WIFI;
    out_cfg->wifi_phase_ms = POOM_DRONE_DEFAULT_WIFI_PHASE_MS;
    out_cfg->ble_phase_ms = POOM_DRONE_DEFAULT_BLE_PHASE_MS;
    out_cfg->hop_interval_ms = POOM_DRONE_DEFAULT_HOP_MS;
    out_cfg->enable_pcap_to_sd = false;
    out_cfg->enable_cli_print = false;
}

void poom_drone_register_report_cb(poom_drone_report_cb_t cb, void *user_ctx)
{
    s_report_cb = cb;
    s_report_cb_ctx = user_ctx;
}

bool poom_drone_is_running(void)
{
    return s_scanner_running;
}

/**
 * @brief Starts the DroneID scanner runtime.
 *
 * @param[in/out] none No input parameter.
 * @return esp_err_t
 */
esp_err_t poom_drone_start(void)
{
    return poom_drone_start_ex(NULL);
}

esp_err_t poom_drone_start_ex(const poom_drone_config_t *cfg)
{
    if (s_scanner_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_drone_config_default(&s_cfg);
    if (cfg != NULL)
    {
        s_cfg = *cfg;
        if (s_cfg.scan_mask == 0U)
        {
            s_cfg.scan_mask = POOM_DRONE_SCAN_WIFI;
        }
        if (s_cfg.hop_interval_ms == 0U)
        {
            s_cfg.hop_interval_ms = POOM_DRONE_DEFAULT_HOP_MS;
        }
        if (s_cfg.wifi_phase_ms == 0U)
        {
            s_cfg.wifi_phase_ms = POOM_DRONE_DEFAULT_WIFI_PHASE_MS;
        }
        if (s_cfg.ble_phase_ms == 0U)
        {
            s_cfg.ble_phase_ms = POOM_DRONE_DEFAULT_BLE_PHASE_MS;
        }
    }

    if (s_devices != NULL)
    {
        free(s_devices);
        s_devices = NULL;
    }

    s_devices = calloc(POOM_DRONE_MAX_DEVICES, sizeof(*s_devices));
    if (s_devices == NULL)
    {
        poom_drone_pcap_stop_();
        return ESP_ERR_NO_MEM;
    }

    if (s_cfg.enable_pcap_to_sd)
    {
        esp_err_t pcap_err = poom_drone_pcap_start_();
        if (pcap_err != ESP_OK)
        {
            return pcap_err;
        }
    }

    s_parse_queue = xQueueCreate(POOM_DRONE_PARSE_QUEUE_LENGTH, sizeof(poom_drone_item_t));
    if (s_parse_queue == NULL)
    {
        poom_drone_pcap_stop_();
        return ESP_ERR_NO_MEM;
    }

    s_scanner_running = true;

    if (xTaskCreate(poom_drone_parser_task_, "poom_drone_parser", 4096, NULL, 5, &s_parser_task_handle) != pdPASS)
    {
        (void)poom_drone_stop();
        return ESP_FAIL;
    }

    if (xTaskCreate(poom_drone_ctrl_task_, "poom_drone_ctrl", 3584, NULL, 5, &s_ctrl_task_handle) != pdPASS)
    {
        (void)poom_drone_stop();
        return ESP_FAIL;
    }

    POOM_DRONE_PRINTF_I("DroneID scanner started (mask=0x%lx)", (unsigned long)s_cfg.scan_mask);
    return ESP_OK;
}

/**
 * @brief Stops the DroneID scanner runtime.
 *
 * @param[in/out] none No input parameter.
 * @return esp_err_t
 */
esp_err_t poom_drone_stop(void)
{
    if (!s_scanner_running)
    {
        return ESP_OK;
    }

    s_scanner_running = false;

    if (s_ctrl_task_handle != NULL)
    {
        xTaskNotifyGive(s_ctrl_task_handle);
        vTaskDelay(pdMS_TO_TICKS(20U));
        vTaskDelete(s_ctrl_task_handle);
        s_ctrl_task_handle = NULL;
    }

    if (s_parser_task_handle != NULL)
    {
        xTaskNotifyGive(s_parser_task_handle);
        vTaskDelay(pdMS_TO_TICKS(20U));
        vTaskDelete(s_parser_task_handle);
        s_parser_task_handle = NULL;
    }

    poom_drone_ble_stop_();
    poom_drone_wifi_stop_();
    poom_drone_pcap_stop_();

    if (s_parse_queue != NULL)
    {
        vQueueDelete(s_parse_queue);
        s_parse_queue = NULL;
    }

    if (s_devices != NULL)
    {
        free(s_devices);
        s_devices = NULL;
    }

    POOM_DRONE_PRINTF_I("DroneID scanner stopped");
    return ESP_OK;
}
