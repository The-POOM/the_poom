// SPDX-License-Identifier: MIT
/* All socket functions below are fakes. These tests never open a network socket. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "poom_wifi_arp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "fcntl.h"

static struct { bool used; int type; uint16_t port; } sockets[16];
static unsigned active, peak, opened, closed, connections, socket_limit;
static uint32_t clock_ms, cancel_at, disconnect_at, own_ip, netmask;
static bool fail_fcntl, fail_select, fail_getsockopt, timeout_all;
static unsigned ping_behavior;
static poom_wifi_arp_progress_t latest;
static struct { uint8_t packet[36]; uint32_t from; } replies[512];
static unsigned reply_read, reply_write, ping_sends;
static uint32_t min_ping, max_ping;

uint16_t htons(uint16_t n) { return (uint16_t)((n << 8U) | (n >> 8U)); }
uint16_t ntohs(uint16_t n) { return htons(n); }
uint32_t htonl(uint32_t n) { return (n << 24U) | ((n & 0xff00U) << 8U) | ((n & 0xff0000U) >> 8U) | (n >> 24U); }
uint32_t ntohl(uint32_t n) { return htonl(n); }
int inet_pton(int family, const char *text, void *out)
{
    unsigned a,b,c,d; char extra;
    assert(family == AF_INET);
    if(sscanf(text, "%u.%u.%u.%u%c", &a,&b,&c,&d,&extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    uint32_t ip = htonl((a << 24U) | (b << 16U) | (c << 8U) | d); memcpy(out, &ip, 4); return 1;
}
const char *inet_ntop(int family, const void *address, char *out, size_t n)
{
    uint32_t ip; assert(family == AF_INET); memcpy(&ip, address, 4); ip = ntohl(ip);
    (void)snprintf(out,n,"%u.%u.%u.%u",(unsigned)(ip >> 24U),(unsigned)((ip >> 16U)&255U),(unsigned)((ip >> 8U)&255U),(unsigned)(ip&255U)); return out;
}
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
uint32_t esp_random(void) { return 0xa1b2c3d4U; }
void vTaskDelay(unsigned ticks) { clock_ms += ticks; }
bool poom_wifi_ctrl_sta_has_ip(void) { return !disconnect_at || clock_ms < disconnect_at; }
esp_err_t poom_wifi_arp_get_network(uint32_t *ip, uint32_t *mask) { *ip = own_ip; *mask = netmask; return ESP_OK; }
bool poom_wifi_arp_lookup_mac(const char *ip, uint8_t mac[6]) { (void)ip; (void)mac; return false; }

int socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && ((type == SOCK_STREAM && protocol == IPPROTO_TCP) || (type == SOCK_RAW && protocol == IPPROTO_ICMP)));
    if(active >= socket_limit) { errno = EMFILE; return -1; }
    for(int i = 2; i < 16; ++i) if(!sockets[i].used) {
        sockets[i].used = true; sockets[i].type = type; ++active; ++opened;
        if(active > peak) peak = active;
        return i;
    }
    assert(false); return -1;
}
int close(int fd) { assert(fd >= 2 && fd < 16 && sockets[fd].used); sockets[fd].used = false; --active; ++closed; return 0; }
int fcntl(int fd, int command, ...)
{
    assert(sockets[fd].used && (command == F_GETFL || command == F_SETFL));
    if(fail_fcntl) { errno = EIO; return -1; }
    return 0;
}
int connect(int fd, const struct sockaddr *address, socklen_t n)
{
    const struct sockaddr_in *target = (const struct sockaddr_in *)address;
    assert(sockets[fd].used && n == sizeof(*target) && target->sin_family == AF_INET);
    sockets[fd].port = ntohs(target->sin_port); assert(sockets[fd].port != 0);
    ++connections; errno = EINPROGRESS; return -1;
}
int select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *wait)
{
    assert(count <= FD_SETSIZE && wait->tv_sec == 0 && wait->tv_usec <= 25000);
    clock_ms += (uint32_t)(wait->tv_usec / 1000);
    if(fail_select) { errno = EIO; return -1; }
    if(error) FD_ZERO(error);
    if(read) { if(reply_read < reply_write) return 1; FD_ZERO(read); return 0; }
    assert(write);
    unsigned ready = 0;
    for(int i = 2; i < 16; ++i) if(FD_ISSET(i,write)) {
        assert(sockets[i].used);
        if(timeout_all || sockets[i].port == 123U) write->bits &= ~(1U << i);
        else ++ready;
    }
    return (int)ready;
}
int getsockopt(int fd, int level, int option, void *value, socklen_t *n)
{
    assert(sockets[fd].used && level == SOL_SOCKET && option == SO_ERROR && *n == sizeof(int));
    if(fail_getsockopt) { errno = EIO; return -1; }
    const uint16_t p = sockets[fd].port;
    *(int *)value = p == 22U || p == 80U || p == 65535U ? 0 : ECONNREFUSED;
    return 0;
}
static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for(size_t i = 0; i < n; i += 2U) sum += ((uint32_t)p[i] << 8U) + (i + 1U < n ? p[i+1U] : 0U);
    while(sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
    return (uint16_t)~sum;
}
static void enqueue_reply(uint32_t from, const uint8_t *request, unsigned variant)
{
    assert(reply_write < 512U);
    uint8_t *p = replies[reply_write].packet; memset(p,0,36);
    p[0] = 0x45; p[3] = 36; p[9] = IPPROTO_ICMP;
    memcpy(p + 12, &from, 4); memcpy(p + 20, request, 16);
    p[20] = 0; p[22] = p[23] = 0;
    if(variant == 1U) p[28] ^= 1U; /* Wrong payload, still valid checksum. */
    if(variant == 3U) p[20] = 8U; /* An echo request is not a reply. */
    uint16_t crc = checksum(p + 20, 16); p[22] = (uint8_t)(crc >> 8U); p[23] = (uint8_t)crc;
    if(variant == 2U) p[22] ^= 1U;
    replies[reply_write++].from = from;
}
int sendto(int fd, const void *data, size_t length, int flags, const struct sockaddr *address, socklen_t n)
{
    const struct sockaddr_in *target = (const struct sockaddr_in *)address;
    const uint8_t *p = data;
    assert(sockets[fd].used && sockets[fd].type == SOCK_RAW && n == sizeof(*target) && !flags);
    assert(length == 16 && p[0] == 8 && p[1] == 0 && checksum(p,length) == 0);
    uint32_t ip = ntohl(target->sin_addr.s_addr); assert(ip != own_ip);
    if(ip < min_ping) min_ping = ip; if(ip > max_ping) max_ping = ip;
    ++ping_sends;
    if(ping_behavior == 1U) enqueue_reply(target->sin_addr.s_addr,p,0);
    else if(ping_behavior == 2U) {
        const unsigned last = ip & 255U;
        enqueue_reply(target->sin_addr.s_addr,p,last == 3U ? 1U : last == 5U ? 2U : last == 6U ? 3U : 0U);
        if(last == 2U) enqueue_reply(target->sin_addr.s_addr,p,0);
    }
    return (int)length;
}
int recvfrom(int fd, void *data, size_t length, int flags, struct sockaddr *address, socklen_t *n)
{
    assert(sockets[fd].used && length >= 36 && !flags && *n == sizeof(struct sockaddr_in));
    if(reply_read == reply_write) { errno = EWOULDBLOCK; return -1; }
    struct sockaddr_in *from = (struct sockaddr_in *)address;
    memset(from,0,sizeof(*from)); from->sin_family = AF_INET; from->sin_addr.s_addr = replies[reply_read].from;
    memcpy(data,replies[reply_read++].packet,36); return 36;
}
static bool cancel(void *user) { (void)user; return cancel_at && clock_ms >= cancel_at; }
static void progress(const poom_wifi_arp_progress_t *p, void *user) { (void)user; latest = *p; }
static const poom_wifi_arp_control_t control = {.cancelled=cancel,.progress=progress};
static void reset(void)
{
    assert(!active && opened == closed);
    memset(sockets,0,sizeof(sockets)); memset(&latest,0,sizeof(latest));
    clock_ms = 0; active = peak = opened = closed = connections = 0; socket_limit = 8;
    cancel_at = disconnect_at = 0; fail_fcntl = fail_select = fail_getsockopt = timeout_all = false;
    own_ip = 0xc0a80101U; netmask = 0xfffffff8U; ping_behavior = reply_read = reply_write = ping_sends = 0;
    min_ping = UINT32_MAX; max_ping = 0;
}

int main(void)
{
    static poom_wifi_arp_tcp_scan_result_t result;
    reset();
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,65535,&control,&result) == ESP_OK);
    assert(result.progress.completed == 65535U && result.progress.attempted == 65535U && connections == 65535U);
    assert(result.progress.found == 3U && result.progress.timed_out == 1U && peak == 8U && !active);
    assert(poom_wifi_arp_tcp_port_open(&result,22) && poom_wifi_arp_tcp_port_open(&result,80) && poom_wifi_arp_tcp_port_open(&result,65535));
    assert(!poom_wifi_arp_tcp_port_open(&result,0) && !poom_wifi_arp_tcp_port_open(&result,443));

    reset(); timeout_all = true;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,32,&control,&result) == ESP_OK);
    assert(clock_ms == 1000U && result.progress.timed_out == 32U && peak == 8U && !result.progress.found);
    reset(); socket_limit = 3;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",20,25,&control,&result) == ESP_OK);
    assert(peak == 3 && result.progress.completed == 6 && result.progress.found == 1);
    reset(); socket_limit = 0;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,65535,&control,&result) == ESP_FAIL);
    assert(!connections && !result.progress.completed && result.progress.errors == 1);
    reset(); fail_fcntl = true;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,50,&control,&result) == ESP_FAIL && !active && !connections);
    reset(); fail_select = true;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,50,&control,&result) == ESP_FAIL && !active && opened == 8);
    reset(); fail_getsockopt = true;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",20,25,&control,&result) == ESP_OK && result.progress.errors == 6 && !result.progress.found);
    reset(); cancel_at = 50;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,65535,&control,&result) == ESP_ERR_INVALID_STATE);
    assert(latest.cancelled && latest.completed < 65535U && !active && clock_ms <= 75U);
    reset(); disconnect_at = 25;
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",1,65535,&control,&result) == ESP_ERR_INVALID_STATE && !active);
    reset();
    assert(poom_wifi_arp_scan_tcp_range("not an address",1,2,&control,&result) == ESP_ERR_INVALID_ARG);
    assert(poom_wifi_arp_scan_tcp_range("224.0.0.1",1,2,&control,&result) == ESP_ERR_INVALID_ARG);
    assert(poom_wifi_arp_scan_tcp_range("192.168.1.2",0,2,&control,&result) == ESP_ERR_INVALID_ARG && !opened);

    poom_wifi_arp_scan_host_t hosts[254]; poom_wifi_arp_scan_ctx_t ctx = {.hosts=hosts,.max_hosts=254};
    reset(); ping_behavior = 2;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_OK);
    assert(ping_sends == 5 && latest.completed == 5 && latest.found == 2 && latest.timed_out == 3 && ctx.num_active_hosts == 2);
    assert(!strcmp(hosts[0].ip,"192.168.1.2") && !strcmp(hosts[1].ip,"192.168.1.4") && !hosts[0].mac_known);
    reset(); netmask = 0xffffff80U;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_OK && latest.total == 125 && ping_sends == 125);
    assert(min_ping == 0xc0a80102U && max_ping == 0xc0a8017eU && !latest.scope_limited);
    reset(); netmask = 0xffff0000U; ping_behavior = 1;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_OK && latest.scope_limited && latest.found == 253);
    assert(min_ping == 0xc0a80102U && max_ping == 0xc0a801feU && ctx.num_active_hosts == 253);
    reset(); netmask = 0xfffffffeU; own_ip = 0xc0a80100U; ping_behavior = 1;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_OK && latest.total == 1 && ctx.num_active_hosts == 1);
    reset(); netmask = UINT32_MAX;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_OK && !latest.total && !opened);
    reset(); netmask = 0xff00ff00U;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_ERR_INVALID_ARG && !opened);
    reset(); cancel_at = 25;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_ERR_INVALID_STATE && latest.cancelled && !active);
    reset(); fail_select = true;
    assert(poom_wifi_arp_ping_subnet(&ctx,&control) == ESP_FAIL && !active);
    assert(opened == closed);
    puts("Network scan: all 65535 ports, bounded parallelism, timeout/error/cancel cleanup and ICMP scope/reply validation passed (mock sockets only)");
    return 0;
}
