// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Networked backend for the POOM network scanner: live-subnet read, ICMP+ARP
// host discovery, and the TCP Connect port scanner. Pure logic (subnet math,
// port sets, service table) lives in poom_net_scanner_logic.c; this file is the
// part that needs lwIP / esp_netif / esp_ping and therefore only builds under
// ESP-IDF.
//
// lwIP usage mirrors the proven patterns in applications/poom_wifi_arp: ARP via
// etharp_request + etharp_find_addr marshalled onto the tcpip thread with
// tcpip_callback_wait, and non-blocking connect()+select() for TCP probes.

#include "poom_net_scanner.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_netif.h"

#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "ping/ping_sock.h"

#include "poom_wifi_ctrl.h"

/* ---- tunables ----------------------------------------------------------- */

#define NET_SCANNER_ARP_BATCH        (10)   /* ARP requests per send burst.   */
#define NET_SCANNER_ARP_REQ_GAP_MS   (10)   /* spacing between ARP requests.  */
#define NET_SCANNER_ARP_SETTLE_MS    (350)  /* wait before reading the table. */
#define NET_SCANNER_ICMP_MAX_HOSTS   (256)  /* cap on the ICMP pass.          */
#define NET_SCANNER_ICMP_DEFAULT_MS  (200)  /* per-host ICMP timeout.         */
#define NET_SCANNER_CONNECT_DEFAULT_MS (600)/* per-port connect timeout.      */

/* =========================================================================
 * netif helpers (STA interface -> esp_netif + lwIP struct netif)
 * ========================================================================= */

static esp_netif_t *net_scanner_sta_esp_netif_(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != NULL)
    {
        return sta;
    }
    /* Fallback: scan handles for a key that looks like a STA netif. */
    esp_netif_t *cur = esp_netif_next_unsafe(NULL);
    while (cur != NULL)
    {
        const char *key = esp_netif_get_ifkey(cur);
        if ((key != NULL) && (strstr(key, "STA") != NULL))
        {
            return cur;
        }
        cur = esp_netif_next_unsafe(cur);
    }
    return NULL;
}

static struct netif *net_scanner_sta_lwip_netif_(void)
{
    struct netif *lwip_if = netif_default;
    esp_netif_t *sta = net_scanner_sta_esp_netif_();
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

/* =========================================================================
 * Subnet (live)
 * ========================================================================= */

esp_err_t poom_net_scanner_subnet_current(poom_net_scanner_subnet_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!poom_wifi_ctrl_sta_has_ip())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_t *sta = net_scanner_sta_esp_netif_();
    if (sta == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t info;
    esp_err_t err = esp_netif_get_ip_info(sta, &info);
    if (err != ESP_OK)
    {
        return err;
    }

    /* esp_ip4_addr_t.addr is network byte order; our math is host order. */
    uint32_t ip_host   = lwip_ntohl(info.ip.addr);
    uint32_t mask_host  = lwip_ntohl(info.netmask.addr);
    uint32_t gw_host   = lwip_ntohl(info.gw.addr);

    if (mask_host == 0U)
    {
        return ESP_ERR_INVALID_STATE; /* no DHCP mask yet */
    }
    return poom_net_scanner_subnet_compute(ip_host, mask_host, gw_host, out);
}

/* =========================================================================
 * ARP (marshalled onto the tcpip thread, like poom_wifi_arp)
 * ========================================================================= */

typedef struct
{
    struct netif *netif;
    ip4_addr_t    target;
    err_t         out_err;
} net_scanner_arp_req_ctx_t;

static void net_scanner_arp_request_cb_(void *arg)
{
    net_scanner_arp_req_ctx_t *ctx = (net_scanner_arp_req_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->netif == NULL))
    {
        return;
    }
    ctx->out_err = etharp_request(ctx->netif, &ctx->target);
}

typedef struct
{
    struct netif *netif;
    ip4_addr_t    target;
    uint8_t       out_mac[6];
    bool          found;
} net_scanner_arp_find_ctx_t;

static void net_scanner_arp_find_cb_(void *arg)
{
    net_scanner_arp_find_ctx_t *ctx = (net_scanner_arp_find_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->netif == NULL))
    {
        return;
    }
    struct eth_addr *eth = NULL;
    const ip4_addr_t *ip = NULL;
    s8_t idx = etharp_find_addr(ctx->netif, &ctx->target, &eth, &ip);
    if ((idx >= 0) && (eth != NULL))
    {
        memcpy(ctx->out_mac, eth->addr, sizeof(ctx->out_mac));
        ctx->found = true;
    }
}

static bool net_scanner_arp_send_(struct netif *netif, uint32_t ip_host)
{
    if (netif == NULL)
    {
        return false;
    }
    net_scanner_arp_req_ctx_t ctx = {.netif = netif, .out_err = ERR_VAL};
    ip4_addr_set_u32(&ctx.target, lwip_htonl(ip_host));
    if (tcpip_callback_wait(net_scanner_arp_request_cb_, &ctx) != ERR_OK)
    {
        return false;
    }
    return (ctx.out_err == ERR_OK);
}

static bool net_scanner_arp_lookup_(struct netif *netif, uint32_t ip_host, uint8_t mac[6])
{
    if (netif == NULL)
    {
        return false;
    }
    net_scanner_arp_find_ctx_t ctx = {.netif = netif, .found = false};
    ip4_addr_set_u32(&ctx.target, lwip_htonl(ip_host));
    if (tcpip_callback_wait(net_scanner_arp_find_cb_, &ctx) != ERR_OK)
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

/* =========================================================================
 * ICMP (esp_ping, one target at a time, synchronous)
 * ========================================================================= */

typedef struct
{
    SemaphoreHandle_t done;
    volatile bool     replied;
    volatile uint8_t  ttl;
} net_scanner_ping_ctx_t;

static void net_scanner_ping_success_(esp_ping_handle_t hdl, void *args)
{
    net_scanner_ping_ctx_t *c = (net_scanner_ping_ctx_t *)args;
    if (c != NULL)
    {
        c->replied = true;
        uint8_t ttl = 0;
        if (esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl)) == ESP_OK)
        {
            c->ttl = ttl;
        }
    }
}

static void net_scanner_ping_end_(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    net_scanner_ping_ctx_t *c = (net_scanner_ping_ctx_t *)args;
    if (c != NULL && c->done != NULL)
    {
        (void)xSemaphoreGive(c->done);
    }
}

/* Returns true if the host replied to a single ICMP echo within timeout.
 * On a reply, *out_ttl (when non-NULL) gets the reply's IP TTL. */
static bool net_scanner_icmp_ping_(uint32_t ip_host, uint32_t timeout_ms,
                                   uint8_t *out_ttl)
{
    net_scanner_ping_ctx_t ctx = {0};
    ctx.done = xSemaphoreCreateBinary();
    if (ctx.done == NULL)
    {
        return false;
    }

    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    /* by-value variant: the pointer form expands to `if(&target){...}`, which
     * trips -Werror=address since the address of a local is never NULL. */
    ip_addr_set_ip4_u32_val(target, lwip_htonl(ip_host));

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = 1;
    cfg.timeout_ms  = (timeout_ms != 0U) ? timeout_ms : NET_SCANNER_ICMP_DEFAULT_MS;
    cfg.interval_ms = cfg.timeout_ms;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = net_scanner_ping_success_,
        .on_ping_timeout = NULL,
        .on_ping_end     = net_scanner_ping_end_,
        .cb_args         = &ctx,
    };

    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK || h == NULL)
    {
        vSemaphoreDelete(ctx.done);
        return false;
    }

    (void)esp_ping_start(h);
    /* The pinned SDK deletes ping sessions asynchronously. Stopping/deleting
     * does not join the ping task: after a timeout it may still call us with
     * &ctx. Keep the stack context and semaphore alive until on_ping_end. */
    TickType_t wait = pdMS_TO_TICKS(cfg.timeout_ms + 300U);
    if (xSemaphoreTake(ctx.done, wait) != pdTRUE)
    {
        (void)esp_ping_stop(h);
        (void)xSemaphoreTake(ctx.done, portMAX_DELAY);
    }
    (void)esp_ping_stop(h);
    (void)esp_ping_delete_session(h);
    bool replied = ctx.replied;
    uint8_t ttl = ctx.ttl;
    vSemaphoreDelete(ctx.done);
    if (replied && out_ttl != NULL) { *out_ttl = ttl; }
    return replied;
}

/* =========================================================================
 * Discovery
 * ========================================================================= */

static size_t net_scanner_find_or_add_(poom_net_scanner_host_t *arr, size_t *count,
                                        size_t cap, uint32_t ip_host, bool *is_new)
{
    for (size_t i = 0; i < *count; ++i)
    {
        if (arr[i].ip == ip_host)
        {
            if (is_new) { *is_new = false; }
            return i;
        }
    }
    if (*count >= cap)
    {
        if (is_new) { *is_new = false; }
        return (size_t)-1;
    }
    size_t i = (*count)++;
    memset(&arr[i], 0, sizeof(arr[i]));
    arr[i].ip = ip_host;
    poom_net_scanner_ip_to_str(ip_host, arr[i].ip_str, sizeof(arr[i].ip_str));
    if (is_new) { *is_new = true; }
    return i;
}

esp_err_t poom_net_scanner_discover(const poom_net_scanner_subnet_t *subnet,
                                    const poom_net_scanner_discovery_opts_t *opts,
                                    poom_net_scanner_host_t *out, size_t cap,
                                    size_t *out_count,
                                    const volatile bool *cancel,
                                    poom_net_scanner_progress_cb_t cb, void *user)
{
    if (subnet == NULL || out == NULL || out_count == NULL || cap == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    poom_net_scanner_discovery_opts_t o = {
        .use_icmp = true, .use_arp = true,
        .icmp_timeout_ms = NET_SCANNER_ICMP_DEFAULT_MS,
        .arp_wait_ms = NET_SCANNER_ARP_SETTLE_MS,
    };
    if (opts != NULL)
    {
        o = *opts;
        if (o.icmp_timeout_ms == 0U) { o.icmp_timeout_ms = NET_SCANNER_ICMP_DEFAULT_MS; }
        if (o.arp_wait_ms == 0U)     { o.arp_wait_ms = NET_SCANNER_ARP_SETTLE_MS; }
    }
    if (!o.use_icmp && !o.use_arp)
    {
        o.use_arp = true; /* must do at least one method */
    }

    struct netif *netif = net_scanner_sta_lwip_netif_();
    if (netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t first = subnet->first_host;
    const uint32_t last  = subnet->last_host;
    const uint32_t total = subnet->host_count;
    uint32_t processed = 0;

    if (total == 0U)
    {
        return ESP_OK;
    }

    /* ---- ARP pass: batched request bursts, then harvest the table. ---- */
    if (o.use_arp)
    {
        for (uint32_t base = first; base <= last; base += NET_SCANNER_ARP_BATCH)
        {
            if (cancel != NULL && *cancel)
            {
                return ESP_OK;
            }
            uint32_t end = base + NET_SCANNER_ARP_BATCH - 1U;
            if (end > last) { end = last; }

            for (uint32_t a = base; a <= end; ++a)
            {
                (void)net_scanner_arp_send_(netif, a);
                vTaskDelay(pdMS_TO_TICKS(NET_SCANNER_ARP_REQ_GAP_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(o.arp_wait_ms));

            for (uint32_t a = base; a <= end; ++a)
            {
                uint8_t mac[6];
                if (net_scanner_arp_lookup_(netif, a, mac))
                {
                    bool is_new = false;
                    size_t i = net_scanner_find_or_add_(out, out_count, cap, a, &is_new);
                    if (i != (size_t)-1)
                    {
                        out[i].arp_responded = true;
                        out[i].mac_known = true;
                        memcpy(out[i].mac, mac, 6);
                    }
                }
                processed++;
                if (cb != NULL)
                {
                    /* ARP pass counts as the first half of progress when ICMP
                     * also runs; otherwise it is the whole bar. */
                    uint32_t denom = o.use_icmp ? (total * 2U) : total;
                    cb(processed, denom, user);
                }
            }
        }
    }

    /* ---- ICMP pass: bounded, confirms liveness + catches ARP misses. ---- */
    if (o.use_icmp)
    {
        uint32_t icmp_done = 0;
        for (uint32_t a = first; a <= last; ++a)
        {
            if (cancel != NULL && *cancel)
            {
                return ESP_OK;
            }
            if (icmp_done >= NET_SCANNER_ICMP_MAX_HOSTS)
            {
                break; /* keep the ICMP pass bounded on large subnets */
            }
            icmp_done++;

            uint8_t ttl = 0;
            if (net_scanner_icmp_ping_(a, o.icmp_timeout_ms, &ttl))
            {
                bool is_new = false;
                size_t i = net_scanner_find_or_add_(out, out_count, cap, a, &is_new);
                if (i != (size_t)-1)
                {
                    out[i].icmp_responded = true;
                    if (ttl > 0) { out[i].ttl = ttl; out[i].ttl_known = true; }
                    /* A reply means the stack just ARP-resolved it; grab MAC. */
                    if (!out[i].mac_known)
                    {
                        uint8_t mac[6];
                        if (net_scanner_arp_lookup_(netif, a, mac))
                        {
                            out[i].mac_known = true;
                            out[i].arp_responded = true;
                            memcpy(out[i].mac, mac, 6);
                        }
                    }
                }
            }

            if (cb != NULL)
            {
                uint32_t denom = o.use_arp ? (total * 2U) : total;
                uint32_t base_done = o.use_arp ? total : 0U;
                cb(base_done + icmp_done, denom, user);
            }
        }
    }

    return ESP_OK;
}

esp_err_t poom_net_scanner_resolve_mac(const char *ip_str, uint8_t mac[6])
{
    if (ip_str == NULL || mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ip4_addr_t a;
    if (!ip4addr_aton(ip_str, &a))
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t ip_host = lwip_ntohl(ip4_addr_get_u32(&a));

    struct netif *netif = net_scanner_sta_lwip_netif_();
    if (netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Prompt resolution, then read the table. */
    if (net_scanner_arp_lookup_(netif, ip_host, mac))
    {
        return ESP_OK;
    }
    (void)net_scanner_arp_send_(netif, ip_host);
    vTaskDelay(pdMS_TO_TICKS(NET_SCANNER_ARP_SETTLE_MS));
    if (net_scanner_arp_lookup_(netif, ip_host, mac))
    {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* =========================================================================
 * TCP Connect port scan (non-blocking connect + select, fanned out)
 * ========================================================================= */

typedef struct
{
    int      sock;
    uint16_t port;
    bool     active;
} net_scanner_conn_t;

/* Start one non-blocking connect. Returns the socket (>=0) or -1. */
static int net_scanner_start_connect_(uint32_t ip_host, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        return -1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(port);
    sa.sin_addr.s_addr = lwip_htonl(ip_host);

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0)
    {
        return sock; /* immediate connect (rare) -> resolves open on first poll */
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
        /* Immediate refusal/error: port is closed/unreachable. Caller detects
         * this by polling, so keep the socket to report via SO_ERROR. */
        return sock;
    }
    return sock;
}

/* Close a probe socket. Where lwIP was built with SO_LINGER, use linger 0 so
 * the connection is reset instead of parking a TIME_WAIT pcb on the heap for
 * ~2 minutes (dozens per scan fragment the heap Wi-Fi/BLE later need).
 * Without SO_LINGER support this is a plain close(). */
static void net_scanner_close_probe_(int sock)
{
#if defined(LWIP_SO_LINGER) && LWIP_SO_LINGER
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    (void)setsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
#endif
    (void)close(sock);
}

/* Classify a resolved socket into a port state. */
static poom_net_scanner_port_state_t net_scanner_classify_(int sock)
{
    int so_error = 0;
    socklen_t slen = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) != 0)
    {
        return POOM_NET_SCANNER_PORT_FILTERED;
    }
    if (so_error == 0)
    {
        return POOM_NET_SCANNER_PORT_OPEN;
    }
    if (so_error == ECONNREFUSED)
    {
        return POOM_NET_SCANNER_PORT_CLOSED;
    }
    /* EHOSTUNREACH / ENETUNREACH / etc: treat as filtered/unreachable. */
    return POOM_NET_SCANNER_PORT_FILTERED;
}

static void net_scanner_record_(poom_net_scanner_port_result_t *out, size_t cap,
                                size_t *n, const poom_net_scanner_scan_opts_t *o,
                                uint16_t port, poom_net_scanner_port_state_t state)
{
    if (state == POOM_NET_SCANNER_PORT_CLOSED && o->n_closed != NULL) { (*o->n_closed)++; }
    if (state == POOM_NET_SCANNER_PORT_FILTERED && o->n_filtered != NULL) { (*o->n_filtered)++; }
    if (o->open_only && state != POOM_NET_SCANNER_PORT_OPEN)
    {
        return;
    }
    if (*n >= cap)
    {
        return;
    }
    poom_net_scanner_port_result_t *r = &out[(*n)++];
    r->port = port;
    r->state = state;
    r->method = o->method;
    r->service = poom_net_scanner_service_name(port);
    r->service_is_guess = true; /* table guess, not a verified banner */
}

esp_err_t poom_net_scanner_port_scan(const char *ip_str,
                                     const uint16_t *ports, size_t n_ports,
                                     const poom_net_scanner_scan_opts_t *opts,
                                     poom_net_scanner_port_result_t *out,
                                     size_t cap, size_t *out_count,
                                     const volatile bool *cancel,
                                     poom_net_scanner_progress_cb_t cb,
                                     void *user)
{
    if (ip_str == NULL || ports == NULL || out == NULL || out_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    poom_net_scanner_scan_opts_t o = {
        .method = POOM_NET_SCANNER_METHOD_TCP_CONNECT,
        .connect_timeout_ms = NET_SCANNER_CONNECT_DEFAULT_MS,
        .fanout = POOM_NET_SCANNER_DEFAULT_FANOUT,
        .open_only = false,
    };
    if (opts != NULL)
    {
        o = *opts;
        if (o.connect_timeout_ms == 0U) { o.connect_timeout_ms = NET_SCANNER_CONNECT_DEFAULT_MS; }
        if (o.fanout == 0U)             { o.fanout = POOM_NET_SCANNER_DEFAULT_FANOUT; }
    }
    if (o.fanout > POOM_NET_SCANNER_MAX_FANOUT)
    {
        o.fanout = POOM_NET_SCANNER_MAX_FANOUT;
    }

    /* SYN is feasibility-gated: never silently fall back to Connect. */
    if (o.method == POOM_NET_SCANNER_METHOD_TCP_SYN && !poom_net_scanner_syn_available())
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t ip_host;
    {
        ip4_addr_t a;
        if (!ip4addr_aton(ip_str, &a))
        {
            return ESP_ERR_INVALID_ARG;
        }
        ip_host = lwip_ntohl(ip4_addr_get_u32(&a));
    }

    net_scanner_conn_t *conns = (net_scanner_conn_t *)calloc(o.fanout, sizeof(*conns));
    if (conns == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t idx = 0;
    while (idx < n_ports)
    {
        if (cancel != NULL && *cancel)
        {
            break;
        }

        /* Open a batch of non-blocking connects. */
        size_t batch = 0;
        while (batch < o.fanout && idx < n_ports)
        {
            int sock = net_scanner_start_connect_(ip_host, ports[idx]);
            if (sock < 0)
            {
                /* Out of sockets: stop growing the batch; if nothing is in
                 * flight yet, back off briefly and retry this port. */
                if (batch == 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                break;
            }
            conns[batch].sock = sock;
            conns[batch].port = ports[idx];
            conns[batch].active = true;
            batch++;
            idx++;
        }
        if (batch == 0)
        {
            continue;
        }

        /* Poll the batch until all resolve or the timeout elapses. */
        TickType_t start = xTaskGetTickCount();
        TickType_t limit = pdMS_TO_TICKS(o.connect_timeout_ms);
        size_t remaining = batch;

        while (remaining > 0)
        {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            int maxfd = -1;
            for (size_t i = 0; i < batch; ++i)
            {
                if (conns[i].active)
                {
                    FD_SET(conns[i].sock, &wfds);
                    FD_SET(conns[i].sock, &efds);
                    if (conns[i].sock > maxfd) { maxfd = conns[i].sock; }
                }
            }
            if (maxfd < 0)
            {
                break;
            }

            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= limit)
            {
                break; /* leftover sockets are filtered */
            }
            TickType_t left = limit - elapsed;
            struct timeval tv;
            tv.tv_sec = left / configTICK_RATE_HZ;
            tv.tv_usec = (left % configTICK_RATE_HZ) * (1000000 / configTICK_RATE_HZ);

            int rc = select(maxfd + 1, NULL, &wfds, &efds, &tv);
            if (rc < 0)
            {
                /* Every still-active port in this batch is then recorded as
                 * filtered - surface it (logs are compiled out in this build). */
                printf("[net_scanner] select() failed: errno %d (maxfd %d)\n", errno, maxfd);
                break;
            }
            if (rc == 0)
            {
                break; /* timeout: remaining are filtered */
            }
            for (size_t i = 0; i < batch; ++i)
            {
                if (conns[i].active &&
                    (FD_ISSET(conns[i].sock, &wfds) || FD_ISSET(conns[i].sock, &efds)))
                {
                    poom_net_scanner_port_state_t st = net_scanner_classify_(conns[i].sock);
                    net_scanner_record_(out, cap, out_count, &o,
                                        conns[i].port, st);
                    net_scanner_close_probe_(conns[i].sock);
                    conns[i].active = false;
                    conns[i].sock = -1;
                    remaining--;
                }
            }
        }

        /* Anything still active timed out -> filtered. */
        for (size_t i = 0; i < batch; ++i)
        {
            if (conns[i].active)
            {
                net_scanner_record_(out, cap, out_count, &o,
                                    conns[i].port, POOM_NET_SCANNER_PORT_FILTERED);
                net_scanner_close_probe_(conns[i].sock);
                conns[i].active = false;
                conns[i].sock = -1;
            }
        }

        if (cb != NULL)
        {
            cb((uint32_t)idx, (uint32_t)n_ports, user);
        }
    }

    free(conns);
    return ESP_OK;
}

/* =========================================================================
 * Service / version detection (banner grab + HTTP probe)
 * ========================================================================= */

/* Connect and return a BLOCKING socket (with a recv timeout set), or -1. */
static int net_scanner_connect_blocking_(uint32_t ip_host, uint16_t port,
                                         uint32_t timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return -1; }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) { (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK); }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(port);
    sa.sin_addr.s_addr = lwip_htonl(ip_host);

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
        (void)close(sock);
        return -1;
    }
    if (rc != 0)
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0)
        {
            (void)close(sock);
            return -1;
        }
        int so_error = 0;
        socklen_t slen = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) != 0 || so_error != 0)
        {
            (void)close(sock);
            return -1;
        }
    }

    /* Back to blocking, with a bounded recv timeout. */
    if (flags >= 0) { (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK); }
    struct timeval rtv;
    rtv.tv_sec = timeout_ms / 1000;
    rtv.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    return sock;
}

esp_err_t poom_net_scanner_detect_service(const char *ip_str, uint16_t port,
                                          uint32_t timeout_ms,
                                          poom_net_scanner_service_t *out,
                                          const volatile bool *cancel)
{
    if (ip_str == NULL || out == NULL) { return ESP_ERR_INVALID_ARG; }

    /* Default result = port-number guess; upgraded only on real evidence. */
    poom_net_scanner_parse_banner(NULL, 0, port, out);

    if (timeout_ms == 0U) { timeout_ms = 3000U; }

    uint32_t ip_host;
    {
        ip4_addr_t a;
        if (!ip4addr_aton(ip_str, &a)) { return ESP_ERR_INVALID_ARG; }
        ip_host = lwip_ntohl(ip4_addr_get_u32(&a));
    }

    int sock = net_scanner_connect_blocking_(ip_host, port, 1500U);
    if (sock < 0) { return ESP_OK; } /* keep port-guess (open port may have closed) */

    char buf[768];

    /* 1) Passive banner: many services greet first (SSH/FTP/SMTP/POP3/IMAP). */
    {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 900 * 1000};
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            poom_net_scanner_parse_banner(buf, (size_t)n, port, out);
            if (out->evidence >= POOM_NET_SCANNER_EV_PROTO_RESPONSE)
            {
                net_scanner_close_probe_(sock);
                return ESP_OK; /* recognised from the greeting */
            }
        }
    }

    if (cancel != NULL && *cancel) { net_scanner_close_probe_(sock); return ESP_OK; }

    /* 2) HTTP probe: send a minimal request and parse the response head. */
    {
        char req[160];
        int rl = snprintf(req, sizeof(req),
                          "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: POOM\r\n"
                          "Connection: close\r\n\r\n", ip_str);
        if (rl > 0) { (void)send(sock, req, (size_t)rl, 0); }

        struct timeval tv = {.tv_sec = 1, .tv_usec = 500 * 1000};
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* Headers often arrive in one segment and the page in the next; read
         * up to two segments so a <title> just after the head is seen. */
        int m = 0;
        for (int part = 0; part < 2 && m < (int)sizeof(buf) - 1; ++part)
        {
            int r = recv(sock, buf + m, sizeof(buf) - 1 - (size_t)m, 0);
            if (r <= 0) { break; }
            m += r;
        }
        if (m > 0)
        {
            buf[m] = '\0';
            poom_net_scanner_service_t http;
            memset(&http, 0, sizeof(http));
            if (poom_net_scanner_parse_http(buf, (size_t)m, &http))
            {
                http.port = port;
                *out = http; /* HTTP evidence supersedes the port guess */
            }
        }
    }

    net_scanner_close_probe_(sock);
    return ESP_OK;
}

/* =========================================================================
 * Identification pass (names + OS clues), read-only discovery traffic.
 * Packet build/parse and the OS fold are pure (poom_net_scanner_ident.c).
 * ========================================================================= */

#define NET_SCANNER_ID_BUF          (600)   /* one receive buffer, heap, freed */
#define NET_SCANNER_ID_ICMP_WAIT_MS (800)   /* collect ping replies            */
#define NET_SCANNER_ID_UDP_WAIT_MS  (1800)  /* collect SSDP/NBNS/mDNS replies  */
#define NET_SCANNER_ID_MAX_LOC      (12)    /* UPnP descriptions fetched       */
#define NET_SCANNER_ID_XML_BUF      (2048)  /* description bytes read (heap)    */

typedef struct
{
    uint8_t  host;
    uint16_t port;
    char     path[48];
} net_scanner_loc_t;

static int net_scanner_host_idx_(const poom_net_scanner_host_t *hosts, size_t n, uint32_t ip)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (hosts[i].ip == ip) { return (int)i; }
    }
    return -1;
}

static void net_scanner_sockaddr_(struct sockaddr_in *sa, uint32_t ip_host, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = lwip_htons(port);
    sa->sin_addr.s_addr = lwip_htonl(ip_host);
}

/* Wait up to ms_left for @p s to become readable. */
static bool net_scanner_wait_readable_(int s, TickType_t start, TickType_t limit)
{
    TickType_t el = xTaskGetTickCount() - start;
    if (el >= limit) { return false; }
    TickType_t left = limit - el;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timeval tv;
    tv.tv_sec = left / configTICK_RATE_HZ;
    tv.tv_usec = (left % configTICK_RATE_HZ) * (1000000 / configTICK_RATE_HZ);
    return select(s + 1, &rfds, NULL, NULL, &tv) > 0;
}

static void net_scanner_id_icmp_(const poom_net_scanner_host_t *hosts, size_t n,
                                 poom_net_scanner_ident_t *out, uint8_t *buf,
                                 const volatile bool *cancel)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0)
    {
        printf("[net_scanner] identify: raw ICMP socket unavailable (errno %d)\n", errno);
        return;
    }
    uint16_t ident = (uint16_t)(0x504FU ^ (uint16_t)xTaskGetTickCount());
    for (size_t i = 0; i < n && !(cancel != NULL && *cancel); ++i)
    {
        uint8_t pkt[32];
        size_t len = poom_ns_icmp_echo_build(pkt, sizeof(pkt), ident, (uint16_t)i, 16U);
        struct sockaddr_in to;
        net_scanner_sockaddr_(&to, hosts[i].ip, 0);
        (void)sendto(s, pkt, len, 0, (struct sockaddr *)&to, sizeof(to));
        if ((i % 8U) == 7U) { vTaskDelay(pdMS_TO_TICKS(5)); }
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(NET_SCANNER_ID_ICMP_WAIT_MS);
    while (!(cancel != NULL && *cancel) && net_scanner_wait_readable_(s, start, limit))
    {
        int r = recv(s, buf, NET_SCANNER_ID_BUF, 0);
        if (r <= 0) { break; }
        uint32_t src = 0;
        uint8_t ttl = 0;
        if (poom_ns_icmp_reply_parse(buf, (size_t)r, ident, &src, &ttl))
        {
            int k = net_scanner_host_idx_(hosts, n, src);
            if (k >= 0 && !out[k].ttl_known)
            {
                out[k].ttl = ttl;
                out[k].ttl_known = true;
            }
        }
    }
    (void)close(s);
}

static void net_scanner_id_udp_(const poom_net_scanner_host_t *hosts, size_t n,
                                poom_net_scanner_ident_t *out, uint8_t *buf,
                                net_scanner_loc_t *locs, size_t *n_locs,
                                const volatile bool *cancel)
{
    static const char k_msearch[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";

    int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u < 0)
    {
        printf("[net_scanner] identify: UDP socket unavailable (errno %d)\n", errno);
        return;
    }

    struct sockaddr_in to;
    net_scanner_sockaddr_(&to, 0xEFFFFFFAU /* 239.255.255.250 */, 1900);
    (void)sendto(u, k_msearch, sizeof(k_msearch) - 1U, 0, (struct sockaddr *)&to, sizeof(to));

    for (size_t i = 0; i < n && !(cancel != NULL && *cancel); ++i)
    {
        uint8_t q[96];
        size_t len = poom_ns_nbns_query_build(q, sizeof(q), (uint16_t)(0x5000U + i));
        net_scanner_sockaddr_(&to, hosts[i].ip, 137);
        if (len > 0U) { (void)sendto(u, q, len, 0, (struct sockaddr *)&to, sizeof(to)); }

        len = poom_ns_mdns_query_build(q, sizeof(q), (uint16_t)(0x6000U + i), hosts[i].ip);
        net_scanner_sockaddr_(&to, hosts[i].ip, 5353);
        if (len > 0U) { (void)sendto(u, q, len, 0, (struct sockaddr *)&to, sizeof(to)); }
        if ((i % 8U) == 7U) { vTaskDelay(pdMS_TO_TICKS(5)); }
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(NET_SCANNER_ID_UDP_WAIT_MS);
    while (!(cancel != NULL && *cancel) && net_scanner_wait_readable_(u, start, limit))
    {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int r = recvfrom(u, buf, NET_SCANNER_ID_BUF - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) { break; }
        int k = net_scanner_host_idx_(hosts, n, lwip_ntohl(from.sin_addr.s_addr));
        if (k < 0) { continue; }
        uint16_t sport = lwip_ntohs(from.sin_port);

        if (r >= 8 && memcmp(buf, "HTTP/1.1", 8) == 0)
        {
            buf[r] = '\0';
            char srv[64];
            if (poom_ns_ssdp_server((const char *)buf, (size_t)r, srv, sizeof(srv)))
            {
                poom_ns_os_from_text(&out[k], srv, POOM_NS_OSEV_CONFIRMED);
            }
            /* Remember one description URL per host (on that host itself). */
            if (locs != NULL && *n_locs < NET_SCANNER_ID_MAX_LOC)
            {
                bool have = false;
                for (size_t j = 0; j < *n_locs; ++j) { if (locs[j].host == (uint8_t)k) { have = true; } }
                uint32_t lip = 0;
                uint16_t lport = 0;
                if (!have && poom_ns_ssdp_location((const char *)buf, (size_t)r, &lip, &lport,
                                                   locs[*n_locs].path, sizeof(locs[0].path)) &&
                    lip == hosts[k].ip)
                {
                    locs[*n_locs].host = (uint8_t)k;
                    locs[*n_locs].port = lport;
                    (*n_locs)++;
                }
            }
        }
        else if (sport == 137)
        {
            char nm[POOM_NS_NAME_CAP];
            if (out[k].name[0] == '\0' &&
                poom_ns_nbns_reply_name(buf, (size_t)r, nm, sizeof(nm)))
            {
                (void)snprintf(out[k].name, sizeof(out[k].name), "%s", nm);
            }
        }
        else if (sport == 5353)
        {
            char nm[POOM_NS_NAME_CAP];
            char model[32];
            if (poom_ns_mdns_reply_parse(buf, (size_t)r, nm, sizeof(nm), model, sizeof(model)))
            {
                if (nm[0] != '\0')
                {
                    /* mDNS hostname preferred over a NetBIOS name. */
                    (void)snprintf(out[k].name, sizeof(out[k].name), "%s", nm);
                }
                if (model[0] != '\0')
                {
                    poom_ns_os_from_text(&out[k], model, POOM_NS_OSEV_CONFIRMED);
                }
            }
        }
    }
    (void)close(u);
}

/* Plain HTTP/1.0 GET; reads up to cap-1 bytes into @p dst (NUL-terminated).
 * @return bytes read, or -1. */
static int net_scanner_http_get_(uint32_t ip_host, uint16_t port, const char *path,
                                 char *dst, size_t cap)
{
    int sock = net_scanner_connect_blocking_(ip_host, port, 1000U);
    if (sock < 0) { return -1; }
    char req[160];
    char ipstr[POOM_NET_SCANNER_IP_STR_CAP];
    poom_net_scanner_ip_to_str(ip_host, ipstr, sizeof(ipstr));
    int rl = snprintf(req, sizeof(req), "GET %.80s HTTP/1.0\r\nHost: %s:%u\r\n"
                      "Connection: close\r\n\r\n", path, ipstr, (unsigned)port);
    if (rl <= 0 || send(sock, req, (size_t)rl, 0) <= 0)
    {
        net_scanner_close_probe_(sock);
        return -1;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int m = 0;
    for (int part = 0; part < 8 && m < (int)cap - 1; ++part)
    {
        int r = recv(sock, dst + m, cap - 1U - (size_t)m, 0);
        if (r <= 0) { break; }
        m += r;
    }
    dst[(m > 0) ? m : 0] = '\0';
    net_scanner_close_probe_(sock);
    return m;
}

esp_err_t poom_net_scanner_identify(const poom_net_scanner_host_t *hosts, size_t n,
                                    poom_net_scanner_ident_t *out,
                                    poom_net_scanner_upnp_t *upnp, size_t upnp_cap,
                                    size_t *upnp_count,
                                    const volatile bool *cancel)
{
    if (hosts == NULL || out == NULL) { return ESP_ERR_INVALID_ARG; }
    if (n == 0U) { return ESP_OK; }

    uint8_t *buf = (uint8_t *)malloc(NET_SCANNER_ID_BUF);
    if (buf == NULL) { return ESP_ERR_NO_MEM; }

    net_scanner_loc_t *locs = NULL;
    size_t n_locs = 0;
    if (upnp_count != NULL) { *upnp_count = 0; }
    if (upnp != NULL && upnp_cap > 0U && upnp_count != NULL)
    {
        locs = (net_scanner_loc_t *)calloc(NET_SCANNER_ID_MAX_LOC, sizeof(*locs));
    }

    net_scanner_id_icmp_(hosts, n, out, buf, cancel);
    if (!(cancel != NULL && *cancel))
    {
        net_scanner_id_udp_(hosts, n, out, buf, locs, &n_locs, cancel);
    }
    free(buf);

    /* UPnP device descriptions: manufacturer / model / firmware. */
    if (locs != NULL && n_locs > 0U && !(cancel != NULL && *cancel))
    {
        char *xml = (char *)malloc(NET_SCANNER_ID_XML_BUF);
        for (size_t j = 0; xml != NULL && j < n_locs && !(cancel != NULL && *cancel); ++j)
        {
            if (*upnp_count >= upnp_cap) { break; }
            int len = net_scanner_http_get_(hosts[locs[j].host].ip, locs[j].port, locs[j].path,
                                            xml, NET_SCANNER_ID_XML_BUF);
            if (len <= 0) { continue; }
            poom_net_scanner_upnp_t *u = &upnp[*upnp_count];
            memset(u, 0, sizeof(*u));
            char friendly[POOM_NS_NAME_CAP];
            friendly[0] = '\0';
            if (poom_ns_upnp_from_xml(xml, (size_t)len, u, friendly, sizeof(friendly)))
            {
                u->host = locs[j].host;
                u->port = locs[j].port;
                (*upnp_count)++;
            }
            poom_net_scanner_ident_t *id = &out[locs[j].host];
            if (id->name[0] == '\0' && friendly[0] != '\0')
            {
                (void)snprintf(id->name, sizeof(id->name), "%s", friendly);
            }
            /* "NETGEAR, Inc. R7000" / "Roku ... Roku Ultra" feed the OS / device fold */
            char txt[48];
            (void)snprintf(txt, sizeof(txt), "%.15s %.23s", u->manufacturer, u->model);
            poom_ns_os_from_text(id, txt, POOM_NS_OSEV_CONFIRMED);
        }
        free(xml);
    }
    free(locs);
    return ESP_OK;
}

int poom_net_scanner_http_get(uint32_t ip_host, uint16_t port, const char *path,
                              char *dst, size_t cap)
{
    if (path == NULL || dst == NULL || cap < 2U) { return -1; }
    return net_scanner_http_get_(ip_host, port, path, dst, cap);
}
