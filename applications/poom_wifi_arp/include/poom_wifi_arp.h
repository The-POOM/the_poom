// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_ARP_H
#define POOM_WIFI_ARP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief ARP scan host entry (IPv4 + MAC).
 */
typedef struct
{
    char ip[16];      /**< "xxx.xxx.xxx.xxx" */
    uint8_t mac[6];   /**< MAC address */
    bool is_active;   /**< true when host responded */
} poom_wifi_arp_scan_host_t;

/**
 * @brief ARP subnet scanner context.
 *
 * Caller owns the hosts array.
 */
typedef struct
{
    poom_wifi_arp_scan_host_t *hosts; /**< Output host list buffer. */
    size_t num_active_hosts;          /**< Output: number of active hosts found. */
    size_t max_hosts;                 /**< Capacity of hosts[]. */
    char subnet_prefix[16];           /**< Output: e.g. "192.168.1." */
} poom_wifi_arp_scan_ctx_t;

/** @brief Max number of ports recorded in a host probe. */
#define POOM_WIFI_ARP_PROBE_MAX_PORTS (16U)

/**
 * @brief Port probe result for a single host.
 */
typedef struct
{
    uint16_t open_ports[POOM_WIFI_ARP_PROBE_MAX_PORTS];
    size_t num_open_ports;
} poom_wifi_arp_port_probe_result_t;

/**
 * @brief Combined host scan result (TCP + UDP + optional SSH banner).
 */
typedef struct
{
    poom_wifi_arp_port_probe_result_t tcp;
    poom_wifi_arp_port_probe_result_t udp;
    bool ssh_found;
    uint16_t ssh_port;
    char ssh_banner[64];
} poom_wifi_arp_service_probe_result_t;

/**
 * @brief Scan local subnet using ARP requests (STA must have IP).
 *
 * This sends ARP requests over the STA interface and inspects the lwIP ARP
 * table for responses.
 *
 * @param[in,out] ctx Scan context with host buffer.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_wifi_arp_scan_subnet(poom_wifi_arp_scan_ctx_t *ctx);

/**
 * @brief Probe a single TCP port on a host (connect-based).
 *
 * @param[in] target_ip IPv4 string, e.g. "192.168.1.10".
 * @param[in] port TCP port number.
 * @param[out] out_open true when the port is reachable.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_wifi_arp_probe_tcp_port(const char *target_ip, uint16_t port, bool *out_open);

/**
 * @brief Probe a single UDP port on a host (response-based).
 *
 * Note: UDP services may not respond to unsolicited probes; this is best-effort.
 *
 * @param[in] target_ip IPv4 string, e.g. "192.168.1.10".
 * @param[in] port UDP port number.
 * @param[out] out_responded true when a response was received.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_wifi_arp_probe_udp_port(const char *target_ip, uint16_t port, bool *out_responded);

/**
 * @brief Probe an SSH service on a host and port (banner-based).
 *
 * This checks whether an SSH banner is received (typically starts with "SSH-").
 *
 * @param[in] target_ip IPv4 string, e.g. "192.168.1.10".
 * @param[in] port SSH port number.
 * @param[out] out_is_ssh true when an SSH banner was detected.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_wifi_arp_probe_ssh_port(const char *target_ip, uint16_t port, bool *out_is_ssh);

/**
 * @brief Probe a host and collect TCP/UDP results plus SSH banner (if any).
 *
 * @param[in] target_ip IPv4 string, e.g. "192.168.1.10".
 * @param[out] out Combined scan result.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_wifi_arp_probe_host_services(const char *target_ip, poom_wifi_arp_service_probe_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_ARP_H */
