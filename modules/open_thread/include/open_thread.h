#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file open_thread.h
 * @brief POOM wrapper helpers for ESP-IDF OpenThread + basic socket utilities.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/logging.h"
#include "openthread/message.h"
#include "openthread/tasklet.h"
#include "openthread/thread.h"
#include "openthread/udp.h"

/**
 * @brief TCP receive callback used by POOM TCP client/server helpers.
 *
 * @param[in] data Data buffer (valid only during callback).
 * @param[in] data_len Number of bytes received.
 * @param[in] user_ctx User context passed at open/start.
 */
typedef void (*poom_ot_tcp_rx_cb_t)(const uint8_t* data, size_t data_len, void* user_ctx);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP client descriptor (single connection + RX task).
 *
 * @note The fields are managed by this module; treat as opaque.
 */
typedef struct
{
    int sock;
    void* rx_task;
    poom_ot_tcp_rx_cb_t rx_cb;
    void* rx_cb_user_ctx;
    volatile bool stop_requested;
} poom_ot_tcp_client_t;

/**
 * @brief TCP server descriptor (single listening socket + optional single client + task).
 *
 * @note This server accepts at most one client at a time. When the client disconnects,
 *       the server continues listening for the next one until stopped.
 * @note The fields are managed by this module; treat as opaque.
 */
typedef struct
{
    int listen_sock;
    int client_sock;
    void* task;
    poom_ot_tcp_rx_cb_t rx_cb;
    void* rx_cb_user_ctx;
    volatile bool stop_requested;
} poom_ot_tcp_server_t;

/**
 * @brief Initializes OpenThread platform and starts the OpenThread mainloop task.
 *
 * @note Safe to call multiple times (no-op after first call).
 */
void poom_ot_init(void);

/**
 * @brief Deinitializes OpenThread stack.
 *
 * @note This does not currently stop the OpenThread task created by `poom_ot_init()`.
 */
void poom_ot_deinit(void);

/**
 * @brief Applies an active dataset and enables IPv6 + Thread.
 *
 * @param[in] channel Thread channel.
 * @param[in] panid Thread PANID.
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_set_dataset(uint8_t channel, uint16_t panid);

/**
 * @brief Sets Thread MAC channel (runtime).
 *
 * @param[in] channel Thread channel.
 */
void poom_ot_set_channel(uint8_t channel);

/**
 * @brief Factory reset the OpenThread instance.
 */
void poom_ot_factory_reset(void);

/**
 * @brief Returns one of the device IPv6 unicast addresses (last valid seen).
 *
 * @return IPv6 address.
 */
otIp6Address poom_ot_get_my_ipv6address(void);

/**
 * @brief Subscribe multicast IPv6 address on the Thread interface.
 *
 * @param[in] address IPv6 string (e.g. "ff03::1").
 * @return OpenThread error code.
 */
otError poom_ot_ipmaddr_subscribe(const char* address);

/**
 * @brief Unsubscribe multicast IPv6 address on the Thread interface.
 *
 * @param[in] address IPv6 string.
 * @return OpenThread error code.
 */
otError poom_ot_ipmaddr_unsubscribe(const char* address);

/**
 * @brief Enables promiscuous mode and registers PCAP callback.
 *
 * @param[in] promiscuous_cb PCAP callback.
 * @return OpenThread error code.
 */
otError poom_ot_enable_promiscuous_mode(otLinkPcapCallback promiscuous_cb);

/**
 * @brief Disables promiscuous mode and clears PCAP callback.
 *
 * @return OpenThread error code.
 */
otError poom_ot_disable_promiscuous_mode(void);

/**
 * @brief Opens an OpenThread UDP socket.
 *
 * @param[in,out] socket UDP socket object.
 * @param[in] receive_cb Receive callback (OpenThread context).
 * @return OpenThread error code.
 */
otError poom_ot_udp_open(otUdpSocket* socket, otUdpReceive receive_cb);

/**
 * @brief Binds an OpenThread UDP socket to `port`.
 *
 * @param[in,out] socket UDP socket object.
 * @param[in] port Local port.
 * @return OpenThread error code.
 */
otError poom_ot_udp_bind(otUdpSocket* socket, uint16_t port);

/**
 * @brief Closes an OpenThread UDP socket.
 *
 * @param[in,out] socket UDP socket object.
 * @return OpenThread error code.
 */
otError poom_ot_udp_close(otUdpSocket* socket);

/**
 * @brief Sends a UDP datagram using an OpenThread UDP socket.
 *
 * @param[in,out] socket UDP socket object.
 * @param[in] dst_ipv6 Destination IPv6 string.
 * @param[in] port Destination port.
 * @param[in] data Payload pointer.
 * @param[in] data_size Payload size.
 * @return OpenThread error code.
 */
otError poom_ot_udp_send(otUdpSocket* socket,
                         const char* dst_ipv6,
                         uint16_t port,
                         const void* data,
                         size_t data_size);

/**
 * @brief One-shot TCP connect+send helper (IPv6).
 *
 * @param[in] dst_ipv6 Destination IPv6 string.
 * @param[in] port Destination TCP port.
 * @param[in] data Data to send.
 * @param[in] data_size Data size.
 * @param[in] timeout_ms Socket send/connect timeout (best-effort).
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_tcp_send(const char* dst_ipv6,
                           uint16_t port,
                           const void* data,
                           size_t data_size,
                           uint32_t timeout_ms);

/**
 * @brief Opens a persistent TCP client connection and starts an RX task.
 *
 * @param[out] client Client descriptor.
 * @param[in] dst_ipv6 Destination IPv6 string.
 * @param[in] port Destination port.
 * @param[in] timeout_ms Send timeout (best-effort).
 * @param[in] rx_cb Receive callback invoked from a FreeRTOS task context.
 * @param[in] user_ctx User context passed to callback.
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_tcp_client_open(poom_ot_tcp_client_t* client,
                                  const char* dst_ipv6,
                                  uint16_t port,
                                  uint32_t timeout_ms,
                                  poom_ot_tcp_rx_cb_t rx_cb,
                                  void* user_ctx);

/**
 * @brief Sends bytes on an opened TCP client.
 *
 * @param[in,out] client Client descriptor.
 * @param[in] data Data to send.
 * @param[in] data_size Data size.
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_tcp_client_send(poom_ot_tcp_client_t* client,
                                  const void* data,
                                  size_t data_size);

/**
 * @brief Requests the TCP client RX task to stop and closes the socket.
 *
 * @param[in,out] client Client descriptor.
 */
void poom_ot_tcp_client_close(poom_ot_tcp_client_t* client);

/**
 * @brief Starts a TCP server on given port and a background task.
 *
 * @param[out] server Server descriptor.
 * @param[in] port Listen port.
 * @param[in] rx_cb Receive callback invoked from a FreeRTOS task context.
 * @param[in] user_ctx User context passed to callback.
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_tcp_server_start(poom_ot_tcp_server_t* server,
                                   uint16_t port,
                                   poom_ot_tcp_rx_cb_t rx_cb,
                                   void* user_ctx);

/**
 * @brief Sends bytes to currently connected server client.
 *
 * @param[in,out] server Server descriptor.
 * @param[in] data Data to send.
 * @param[in] data_size Data size.
 * @return `ESP_OK` on success.
 */
esp_err_t poom_ot_tcp_server_send(poom_ot_tcp_server_t* server, const void* data, size_t data_size);

/**
 * @brief Stops the TCP server task and closes sockets.
 *
 * @param[in,out] server Server descriptor.
 */
void poom_ot_tcp_server_stop(poom_ot_tcp_server_t* server);

#if !defined(POOM_OT_DISABLE_LEGACY_API)
#define openthread_init poom_ot_init
#define openthread_deinit poom_ot_deinit
#define openthread_set_dataset poom_ot_set_dataset
#define openthread_udp_open poom_ot_udp_open
#define openthread_udp_bind poom_ot_udp_bind
#define openthread_udp_close poom_ot_udp_close
#define openthread_udp_send poom_ot_udp_send
#define openthread_get_my_ipv6address poom_ot_get_my_ipv6address
#define openthread_ipmaddr_subscribe poom_ot_ipmaddr_subscribe
#define openthread_ipmaddr_unsubscribe poom_ot_ipmaddr_unsubscribe
#define openthread_enable_promiscous_mode poom_ot_enable_promiscuous_mode
#define openthread_disable_promiscous_mode poom_ot_disable_promiscuous_mode
#define openthread_set_channel poom_ot_set_channel
#define openthread_factory_reset poom_ot_factory_reset
#define openthread_tcp_send poom_ot_tcp_send
#endif

#ifdef __cplusplus
}
#endif
