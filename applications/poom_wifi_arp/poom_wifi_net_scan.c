// SPDX-License-Identifier: MIT
/* Bounded connected-network discovery using the existing SCAN NET owner. */
#include "poom_wifi_arp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "poom_wifi_ctrl.h"

#define TCP_TIMEOUT_MS 250U
#define PING_TIMEOUT_MS 350U
#define SELECT_SLICE_MS 25U
#define PING_PARALLEL 16U

static uint32_t now_ms_(void) { return (uint32_t)(esp_timer_get_time() / 1000LL); }
static bool cancelled_(const poom_wifi_arp_control_t *c)
{ return c && c->cancelled && c->cancelled(c->user); }
static void report_(const poom_wifi_arp_control_t *c, poom_wifi_arp_progress_t *p, uint32_t start)
{
    p->elapsed_ms = now_ms_() - start;
    if(c && c->progress) c->progress(p, c->user);
}
static bool nonblocking_(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static void tcp_result_(poom_wifi_arp_tcp_scan_result_t *out, uint16_t port, int error)
{
    ++out->progress.completed;
    if(error == 0) {
        out->open_bitmap[port / 8U] |= (uint8_t)(1U << (port % 8U));
        ++out->progress.found;
    } else if(error == ETIMEDOUT) ++out->progress.timed_out;
    else if(error != ECONNREFUSED) { ++out->progress.errors; out->progress.last_socket_error = error; }
}

bool poom_wifi_arp_tcp_port_open(const poom_wifi_arp_tcp_scan_result_t *result, uint16_t port)
{ return result && port && (result->open_bitmap[port / 8U] & (uint8_t)(1U << (port % 8U))) != 0U; }

esp_err_t poom_wifi_arp_scan_tcp_range(const char *target_ip, uint16_t first_port,
                                     uint16_t last_port, const poom_wifi_arp_control_t *control,
                                     poom_wifi_arp_tcp_scan_result_t *out)
{
    struct sockaddr_in target = {.sin_family = AF_INET};
    if(!target_ip || !out || !first_port || last_port < first_port ||
       inet_pton(AF_INET, target_ip, &target.sin_addr) != 1) return ESP_ERR_INVALID_ARG;
    const uint32_t address = ntohl(target.sin_addr.s_addr);
    if(address == 0U || address >= 0xE0000000U) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->progress.total = (uint32_t)last_port - first_port + 1U;
    struct { int fd; uint16_t port; uint32_t began; } slots[POOM_WIFI_ARP_TCP_PARALLEL];
    for(size_t i = 0; i < POOM_WIFI_ARP_TCP_PARALLEL; ++i) slots[i].fd = -1;
    const uint32_t start = now_ms_();
    uint32_t next = first_port;
    unsigned active = 0U;
    esp_err_t result = ESP_OK;
    while(next <= last_port || active) {
        if(cancelled_(control)) { out->progress.cancelled = true; result = ESP_ERR_INVALID_STATE; break; }
        if(!poom_wifi_ctrl_sta_has_ip()) { result = ESP_ERR_INVALID_STATE; break; }
        for(size_t i = 0; i < POOM_WIFI_ARP_TCP_PARALLEL && next <= last_port; ++i) {
            if(slots[i].fd >= 0) continue;
            int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if(fd < 0) {
                /* A partially full socket pool can make progress by draining
                 * existing attempts. Never count an unattempted port closed. */
                if((errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) && active) break;
                out->progress.last_socket_error = errno; ++out->progress.errors; result = ESP_FAIL; goto finish;
            }
            if(fd >= FD_SETSIZE || !nonblocking_(fd)) {
                out->progress.last_socket_error = errno; ++out->progress.errors;
                close(fd); result = ESP_FAIL; goto finish;
            }
            const uint16_t port = (uint16_t)next++;
            target.sin_port = htons(port);
            ++out->progress.attempted;
            const int rc = connect(fd, (const struct sockaddr *)&target, sizeof(target));
            if(rc == 0 || (errno != EINPROGRESS && errno != EWOULDBLOCK)) {
                tcp_result_(out, port, rc == 0 ? 0 : errno); close(fd); continue;
            }
            slots[i].fd = fd; slots[i].port = port; slots[i].began = now_ms_(); ++active;
        }
        if(active) {
            fd_set writing, failed; FD_ZERO(&writing); FD_ZERO(&failed);
            int max_fd = -1;
            for(size_t i = 0; i < POOM_WIFI_ARP_TCP_PARALLEL; ++i) if(slots[i].fd >= 0) {
                FD_SET(slots[i].fd, &writing); FD_SET(slots[i].fd, &failed);
                if(slots[i].fd > max_fd) max_fd = slots[i].fd;
            }
            struct timeval wait = {.tv_sec = 0, .tv_usec = SELECT_SLICE_MS * 1000U};
            const int rc = select(max_fd + 1, NULL, &writing, &failed, &wait);
            if(rc < 0 && errno != EINTR) { out->progress.last_socket_error = errno; ++out->progress.errors; result = ESP_FAIL; break; }
            for(size_t i = 0; i < POOM_WIFI_ARP_TCP_PARALLEL; ++i) if(slots[i].fd >= 0) {
                int error = ETIMEDOUT;
                bool done = now_ms_() - slots[i].began >= TCP_TIMEOUT_MS;
                if(rc > 0 && (FD_ISSET(slots[i].fd, &writing) || FD_ISSET(slots[i].fd, &failed))) {
                    socklen_t n = sizeof(error);
                    if(getsockopt(slots[i].fd, SOL_SOCKET, SO_ERROR, &error, &n) < 0) error = errno;
                    done = true;
                }
                if(done) { tcp_result_(out, slots[i].port, error); close(slots[i].fd); slots[i].fd = -1; --active; }
            }
        } else vTaskDelay(pdMS_TO_TICKS(10U));
        report_(control, &out->progress, start);
    }
finish:
    for(size_t i = 0; i < POOM_WIFI_ARP_TCP_PARALLEL; ++i) if(slots[i].fd >= 0) close(slots[i].fd);
    report_(control, &out->progress, start);
    return result;
}

static uint16_t read_be16_(const uint8_t *p) { return ((uint16_t)p[0] << 8U) | p[1]; }
static void write_be16_(uint8_t *p, uint16_t value) { p[0] = (uint8_t)(value >> 8U); p[1] = (uint8_t)value; }
static uint16_t checksum_(const uint8_t *p, size_t n)
{
    uint32_t sum = 0U;
    while(n >= 2U) { sum += read_be16_(p); p += 2; n -= 2; }
    if(n) sum += (uint32_t)p[0] << 8U;
    while(sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
    return (uint16_t)~sum;
}

/* Pure bounds/identity check, also exercised by the host socket simulator. */
static bool ping_reply_(const uint8_t *packet, size_t length, uint32_t from,
                        uint16_t id, const uint8_t nonce[8], uint16_t *sequence)
{
    if(length < 20U || (packet[0] >> 4U) != 4U) return false;
    const size_t header = (packet[0] & 15U) * 4U;
    const size_t total = read_be16_(packet + 2U);
    if(header < 20U || header > length || total > length || total < header + 16U ||
       packet[9] != IPPROTO_ICMP || (read_be16_(packet + 6U) & 0x3fffU)) return false;
    uint32_t source; memcpy(&source, packet + 12U, sizeof(source));
    if(source != from) return false;
    const uint8_t *icmp = packet + header;
    if(icmp[0] != 0U || icmp[1] != 0U || read_be16_(icmp + 4U) != id ||
       checksum_(icmp, total - header) != 0U || memcmp(icmp + 8U, nonce, 8U) != 0) return false;
    *sequence = read_be16_(icmp + 6U);
    return true;
}

esp_err_t poom_wifi_arp_ping_subnet(poom_wifi_arp_scan_ctx_t *ctx, const poom_wifi_arp_control_t *control)
{
    if(!ctx || !ctx->hosts || !ctx->max_hosts) return ESP_ERR_INVALID_ARG;
    ctx->num_active_hosts = 0U;
    uint32_t own, mask;
    esp_err_t result = poom_wifi_arp_get_network(&own, &mask);
    if(result != ESP_OK) return result;
    if(mask == 0U || ((~mask + 1U) & ~mask) != 0U) return ESP_ERR_INVALID_ARG;
    poom_wifi_arp_progress_t progress = {0};
    /* Bound large LANs to this device's /24; narrower masks stay narrower. */
    progress.scope_limited = mask < 0xffffff00U;
    if(progress.scope_limited) mask = 0xffffff00U;
    uint32_t first = own & mask, last = first | ~mask;
    if(mask < 0xfffffffeU) { ++first; --last; }
    progress.first_ipv4 = first; progress.last_ipv4 = last;
    progress.total = last - first + 1U - (own >= first && own <= last ? 1U : 0U);
    const uint32_t start = now_ms_();
    if(cancelled_(control)) { progress.cancelled = true; report_(control, &progress, start); return ESP_ERR_INVALID_STATE; }
    if(!progress.total) { report_(control, &progress, start); return ESP_OK; }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if(fd < 0) { progress.last_socket_error = errno; ++progress.errors; report_(control, &progress, start); return ESP_FAIL; }
    if(fd >= FD_SETSIZE || !nonblocking_(fd)) { close(fd); return ESP_FAIL; }
    const uint16_t id = (uint16_t)esp_random();
    uint8_t nonce[8] = {'P','O','O','M'};
    const uint32_t random = esp_random(); memcpy(nonce + 4U, &random, 4U);
    struct { uint32_t address, began; uint16_t sequence; bool active; } slots[PING_PARALLEL] = {0};
    uint32_t next = first;
    unsigned active = 0U;
    while(next <= last || active) {
        if(cancelled_(control)) { progress.cancelled = true; result = ESP_ERR_INVALID_STATE; break; }
        if(!poom_wifi_ctrl_sta_has_ip()) { result = ESP_ERR_INVALID_STATE; break; }
        for(size_t i = 0; i < PING_PARALLEL && next <= last; ++i) {
            if(slots[i].active) continue;
            if(next == own) { ++next; if(next > last) break; }
            const uint32_t address = next++;
            uint8_t packet[16] = {8,0};
            write_be16_(packet + 4U, id);
            const uint16_t seq = (uint16_t)(address - first);
            write_be16_(packet + 6U, seq); memcpy(packet + 8U, nonce, 8U);
            write_be16_(packet + 2U, checksum_(packet, sizeof(packet)));
            struct sockaddr_in target = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(address)};
            ++progress.attempted;
            if(sendto(fd, packet, sizeof(packet), 0, (const struct sockaddr *)&target, sizeof(target)) != (int)sizeof(packet)) {
                ++progress.completed; ++progress.errors; progress.last_socket_error = errno; continue;
            }
            slots[i].address = address; slots[i].sequence = seq; slots[i].began = now_ms_(); slots[i].active = true; ++active;
        }
        if(active) {
            fd_set reading; FD_ZERO(&reading); FD_SET(fd, &reading);
            struct timeval wait = {.tv_sec = 0, .tv_usec = SELECT_SLICE_MS * 1000U};
            const int rc = select(fd + 1, &reading, NULL, NULL, &wait);
            if(rc < 0 && errno != EINTR) { ++progress.errors; progress.last_socket_error = errno; result = ESP_FAIL; break; }
            if(rc > 0) for(size_t received = 0; received < PING_PARALLEL; ++received) {
                uint8_t packet[96]; struct sockaddr_in from; socklen_t from_len = sizeof(from);
                const int n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_len);
                if(n < 0) { if(errno != EAGAIN && errno != EWOULDBLOCK) { ++progress.errors; progress.last_socket_error = errno; } break; }
                uint16_t seq;
                if(!ping_reply_(packet, (size_t)n, from.sin_addr.s_addr, id, nonce, &seq)) continue;
                for(size_t i = 0; i < PING_PARALLEL; ++i) if(slots[i].active && slots[i].sequence == seq &&
                    slots[i].address == ntohl(from.sin_addr.s_addr)) {
                    slots[i].active = false; --active; ++progress.completed; ++progress.found;
                    if(ctx->num_active_hosts < ctx->max_hosts) {
                        poom_wifi_arp_scan_host_t *host = &ctx->hosts[ctx->num_active_hosts++];
                        memset(host, 0, sizeof(*host));
                        (void)inet_ntop(AF_INET, &from.sin_addr, host->ip, sizeof(host->ip));
                        host->is_active = true; host->ping_rtt_ms = now_ms_() - slots[i].began;
                        host->mac_known = poom_wifi_arp_lookup_mac(host->ip, host->mac);
                    }
                    break;
                }
            }
            for(size_t i = 0; i < PING_PARALLEL; ++i) if(slots[i].active && now_ms_() - slots[i].began >= PING_TIMEOUT_MS) {
                slots[i].active = false; --active; ++progress.completed; ++progress.timed_out;
            }
        } else vTaskDelay(pdMS_TO_TICKS(10U));
        report_(control, &progress, start);
    }
    close(fd);
    report_(control, &progress, start);
    return result;
}
