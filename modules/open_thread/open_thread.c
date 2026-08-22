// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file open_thread.c
 * @brief OpenThread bring-up and helpers.
 */

#include "open_thread.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "open_thread_config.h"

#if CONFIG_LWIP_IPV6
#include "lwip/inet.h"
#include "lwip/sockets.h"
#endif

#if CONFIG_OPENTHREAD_FTD
  #include "openthread/dataset_ftd.h"
#endif

#define OPEN_THREAD_TAG "open_thread"

#define OPEN_THREAD_DEFAULT_CHANNEL (15U)
#define OPEN_THREAD_DEFAULT_PANID (0x1234U)
#define OPEN_THREAD_DEFAULT_NETWORK_NAME "OpenThread-ESP"
#define OPEN_THREAD_DEFAULT_EXTPANID_HEX "dead00beef00cafe"
#define OPEN_THREAD_DEFAULT_MESH_LOCAL_PREFIX "fd00:db8:a0:0::/64"
#define OPEN_THREAD_DEFAULT_MASTERKEY_HEX "00112233445566778899aabbccddeeff"
#define OPEN_THREAD_DEFAULT_PSKC_HEX "104810e2315100afd6bc9215a6bfac53"

#define POOM_OT_TCP_RX_TASK_STACK_WORDS (3072U)
#define POOM_OT_TCP_RX_TASK_PRIORITY (5U)
#define POOM_OT_TCP_RX_TIMEOUT_MS (200U)
#define POOM_OT_TCP_RX_BUF_SIZE (512U)

#define POOM_OT_TCP_SERVER_TASK_STACK_WORDS (4096U)
#define POOM_OT_TCP_SERVER_TASK_PRIORITY (5U)

static TaskHandle_t s_poom_ot_task = NULL;
static bool s_poom_ot_started = false;
static otOperationalDataset s_poom_ot_dataset;
static otIp6Address s_poom_ot_my_addr;

/**
 * @brief Initializes internal resources for this module.
 *
 * @param[in] config Parameter passed to the function.
 * @return esp_netif_t*
 */
static esp_netif_t* poom_ot_netif_init_(const esp_openthread_platform_config_t* config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t* netif = esp_netif_new(&cfg);
    if(netif == NULL)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "esp_netif_new failed");
        return NULL;
    }

    esp_err_t err = esp_netif_attach(netif, esp_openthread_netif_glue_init(config));
    if(err != ESP_OK)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        esp_netif_destroy(netif);
        return NULL;
    }

    return netif;
}

/**
 * @brief Internal helper for `poom_ot_hex_nibble`.
 *
 * @param[in] c Parameter passed to the function.
 * @return int
 */
static int poom_ot_hex_nibble_(char c)
{
    if((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if((c >= 'a') && (c <= 'f'))
    {
        return 10 + (c - 'a');
    }
    if((c >= 'A') && (c <= 'F'))
    {
        return 10 + (c - 'A');
    }
    return -1;
}

/**
 * @brief Internal helper for `poom_ot_hex_to_bytes`.
 *
 * @param[in] hex_string Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_ot_hex_to_bytes_(const char* hex_string, uint8_t* out, size_t out_len)
{
    if((hex_string == NULL) || (out == NULL) || (out_len == 0U))
    {
        return false;
    }

    const size_t n = strlen(hex_string);
    if(n != (out_len * 2U))
    {
        return false;
    }

    for(size_t i = 0U; i < out_len; i++)
    {
        int hi = poom_ot_hex_nibble_(hex_string[i * 2U]);
        int lo = poom_ot_hex_nibble_(hex_string[i * 2U + 1U]);
        if((hi < 0) || (lo < 0))
        {
            return false;
        }
        out[i] = (uint8_t)(((uint8_t)hi << 4) | (uint8_t)lo);
    }

    return true;
}

/**
 * @brief Refreshes the internal state used by this module.
 *
 * @param[in] instance Parameter passed to the function.
 * @return void
 */
static void poom_ot_refresh_my_addr_locked_(otInstance* instance)
{
    if(instance == NULL)
    {
        return;
    }

    const otNetifAddress* addr = otIp6GetUnicastAddresses(instance);
    while(addr != NULL)
    {
        if(addr->mValid)
        {
            s_poom_ot_my_addr = addr->mAddress;
        }
        addr = addr->mNext;
    }
}

/**
 * @brief Internal helper for `poom_ot_log_my_addr_locked`.
 *
 * @return void
 */
static void poom_ot_log_my_addr_locked_(void)
{
    char ip[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&s_poom_ot_my_addr, ip, sizeof(ip));
    ESP_LOGI(OPEN_THREAD_TAG, "IPv6: %s", ip);
}

/**
 * @brief Internal helper for `poom_ot_log_active_dataset_tlvs_locked`.
 *
 * @param[in] instance Parameter passed to the function.
 * @return void
 */
static void poom_ot_log_active_dataset_tlvs_locked_(otInstance* instance)
{
    otOperationalDatasetTlvs tlvs;
    otError error = otDatasetGetActiveTlvs(instance, &tlvs);
    if(error != OT_ERROR_NONE)
    {
        ESP_LOGW(OPEN_THREAD_TAG, "otDatasetGetActiveTlvs failed: %d", (int)error);
        return;
    }

    printf("DATASET: ");
    for(uint8_t i = 0U; i < tlvs.mLength; i++)
    {
        printf("%02x", tlvs.mTlvs[i]);
    }
    printf("\n");
}

/**
 * @brief Internal helper for `poom_ot_set_dataset_locked`.
 *
 * @param[in] instance Parameter passed to the function.
 * @param[in] channel Parameter passed to the function.
 * @param[in] panid Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_ot_set_dataset_locked_(otInstance* instance, uint8_t channel, uint16_t panid)
{
    if(instance == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_OPENTHREAD_FTD
    otDatasetCreateNewNetwork(instance, &s_poom_ot_dataset);
#else
    memset(&s_poom_ot_dataset, 0, sizeof(s_poom_ot_dataset));
#endif

    s_poom_ot_dataset.mActiveTimestamp.mSeconds = 1U;
    s_poom_ot_dataset.mActiveTimestamp.mTicks = 0U;
    s_poom_ot_dataset.mActiveTimestamp.mAuthoritative = false;
    s_poom_ot_dataset.mComponents.mIsActiveTimestampPresent = true;

    s_poom_ot_dataset.mChannel = channel;
    s_poom_ot_dataset.mComponents.mIsChannelPresent = true;
    s_poom_ot_dataset.mPanId = panid;
    s_poom_ot_dataset.mComponents.mIsPanIdPresent = true;

    size_t name_len = strlen(OPEN_THREAD_DEFAULT_NETWORK_NAME);
    if(name_len > OT_NETWORK_NAME_MAX_SIZE)
    {
        name_len = OT_NETWORK_NAME_MAX_SIZE;
    }
    memcpy(s_poom_ot_dataset.mNetworkName.m8, OPEN_THREAD_DEFAULT_NETWORK_NAME, name_len);
    s_poom_ot_dataset.mNetworkName.m8[name_len] = '\0';
    s_poom_ot_dataset.mComponents.mIsNetworkNamePresent = true;

    if(!poom_ot_hex_to_bytes_(OPEN_THREAD_DEFAULT_EXTPANID_HEX,
                                 s_poom_ot_dataset.mExtendedPanId.m8,
                                 sizeof(s_poom_ot_dataset.mExtendedPanId.m8)))
    {
        ESP_LOGE(OPEN_THREAD_TAG, "Invalid ext PANID hex");
        return ESP_ERR_INVALID_ARG;
    }
    s_poom_ot_dataset.mComponents.mIsExtendedPanIdPresent = true;

    otIp6Prefix prefix;
    memset(&prefix, 0, sizeof(prefix));
    if(otIp6PrefixFromString(OPEN_THREAD_DEFAULT_MESH_LOCAL_PREFIX, &prefix) != OT_ERROR_NONE)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "Invalid mesh-local prefix: %s", OPEN_THREAD_DEFAULT_MESH_LOCAL_PREFIX);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_poom_ot_dataset.mMeshLocalPrefix.m8, prefix.mPrefix.mFields.m8, sizeof(s_poom_ot_dataset.mMeshLocalPrefix.m8));
    s_poom_ot_dataset.mComponents.mIsMeshLocalPrefixPresent = true;

    if(!poom_ot_hex_to_bytes_(OPEN_THREAD_DEFAULT_MASTERKEY_HEX,
                                 s_poom_ot_dataset.mNetworkKey.m8,
                                 sizeof(s_poom_ot_dataset.mNetworkKey.m8)))
    {
        ESP_LOGE(OPEN_THREAD_TAG, "Invalid master key hex");
        return ESP_ERR_INVALID_ARG;
    }
    s_poom_ot_dataset.mComponents.mIsNetworkKeyPresent = true;

    if(!poom_ot_hex_to_bytes_(OPEN_THREAD_DEFAULT_PSKC_HEX, s_poom_ot_dataset.mPskc.m8, sizeof(s_poom_ot_dataset.mPskc.m8)))
    {
        ESP_LOGE(OPEN_THREAD_TAG, "Invalid PSKc hex");
        return ESP_ERR_INVALID_ARG;
    }
    s_poom_ot_dataset.mComponents.mIsPskcPresent = true;

    otError error = otDatasetSetActive(instance, &s_poom_ot_dataset);
    if(error != OT_ERROR_NONE)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "otDatasetSetActive failed: %d", (int)error);
        return ESP_FAIL;
    }

    (void)otIp6SetEnabled(instance, true);
    (void)otThreadSetEnabled(instance, true);
    return ESP_OK;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_ot_init_platform_(void)
{
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if(err != ESP_OK)
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        return err;
    }

    err = esp_netif_init();
    if(err != ESP_OK)
    {
        return err;
    }

    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 3};
    err = esp_vfs_eventfd_register(&eventfd_config);
    if(err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_ot_task_(void* arg)
{
    (void)arg;

    const esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_openthread_init(&config));

    esp_netif_t* ot_netif = poom_ot_netif_init_(&config);
    if(ot_netif == NULL)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "OpenThread netif init failed");
        esp_openthread_deinit();
        s_poom_ot_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    esp_netif_set_default_netif(ot_netif);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();
    (void)poom_ot_set_dataset_locked_(instance, OPEN_THREAD_DEFAULT_CHANNEL, OPEN_THREAD_DEFAULT_PANID);
    poom_ot_refresh_my_addr_locked_(instance);
    poom_ot_log_my_addr_locked_();
    poom_ot_log_active_dataset_tlvs_locked_(instance);
    esp_openthread_lock_release();

    esp_openthread_launch_mainloop();

    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(ot_netif);
    esp_vfs_eventfd_unregister();
    s_poom_ot_task = NULL;
    vTaskDelete(NULL);
}

void poom_ot_init(void)
{
    if(s_poom_ot_started)
    {
        return;
    }
    s_poom_ot_started = true;

#if defined(CONFIG_OPEN_THREAD_DEBUG)
    esp_log_level_set(OPEN_THREAD_TAG, ESP_LOG_INFO);
#else
    esp_log_level_set(OPEN_THREAD_TAG, ESP_LOG_NONE);
#endif

    ESP_ERROR_CHECK(poom_ot_init_platform_());

    if(s_poom_ot_task == NULL)
    {
        (void)xTaskCreate(poom_ot_task_, "open_thread", 1024U * 5U, NULL, 10, &s_poom_ot_task);
    }
}

void poom_ot_deinit(void)
{
    esp_openthread_deinit();
}

void poom_ot_set_channel(uint8_t channel)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otLinkSetChannel(esp_openthread_get_instance(), channel);
    esp_openthread_lock_release();
}

esp_err_t poom_ot_set_dataset(uint8_t channel, uint16_t panid)
{
    esp_err_t result;

    esp_openthread_lock_acquire(portMAX_DELAY);
    result = poom_ot_set_dataset_locked_(esp_openthread_get_instance(), channel, panid);
    poom_ot_refresh_my_addr_locked_(esp_openthread_get_instance());
    esp_openthread_lock_release();

    return result;
}

otIp6Address poom_ot_get_my_ipv6address(void)
{
    return s_poom_ot_my_addr;
}

void poom_ot_factory_reset(void)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstanceFactoryReset(esp_openthread_get_instance());
    esp_openthread_lock_release();
}

otError poom_ot_ipmaddr_subscribe(const char* address)
{
    otError error;
    otIp6Address addr;

    if(address == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otIp6AddressFromString(address, &addr);
    if(error == OT_ERROR_NONE)
    {
        error = otIp6SubscribeMulticastAddress(esp_openthread_get_instance(), &addr);
    }
    esp_openthread_lock_release();

    return error;
}

otError poom_ot_ipmaddr_unsubscribe(const char* address)
{
    otError error;
    otIp6Address addr;

    if(address == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otIp6AddressFromString(address, &addr);
    if(error == OT_ERROR_NONE)
    {
        error = otIp6UnsubscribeMulticastAddress(esp_openthread_get_instance(), &addr);
    }
    esp_openthread_lock_release();

    return error;
}

otError poom_ot_udp_open(otUdpSocket* socket, otUdpReceive receive_cb)
{
    otError error = OT_ERROR_NONE;

    if((socket == NULL) || (receive_cb == NULL))
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();

    if(otUdpIsOpen(instance, socket))
    {
        error = OT_ERROR_ALREADY;
    }
    else
    {
        error = otUdpOpen(instance, socket, receive_cb, NULL);
    }

    esp_openthread_lock_release();
    return error;
}

otError poom_ot_udp_bind(otUdpSocket* socket, uint16_t port)
{
    otError error;
    otSockAddr sockaddr;

    if(socket == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = port;

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otUdpBind(esp_openthread_get_instance(), socket, &sockaddr, OT_NETIF_THREAD_HOST);
    esp_openthread_lock_release();
    return error;
}

otError poom_ot_udp_close(otUdpSocket* socket)
{
    otError error;

    if(socket == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otUdpClose(esp_openthread_get_instance(), socket);
    esp_openthread_lock_release();
    return error;
}

otError poom_ot_udp_send(otUdpSocket* socket, const char* dst_ipv6, uint16_t port, const void* data, size_t data_size)
{
    otError error = OT_ERROR_NONE;

    if((socket == NULL) || (dst_ipv6 == NULL) || (data == NULL) || (data_size == 0U))
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();

    otMessageInfo messageInfo;
    memset(&messageInfo, 0, sizeof(messageInfo));
    error = otIp6AddressFromString(dst_ipv6, &messageInfo.mPeerAddr);
    if(error != OT_ERROR_NONE)
    {
        goto out;
    }
    messageInfo.mPeerPort = port;

    otMessageSettings messageSettings = {
        .mLinkSecurityEnabled = true,
        .mPriority = OT_MESSAGE_PRIORITY_HIGH,
    };

    otMessage* message = otUdpNewMessage(instance, &messageSettings);
    if(message == NULL)
    {
        error = OT_ERROR_NO_BUFS;
        goto out;
    }

    error = otMessageAppend(message, data, data_size);
    if(error != OT_ERROR_NONE)
    {
        otMessageFree(message);
        goto out;
    }

    error = otUdpSend(instance, socket, message, &messageInfo);
    if(error != OT_ERROR_NONE)
    {
        otMessageFree(message);
        goto out;
    }

out:
    esp_openthread_lock_release();
    return error;
}

otError poom_ot_enable_promiscuous_mode(otLinkPcapCallback promiscuous_cb)
{
    otError error;

    if(promiscuous_cb == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();

    (void)otIp6SetEnabled(instance, false);
    (void)otThreadSetEnabled(instance, false);

    error = otLinkSetPromiscuous(instance, true);
    if(error != OT_ERROR_NONE)
    {
        goto out;
    }

    otLinkSetPcapCallback(instance, promiscuous_cb, NULL);
    (void)otIp6SetEnabled(instance, true);
    (void)otThreadSetEnabled(instance, true);

out:
    esp_openthread_lock_release();
    return error;
}

otError poom_ot_disable_promiscuous_mode(void)
{
    otError error;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance* instance = esp_openthread_get_instance();

    (void)otIp6SetEnabled(instance, false);
    (void)otThreadSetEnabled(instance, false);
    otLinkSetPcapCallback(instance, NULL, NULL);
    error = otLinkSetPromiscuous(instance, false);
    (void)otIp6SetEnabled(instance, true);
    (void)otThreadSetEnabled(instance, true);

    esp_openthread_lock_release();
    return error;
}

esp_err_t poom_ot_tcp_send(const char* dst_ipv6,
                           uint16_t port,
                           const void* data,
                           size_t data_size,
                           uint32_t timeout_ms)
{
#if !CONFIG_LWIP_IPV6
    (void)dst_ipv6;
    (void)port;
    (void)data;
    (void)data_size;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if((dst_ipv6 == NULL) || (data == NULL) || (data_size == 0U) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    if(timeout_ms > 0U)
    {
        struct timeval tv;
        tv.tv_sec = (time_t)(timeout_ms / 1000U);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
        (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    if(inet_pton(AF_INET6, dst_ipv6, &addr.sin6_addr) != 1)
    {
        (void)close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp connect failed: errno=%d", errno);
        (void)close(sock);
        return ESP_FAIL;
    }

    const uint8_t* buf = (const uint8_t*)data;
    size_t sent = 0U;
    while(sent < data_size)
    {
        ssize_t n = send(sock, buf + sent, data_size - sent, 0);
        if(n <= 0)
        {
            ESP_LOGE(OPEN_THREAD_TAG, "tcp send failed: errno=%d", errno);
            (void)close(sock);
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }

    (void)shutdown(sock, SHUT_RDWR);
    (void)close(sock);
    return ESP_OK;
#endif
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_ot_tcp_rx_task_(void* arg)
{
#if !CONFIG_LWIP_IPV6
    (void)arg;
    vTaskDelete(NULL);
#else
    poom_ot_tcp_client_t* client = (poom_ot_tcp_client_t*)arg;
    uint8_t buf[POOM_OT_TCP_RX_BUF_SIZE];

    if(client == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    while(!client->stop_requested)
    {
        int sock = client->sock;
        if(sock < 0)
        {
            break;
        }

        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if(n > 0)
        {
            if(client->rx_cb != NULL)
            {
                client->rx_cb(buf, (size_t)n, client->rx_cb_user_ctx);
            }
            continue;
        }

        if(n == 0)
        {
            break;
        }

        if((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == ETIMEDOUT))
        {
            continue;
        }

        break;
    }

    int sock = client->sock;
    client->sock = -1;
    if(sock >= 0)
    {
        (void)shutdown(sock, SHUT_RDWR);
        (void)close(sock);
    }

    client->rx_task = NULL;
    vTaskDelete(NULL);
#endif
}

esp_err_t poom_ot_tcp_client_open(poom_ot_tcp_client_t* client,
                                  const char* dst_ipv6,
                                  uint16_t port,
                                  uint32_t timeout_ms,
                                  poom_ot_tcp_rx_cb_t rx_cb,
                                  void* user_ctx)
{
#if !CONFIG_LWIP_IPV6
    (void)client;
    (void)dst_ipv6;
    (void)port;
    (void)timeout_ms;
    (void)rx_cb;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if((client == NULL) || (dst_ipv6 == NULL) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(client, 0, sizeof(*client));
    client->sock = -1;
    client->rx_cb = rx_cb;
    client->rx_cb_user_ctx = user_ctx;
    client->stop_requested = false;

    int sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    if(timeout_ms > 0U)
    {
        struct timeval tv;
        tv.tv_sec = (time_t)(timeout_ms / 1000U);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
        (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    struct timeval tv_rx;
    tv_rx.tv_sec = (time_t)(POOM_OT_TCP_RX_TIMEOUT_MS / 1000U);
    tv_rx.tv_usec = (suseconds_t)((POOM_OT_TCP_RX_TIMEOUT_MS % 1000U) * 1000U);
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rx, sizeof(tv_rx));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    if(inet_pton(AF_INET6, dst_ipv6, &addr.sin6_addr) != 1)
    {
        (void)close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp connect failed: errno=%d", errno);
        (void)close(sock);
        return ESP_FAIL;
    }

    client->sock = sock;

    TaskHandle_t rx_task = NULL;
    BaseType_t ok = xTaskCreate(poom_ot_tcp_rx_task_,
                                "poom_ot_tcp_rx",
                                POOM_OT_TCP_RX_TASK_STACK_WORDS,
                                client,
                                POOM_OT_TCP_RX_TASK_PRIORITY,
                                &rx_task);
    if(ok != pdPASS)
    {
        client->sock = -1;
        (void)shutdown(sock, SHUT_RDWR);
        (void)close(sock);
        return ESP_ERR_NO_MEM;
    }

    client->rx_task = (void*)rx_task;
    return ESP_OK;
#endif
}

esp_err_t poom_ot_tcp_client_send(poom_ot_tcp_client_t* client, const void* data, size_t data_size)
{
#if !CONFIG_LWIP_IPV6
    (void)client;
    (void)data;
    (void)data_size;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if((client == NULL) || (data == NULL) || (data_size == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(client->sock < 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t* buf = (const uint8_t*)data;
    size_t sent = 0U;
    while(sent < data_size)
    {
        ssize_t n = send(client->sock, buf + sent, data_size - sent, 0);
        if(n <= 0)
        {
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }

    return ESP_OK;
#endif
}

void poom_ot_tcp_client_close(poom_ot_tcp_client_t* client)
{
#if !CONFIG_LWIP_IPV6
    (void)client;
#else
    if(client == NULL)
    {
        return;
    }

    client->stop_requested = true;
    if(client->sock >= 0)
    {
        (void)shutdown(client->sock, SHUT_RDWR);
    }

    if((client->rx_task == NULL) && (client->sock >= 0))
    {
        int sock = client->sock;
        client->sock = -1;
        (void)close(sock);
    }
#endif
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_ot_tcp_server_task_(void* arg)
{
#if !CONFIG_LWIP_IPV6
    (void)arg;
    vTaskDelete(NULL);
#else
    poom_ot_tcp_server_t* server = (poom_ot_tcp_server_t*)arg;
    uint8_t buf[POOM_OT_TCP_RX_BUF_SIZE];

    if(server == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    while(!server->stop_requested)
    {
        if(server->listen_sock < 0)
        {
            break;
        }

        if(server->client_sock < 0)
        {
            struct sockaddr_in6 src_addr;
            socklen_t addr_len = sizeof(src_addr);
            int sock = accept(server->listen_sock, (struct sockaddr*)&src_addr, &addr_len);
            if(sock < 0)
            {
                if((errno == EWOULDBLOCK) || (errno == EAGAIN))
                {
                    vTaskDelay(pdMS_TO_TICKS(30U));
                    continue;
                }
                break;
            }

            struct timeval tv_rx;
            tv_rx.tv_sec = (time_t)(POOM_OT_TCP_RX_TIMEOUT_MS / 1000U);
            tv_rx.tv_usec = (suseconds_t)((POOM_OT_TCP_RX_TIMEOUT_MS % 1000U) * 1000U);
            (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rx, sizeof(tv_rx));

            if(server->stop_requested)
            {
                (void)shutdown(sock, SHUT_RDWR);
                (void)close(sock);
                break;
            }

            server->client_sock = sock;
            continue;
        }

        ssize_t n = recv(server->client_sock, buf, sizeof(buf), 0);
        if(n > 0)
        {
            if(server->rx_cb != NULL)
            {
                server->rx_cb(buf, (size_t)n, server->rx_cb_user_ctx);
            }
            continue;
        }

        if(n == 0)
        {
            int sock = server->client_sock;
            server->client_sock = -1;
            (void)shutdown(sock, SHUT_RDWR);
            (void)close(sock);
            continue;
        }

        if((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == ETIMEDOUT))
        {
            continue;
        }

        int sock = server->client_sock;
        server->client_sock = -1;
        (void)shutdown(sock, SHUT_RDWR);
        (void)close(sock);
    }

    int client = server->client_sock;
    server->client_sock = -1;
    if(client >= 0)
    {
        (void)shutdown(client, SHUT_RDWR);
        (void)close(client);
    }

    int listen_sock = server->listen_sock;
    server->listen_sock = -1;
    if(listen_sock >= 0)
    {
        (void)shutdown(listen_sock, SHUT_RDWR);
        (void)close(listen_sock);
    }

    server->task = NULL;
    vTaskDelete(NULL);
#endif
}

esp_err_t poom_ot_tcp_server_start(poom_ot_tcp_server_t* server,
                                   uint16_t port,
                                   poom_ot_tcp_rx_cb_t rx_cb,
                                   void* user_ctx)
{
#if !CONFIG_LWIP_IPV6
    (void)server;
    (void)port;
    (void)rx_cb;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if((server == NULL) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(server->task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(server, 0, sizeof(*server));
    server->listen_sock = -1;
    server->client_sock = -1;
    server->rx_cb = rx_cb;
    server->rx_cb_user_ctx = user_ctx;
    server->stop_requested = false;

    int listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if(listen_sock < 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp server socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    int yes = 1;
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;

    if(bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp server bind failed: errno=%d", errno);
        (void)close(listen_sock);
        return ESP_FAIL;
    }

    if(listen(listen_sock, 1) != 0)
    {
        ESP_LOGE(OPEN_THREAD_TAG, "tcp server listen failed: errno=%d", errno);
        (void)close(listen_sock);
        return ESP_FAIL;
    }

    int flags = fcntl(listen_sock, F_GETFL, 0);
    if(flags >= 0)
    {
        (void)fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);
    }

    server->listen_sock = listen_sock;

    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreate(poom_ot_tcp_server_task_,
                                "poom_ot_tcp_srv",
                                POOM_OT_TCP_SERVER_TASK_STACK_WORDS,
                                server,
                                POOM_OT_TCP_SERVER_TASK_PRIORITY,
                                &task);
    if(ok != pdPASS)
    {
        server->listen_sock = -1;
        (void)close(listen_sock);
        return ESP_ERR_NO_MEM;
    }

    server->task = (void*)task;
    return ESP_OK;
#endif
}

esp_err_t poom_ot_tcp_server_send(poom_ot_tcp_server_t* server, const void* data, size_t data_size)
{
#if !CONFIG_LWIP_IPV6
    (void)server;
    (void)data;
    (void)data_size;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if((server == NULL) || (data == NULL) || (data_size == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(server->client_sock < 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t* buf = (const uint8_t*)data;
    size_t sent = 0U;
    while(sent < data_size)
    {
        ssize_t n = send(server->client_sock, buf + sent, data_size - sent, 0);
        if(n <= 0)
        {
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }

    return ESP_OK;
#endif
}

void poom_ot_tcp_server_stop(poom_ot_tcp_server_t* server)
{
#if !CONFIG_LWIP_IPV6
    (void)server;
#else
    if(server == NULL)
    {
        return;
    }

    server->stop_requested = true;

    if(server->client_sock >= 0)
    {
        (void)shutdown(server->client_sock, SHUT_RDWR);
    }

    if(server->listen_sock >= 0)
    {
        (void)shutdown(server->listen_sock, SHUT_RDWR);
    }
#endif
}
