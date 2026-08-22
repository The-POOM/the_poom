// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_arp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "poom_wifi_ctrl.h"

#define POOM_WIFI_ARP_MAC_LEN            (6)
#define POOM_WIFI_ARP_IPV4_LEN           (4)

#define POOM_WIFI_ARP_SCAN_START_HOST (1)
#define POOM_WIFI_ARP_SCAN_END_HOST (254)
#define POOM_WIFI_ARP_SCAN_BATCH_SIZE (10)
#define POOM_WIFI_ARP_SCAN_REQ_DELAY_MS (10)
#define POOM_WIFI_ARP_SCAN_RESP_WAIT_MS (350)

#define POOM_WIFI_ARP_TCP_PROBE_TIMEOUT_MS (120U)
#define POOM_WIFI_ARP_TCP_PROBE_INTER_DELAY_MS (8U)
#define POOM_WIFI_ARP_UDP_PROBE_TIMEOUT_MS (60U)

#if CONFIG_POOM_WIFI_ARP_ENABLE_LOG

static const char *POOM_WIFI_ARP_TAG = "poom_wifi_arp";

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_ARP_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_WIFI_ARP_TAG "poom_wifi_arp"

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif

typedef struct
{
    struct netif *netif;
    ip4_addr_t target;
    err_t out_err;
} poom_wifi_arp_tcpip_req_ctx_t;

/**
 * @brief Internal helper for `poom_wifi_arp_tcpip_etharp_request`.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_wifi_arp_tcpip_etharp_request_(void *arg)
{
    poom_wifi_arp_tcpip_req_ctx_t *ctx = (poom_wifi_arp_tcpip_req_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->netif == NULL))
    {
        return;
    }

    ctx->out_err = etharp_request(ctx->netif, &ctx->target);
}

typedef struct
{
    struct netif *netif;
    ip4_addr_t target;
    uint8_t out_mac[6];
    bool found;
} poom_wifi_arp_tcpip_find_ctx_t;

/**
 * @brief Internal helper for `poom_wifi_arp_tcpip_etharp_find`.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_wifi_arp_tcpip_etharp_find_(void *arg)
{
    poom_wifi_arp_tcpip_find_ctx_t *ctx = (poom_wifi_arp_tcpip_find_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->netif == NULL))
    {
        return;
    }

    struct eth_addr *eth_local = NULL;
    const ip4_addr_t *ip_local = NULL;
    s8_t idx = etharp_find_addr(ctx->netif, &ctx->target, &eth_local, &ip_local);
    if ((idx >= 0) && (eth_local != NULL))
    {
        memcpy(ctx->out_mac, eth_local->addr, sizeof(ctx->out_mac));
        ctx->found = true;
    }
}

static const uint16_t s_poom_wifi_arp_common_tcp_ports_[] = {
    21,   /* FTP */
    22,   /* SSH */
    23,   /* Telnet */
    53,   /* DNS (sometimes TCP) */
    80,   /* HTTP */
    443,  /* HTTPS */
    445,  /* SMB */
    554,  /* RTSP */
    1883, /* MQTT */
    8080, /* HTTP-alt */
    8443, /* HTTPS-alt */
};

static const uint16_t s_poom_wifi_arp_common_udp_ports_[] = {
    53,   /* DNS */
    67,   /* DHCP server */
    68,   /* DHCP client */
    69,   /* TFTP */
    123,  /* NTP */
    161,  /* SNMP */
    500,  /* IKE */
    1900, /* SSDP */
};

/**
 * @brief Internal helper for `poom_wifi_arp_write_u16_be`.
 *
 * @param[in] dst Parameter passed to the function.
 * @param[in] v Parameter passed to the function.
 * @return void
 */
static void poom_wifi_arp_write_u16_be_(uint8_t *dst, uint16_t v)
{
    if (dst == NULL)
    {
        return;
    }
    dst[0] = (uint8_t)((v >> 8) & 0xFFU);
    dst[1] = (uint8_t)(v & 0xFFU);
}

/**
 * @brief Internal helper for `poom_wifi_arp_get_sta_netif`.
 *
 * @return esp_netif_t *
 */
static esp_netif_t *poom_wifi_arp_get_sta_netif_(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL)
    {
        return netif;
    }

    static const char needle[] = "WIFI_STA";
    esp_netif_t *cur = NULL;
    while ((cur = esp_netif_next_unsafe(cur)) != NULL)
    {
        const char *key = esp_netif_get_ifkey(cur);
        if ((key != NULL) && (strstr(key, needle) != NULL))
        {
            return cur;
        }
    }

    return NULL;
}

/**
 * @brief Internal helper for `poom_wifi_arp_get_lwip_sta_netif`.
 *
 * @return struct netif *
 */
static struct netif *poom_wifi_arp_get_lwip_sta_netif_(void)
{
    struct netif *lwip_if = netif_default;
    esp_netif_t *sta = poom_wifi_arp_get_sta_netif_();
    if (sta == NULL)
    {
        return lwip_if;
    }

    char ifname[8] = {0};
    if (esp_netif_get_netif_impl_name(sta, ifname) == ESP_OK)
    {
        struct netif *found = netif_find(ifname);
        if (found != NULL)
        {
            lwip_if = found;
        }
    }

    return lwip_if;
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_send_arp_request`.
 *
 * @param[in] netif Parameter passed to the function.
 * @param[in] target_ip Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_scan_send_arp_request_(struct netif *netif, const char *target_ip)
{
    ip4_addr_t target_addr;

    if ((netif == NULL) || (target_ip == NULL))
    {
        return false;
    }

    if (!ip4addr_aton(target_ip, &target_addr))
    {
        return false;
    }

    poom_wifi_arp_tcpip_req_ctx_t ctx = {
        .netif = netif,
        .target = target_addr,
        .out_err = ERR_VAL,
    };

    if (tcpip_callback_wait(poom_wifi_arp_tcpip_etharp_request_, &ctx) != ERR_OK)
    {
        return false;
    }

    return (ctx.out_err == ERR_OK);
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_get_arp_entry`.
 *
 * @param[in] netif Parameter passed to the function.
 * @param[in] ip Parameter passed to the function.
 * @param[in] mac Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_scan_get_arp_entry_(struct netif *netif, const char *ip, uint8_t *mac)
{
    ip4_addr_t target_addr;

    if ((ip == NULL) || (mac == NULL))
    {
        return false;
    }

    if (!ip4addr_aton(ip, &target_addr))
    {
        return false;
    }

    if (netif == NULL)
    {
        return false;
    }

    poom_wifi_arp_tcpip_find_ctx_t ctx = {
        .netif = netif,
        .target = target_addr,
        .out_mac = {0},
        .found = false,
    };

    if (tcpip_callback_wait(poom_wifi_arp_tcpip_etharp_find_, &ctx) != ERR_OK)
    {
        return false;
    }

    if (!ctx.found)
    {
        return false;
    }

    memcpy(mac, ctx.out_mac, 6);
    return true;
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_get_subnet_prefix`.
 *
 * @param[in] subnet_prefix Parameter passed to the function.
 * @param[in] prefix_size Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_scan_get_subnet_prefix_(char *subnet_prefix, size_t prefix_size)
{
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;
    ip4_addr_t network_addr;
    char network_str[16];
    char *last_dot;
    uint32_t network;
    size_t len;

    if (subnet_prefix == NULL)
    {
        return false;
    }

    if (!poom_wifi_ctrl_sta_has_ip())
    {
        return false;
    }

    netif = poom_wifi_arp_get_sta_netif_();
    if (netif == NULL)
    {
        return false;
    }

    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK)
    {
        return false;
    }

    network = ip_info.ip.addr & ip_info.netmask.addr;
    network_addr.addr = network;

    if (ip4addr_ntoa_r(&network_addr, network_str, sizeof(network_str)) == NULL)
    {
        return false;
    }

    last_dot = strrchr(network_str, '.');
    if (last_dot == NULL)
    {
        return false;
    }

    len = (size_t)(last_dot - network_str + 1);
    if (len >= prefix_size)
    {
        return false;
    }

    memcpy(subnet_prefix, network_str, len);
    subnet_prefix[len] = '\0';
    return true;
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_build_ip_string`.
 *
 * @param[in] out Parameter passed to the function.
 * @param[in] out_size Parameter passed to the function.
 * @param[in] prefix Parameter passed to the function.
 * @param[in] host Parameter passed to the function.
 * @return void
 */
static void poom_wifi_arp_scan_build_ip_string_(char *out, size_t out_size, const char *prefix, int host)
{
    if ((out == NULL) || (out_size == 0U))
    {
        return;
    }

    if (prefix == NULL)
    {
        out[0] = '\0';
        return;
    }

    (void)snprintf(out, out_size, "%s%d", prefix, host);
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_add_host`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] ip Parameter passed to the function.
 * @param[in] mac Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_scan_add_host_(poom_wifi_arp_scan_ctx_t *ctx, const char *ip, const uint8_t *mac)
{
    if ((ctx == NULL) || (ctx->hosts == NULL) || (ip == NULL) || (mac == NULL))
    {
        return false;
    }

    if (ctx->num_active_hosts >= ctx->max_hosts)
    {
        return false;
    }

    poom_wifi_arp_scan_host_t *host = &ctx->hosts[ctx->num_active_hosts];
    (void)memset(host, 0, sizeof(*host));
    (void)snprintf(host->ip, sizeof(host->ip), "%s", ip);
    memcpy(host->mac, mac, 6);
    host->is_active = true;
    ctx->num_active_hosts++;
    return true;
}

/**
 * @brief Internal helper for `poom_wifi_arp_scan_process_batch`.
 *
 * @param[in] ctx Parameter passed to the function.
 * @param[in] netif Parameter passed to the function.
 * @param[in] batch_start Parameter passed to the function.
 * @param[in] batch_end Parameter passed to the function.
 * @return void
 */
static void poom_wifi_arp_scan_process_batch_(poom_wifi_arp_scan_ctx_t *ctx, struct netif *netif, int batch_start, int batch_end)
{
    char current_ip[24];

    if ((ctx == NULL) || (netif == NULL))
    {
        return;
    }

    for (int host = batch_start; host <= batch_end; host++)
    {
        poom_wifi_arp_scan_build_ip_string_(current_ip, sizeof(current_ip), ctx->subnet_prefix, host);
        (void)poom_wifi_arp_scan_send_arp_request_(netif, current_ip);
        vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_ARP_SCAN_REQ_DELAY_MS));
    }

    vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_ARP_SCAN_RESP_WAIT_MS));

    for (int host = batch_start; host <= batch_end; host++)
    {
        uint8_t mac[6];
        poom_wifi_arp_scan_build_ip_string_(current_ip, sizeof(current_ip), ctx->subnet_prefix, host);
        if (poom_wifi_arp_scan_get_arp_entry_(netif, current_ip, mac))
        {
            (void)poom_wifi_arp_scan_add_host_(ctx, current_ip, mac);
        }
    }
}

/**
 * @brief Internal helper for `poom_wifi_arp_tcp_connect_probe`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] port Parameter passed to the function.
 * @param[in] timeout_ms Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_tcp_connect_probe_(const char *target_ip, uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in server_addr;
    int sock = -1;
    int flags;
    int rc;
    bool is_open = false;

    if (target_ip == NULL)
    {
        return false;
    }

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1)
    {
        return false;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        return false;
    }

    flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    rc = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (rc == 0)
    {
        is_open = true;
        goto out;
    }

    if (rc < 0 && errno != EINPROGRESS)
    {
        goto out;
    }

    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000U),
        .tv_usec = (long)((timeout_ms % 1000U) * 1000U),
    };

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);

    rc = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (rc > 0 && FD_ISSET(sock, &wfds))
    {
        int so_error = 0;
        socklen_t slen = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) == 0 && so_error == 0)
        {
            is_open = true;
        }
    }

out:
    close(sock);
    return is_open;
}

static bool poom_wifi_arp_udp_response_probe_(const char *target_ip, uint16_t port, uint32_t timeout_ms);

esp_err_t poom_wifi_arp_probe_tcp_port(const char *target_ip, uint16_t port, bool *out_open)
{
    if ((target_ip == NULL) || (out_open == NULL) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_open = poom_wifi_arp_tcp_connect_probe_(target_ip, port, 250U);
    return ESP_OK;
}

esp_err_t poom_wifi_arp_probe_udp_port(const char *target_ip, uint16_t port, bool *out_responded)
{
    if ((target_ip == NULL) || (out_responded == NULL) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_responded = poom_wifi_arp_udp_response_probe_(target_ip, port, 120U);
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_arp_ssh_is_service`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] port Parameter passed to the function.
 * @param[in] connect_timeout_ms Parameter passed to the function.
 * @param[in] banner_timeout_ms Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_ssh_is_service_(const char *target_ip, uint16_t port, uint32_t connect_timeout_ms, uint32_t banner_timeout_ms)
{
    struct sockaddr_in server_addr;
    int sock = -1;
    int flags;
    int rc;
    bool is_open = false;

    if (target_ip == NULL)
    {
        return false;
    }

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1)
    {
        return false;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        return false;
    }

    flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    rc = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (rc == 0)
    {
        is_open = true;
    }
    else if (rc < 0 && errno == EINPROGRESS)
    {
        struct timeval tv = {
            .tv_sec = (long)(connect_timeout_ms / 1000U),
            .tv_usec = (long)((connect_timeout_ms % 1000U) * 1000U),
        };
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0 && FD_ISSET(sock, &wfds))
        {
            int so_error = 0;
            socklen_t slen = sizeof(so_error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) == 0 && so_error == 0)
            {
                is_open = true;
            }
        }
    }

    if (!is_open)
    {
        close(sock);
        return false;
    }

    (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval rcv_tv = {
        .tv_sec = (long)(banner_timeout_ms / 1000U),
        .tv_usec = (long)((banner_timeout_ms % 1000U) * 1000U),
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    char banner[16] = {0};
    const int n = (int)recv(sock, banner, (int)(sizeof(banner) - 1U), 0);
    close(sock);
    if (n <= 0)
    {
        return false;
    }

    banner[sizeof(banner) - 1U] = '\0';
    return (strncmp(banner, "SSH-", 4) == 0);
}

esp_err_t poom_wifi_arp_probe_ssh_port(const char *target_ip, uint16_t port, bool *out_is_ssh)
{
    if ((target_ip == NULL) || (out_is_ssh == NULL) || (port == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_is_ssh = poom_wifi_arp_ssh_is_service_(target_ip, port, 250U, 200U);
    return ESP_OK;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] port Parameter passed to the function.
 * @param[in] buf Parameter passed to the function.
 * @param[in] bufsize Parameter passed to the function.
 * @return size_t
 */
static size_t poom_wifi_arp_udp_make_payload_(uint16_t port, uint8_t *buf, size_t bufsize)
{
    if ((buf == NULL) || (bufsize == 0U))
    {
        return 0U;
    }

    if (port == 123U)
    {
        if (bufsize < 48U)
        {
            return 0U;
        }
        (void)memset(buf, 0, 48U);
        buf[0] = 0x1BU;
        return 48U;
    }

    if (port == 53U)
    {
        static const char name[] = "test.com";
        size_t p = 0U;

        if (bufsize < 32U)
        {
            return 0U;
        }

        poom_wifi_arp_write_u16_be_(&buf[p], (uint16_t)esp_random());
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 0x0100U);
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 1U);
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 0U);
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 0U);
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 0U);
        p += 2U;

        const char *cursor = name;
        while (*cursor != '\0')
        {
            const char *dot = strchr(cursor, '.');
            const size_t label_len = (dot != NULL) ? (size_t)(dot - cursor) : strlen(cursor);
            if ((p + 1U + label_len + 1U + 4U) >= bufsize)
            {
                return 0U;
            }
            buf[p++] = (uint8_t)label_len;
            memcpy(&buf[p], cursor, label_len);
            p += label_len;
            cursor = (dot != NULL) ? (dot + 1) : (cursor + label_len);
        }
        buf[p++] = 0U;

        poom_wifi_arp_write_u16_be_(&buf[p], 1U);
        p += 2U;
        poom_wifi_arp_write_u16_be_(&buf[p], 1U);
        p += 2U;
        return p;
    }

    buf[0] = 0U;
    return 1U;
}

/**
 * @brief Internal helper for `poom_wifi_arp_udp_response_probe`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] port Parameter passed to the function.
 * @param[in] timeout_ms Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_udp_response_probe_(const char *target_ip, uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in server_addr;
    int sock = -1;
    bool responding = false;

    if (target_ip == NULL)
    {
        return false;
    }

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1)
    {
        return false;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        return false;
    }

    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000U),
        .tv_usec = (long)((timeout_ms % 1000U) * 1000U),
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t probe[96];
    const size_t probe_len = poom_wifi_arp_udp_make_payload_(port, probe, sizeof(probe));
    if (probe_len == 0U)
    {
        close(sock);
        return false;
    }

    (void)sendto(sock, probe, probe_len, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

    uint8_t buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    if (n > 0)
    {
        responding = true;
    }

    close(sock);
    return responding;
}

/**
 * @brief Internal helper for `poom_wifi_arp_ssh_banner_probe`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] port Parameter passed to the function.
 * @param[in] out_banner Parameter passed to the function.
 * @param[in] banner_size Parameter passed to the function.
 * @return bool
 */
static bool poom_wifi_arp_ssh_banner_probe_(const char *target_ip, uint16_t port, char *out_banner, size_t banner_size)
{
    struct sockaddr_in server_addr;
    int sock = -1;
    int flags;
    int rc;
    bool open = false;

    if ((target_ip == NULL) || (out_banner == NULL) || (banner_size == 0U))
    {
        return false;
    }

    out_banner[0] = '\0';

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1)
    {
        return false;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        return false;
    }

    flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    rc = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (rc == 0)
    {
        open = true;
    }
    else if (rc < 0 && errno == EINPROGRESS)
    {
        struct timeval tv = {
            .tv_sec = 2,
            .tv_usec = 0,
        };

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0 && FD_ISSET(sock, &wfds))
        {
            int so_error = 0;
            socklen_t slen = sizeof(so_error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) == 0 && so_error == 0)
            {
                open = true;
            }
        }
    }

    if (!open)
    {
        close(sock);
        return false;
    }

    (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = {
        .tv_sec = 2,
        .tv_usec = 0,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char banner[96] = {0};
    const int n = (int)recv(sock, banner, (int)(sizeof(banner) - 1U), 0);
    if (n > 0)
    {
        banner[n] = '\0';
        char *end = strpbrk(banner, "\r\n");
        if (end != NULL)
        {
            *end = '\0';
        }
        (void)snprintf(out_banner, banner_size, "%.63s", banner);
    }

    close(sock);
    return true;
}

/**
 * @brief Internal helper for `poom_wifi_arp_probe_tcp_ports`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_arp_probe_tcp_ports_(const char *target_ip, poom_wifi_arp_port_probe_result_t *out)
{
    if ((target_ip == NULL) || (out == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    out->num_open_ports = 0U;
    (void)memset(out->open_ports, 0, sizeof(out->open_ports));

    for (size_t i = 0; i < (sizeof(s_poom_wifi_arp_common_tcp_ports_) / sizeof(s_poom_wifi_arp_common_tcp_ports_[0])); i++)
    {
        if (out->num_open_ports >= POOM_WIFI_ARP_PROBE_MAX_PORTS)
        {
            break;
        }

        const uint16_t port = s_poom_wifi_arp_common_tcp_ports_[i];
        if (poom_wifi_arp_tcp_connect_probe_(target_ip, port, POOM_WIFI_ARP_TCP_PROBE_TIMEOUT_MS))
        {
            out->open_ports[out->num_open_ports++] = port;
        }

        vTaskDelay(pdMS_TO_TICKS(POOM_WIFI_ARP_TCP_PROBE_INTER_DELAY_MS));
    }

    printf("[BeastScan] host=%s tcp_open=%u\n", target_ip, (unsigned)out->num_open_ports);
    if (out->num_open_ports > 0U)
    {
        for (size_t j = 0; j < out->num_open_ports; j++)
        {
            printf("[BeastScan] %s tcp=%u OPEN\n", target_ip, (unsigned)out->open_ports[j]);
        }
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_wifi_arp_probe_udp_ports`.
 *
 * @param[in] target_ip Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_arp_probe_udp_ports_(const char *target_ip, poom_wifi_arp_port_probe_result_t *out)
{
    if ((target_ip == NULL) || (out == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    out->num_open_ports = 0U;
    (void)memset(out->open_ports, 0, sizeof(out->open_ports));

    for (size_t i = 0; i < (sizeof(s_poom_wifi_arp_common_udp_ports_) / sizeof(s_poom_wifi_arp_common_udp_ports_[0])); i++)
    {
        if (out->num_open_ports >= POOM_WIFI_ARP_PROBE_MAX_PORTS)
        {
            break;
        }

        const uint16_t port = s_poom_wifi_arp_common_udp_ports_[i];
        if (poom_wifi_arp_udp_response_probe_(target_ip, port, POOM_WIFI_ARP_UDP_PROBE_TIMEOUT_MS))
        {
            out->open_ports[out->num_open_ports++] = port;
        }

        vTaskDelay(pdMS_TO_TICKS(4));
    }

    printf("[BeastScan] host=%s udp_resp=%u\n", target_ip, (unsigned)out->num_open_ports);
    if (out->num_open_ports > 0U)
    {
        for (size_t j = 0; j < out->num_open_ports; j++)
        {
            printf("[BeastScan] %s udp=%u RESP\n", target_ip, (unsigned)out->open_ports[j]);
        }
    }

    return ESP_OK;
}

esp_err_t poom_wifi_arp_probe_host_services(const char *target_ip, poom_wifi_arp_service_probe_result_t *out)
{
    if ((target_ip == NULL) || (out == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)memset(out, 0, sizeof(*out));
    out->ssh_found = false;
    out->ssh_port = 0U;
    (void)memset(out->ssh_banner, 0, sizeof(out->ssh_banner));

    (void)poom_wifi_arp_probe_tcp_ports_(target_ip, &out->tcp);
    (void)poom_wifi_arp_probe_udp_ports_(target_ip, &out->udp);

    const uint16_t ssh_ports[] = {22U, 2222U, 2022U};
    for (size_t i = 0; i < (sizeof(ssh_ports) / sizeof(ssh_ports[0])); i++)
    {
        char banner[64] = {0};
        if (poom_wifi_arp_ssh_banner_probe_(target_ip, ssh_ports[i], banner, sizeof(banner)))
        {
            out->ssh_found = true;
            out->ssh_port = ssh_ports[i];
            (void)snprintf(out->ssh_banner, sizeof(out->ssh_banner), "%.63s", banner);
            break;
        }
    }

    if (out->ssh_found)
    {
        printf("[BeastScan] host=%s ssh=%u banner=%.63s\n",
               target_ip,
               (unsigned)out->ssh_port,
               (out->ssh_banner[0] != '\0') ? out->ssh_banner : "<none>");
    }

    return ESP_OK;
}

esp_err_t poom_wifi_arp_scan_subnet(poom_wifi_arp_scan_ctx_t *ctx)
{
    if ((ctx == NULL) || (ctx->hosts == NULL) || (ctx->max_hosts == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!poom_wifi_ctrl_sta_has_ip())
    {
        POOM_PRINTF_W("STA has no IP; connect first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!poom_wifi_arp_scan_get_subnet_prefix_(ctx->subnet_prefix, sizeof(ctx->subnet_prefix)))
    {
        POOM_PRINTF_W("Failed to resolve subnet prefix");
        return ESP_FAIL;
    }

    ctx->num_active_hosts = 0U;
    struct netif *netif = poom_wifi_arp_get_lwip_sta_netif_();
    if (netif == NULL)
    {
        POOM_PRINTF_W("lwIP netif not available");
        return ESP_ERR_INVALID_STATE;
    }

    for (int batch_start = POOM_WIFI_ARP_SCAN_START_HOST;
         batch_start <= POOM_WIFI_ARP_SCAN_END_HOST;
         batch_start += POOM_WIFI_ARP_SCAN_BATCH_SIZE)
    {
        int batch_end = batch_start + POOM_WIFI_ARP_SCAN_BATCH_SIZE - 1;
        if (batch_end > POOM_WIFI_ARP_SCAN_END_HOST)
        {
            batch_end = POOM_WIFI_ARP_SCAN_END_HOST;
        }

        poom_wifi_arp_scan_process_batch_(ctx, netif, batch_start, batch_end);
        if (ctx->num_active_hosts >= ctx->max_hosts)
        {
            break;
        }
    }

    printf("[BeastScan] subnet=%s hosts=%u\n",
           ctx->subnet_prefix,
           (unsigned)ctx->num_active_hosts);
    for (size_t i = 0; i < ctx->num_active_hosts; i++)
    {
        const uint8_t *m = ctx->hosts[i].mac;
        printf("[BeastScan] host=%s mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
               ctx->hosts[i].ip,
               (unsigned)m[0],
               (unsigned)m[1],
               (unsigned)m[2],
               (unsigned)m[3],
               (unsigned)m[4],
               (unsigned)m[5]);
    }

    return ESP_OK;
}
