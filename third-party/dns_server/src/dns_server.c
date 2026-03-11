/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#include "dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

/**
 * @file dns_server.c
 * @brief Lightweight DNS server that answers A queries from configured rules.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */
#if DNS_SERVER_LOG_ENABLED
    static const char *DNS_SERVER_TAG = "dns_server";

    #define DNS_SERVER_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", DNS_SERVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define DNS_SERVER_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", DNS_SERVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define DNS_SERVER_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", DNS_SERVER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if DNS_SERVER_DEBUG_LOG_ENABLED
        #define DNS_SERVER_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", DNS_SERVER_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define DNS_SERVER_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define DNS_SERVER_PRINTF_E(...) do { } while (0)
    #define DNS_SERVER_PRINTF_W(...) do { } while (0)
    #define DNS_SERVER_PRINTF_I(...) do { } while (0)
    #define DNS_SERVER_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define DNS_SERVER_PORT                              (53)
#define DNS_SERVER_PACKET_MAX_LEN                    (256U)
#define DNS_SERVER_TASK_NAME                         "dns_server"
#define DNS_SERVER_TASK_STACK                        (4096U)
#define DNS_SERVER_TASK_PRIORITY                     (5U)

#define DNS_SERVER_OPCODE_MASK                       (0x7800U)
#define DNS_SERVER_QR_RESPONSE_BIT                   (0x8000U)
#define DNS_SERVER_QD_TYPE_A                         (0x0001U)
#define DNS_SERVER_PTR_OFFSET_MASK                   (0xC000U)
#define DNS_SERVER_PTR_OFFSET_MAX                    (0x3FFFU)
#define DNS_SERVER_LABEL_PTR_MASK                    (0xC0U)
#define DNS_SERVER_ANS_TTL_SEC                       (300U)

/* =========================
 * Local packet structures
 * ========================= */
typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((__packed__)) {
    uint16_t type;
    uint16_t class_;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class_;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

/* =========================
 * Local state
 * ========================= */
struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int sock;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

/* =========================
 * Local helpers
 * ========================= */
static bool dns_server_name_matches_(const char *rule_name, const char *query_name)
{
    if((rule_name == NULL) || (query_name == NULL)) {
        return false;
    }

    return ((strcmp(rule_name, "*") == 0) || (strcmp(rule_name, query_name) == 0));
}

static bool dns_server_resolve_entry_ip_(const dns_entry_pair_t *entry, esp_ip4_addr_t *out_ip)
{
    if((entry == NULL) || (out_ip == NULL)) {
        return false;
    }

    if((entry->if_key != NULL) && (entry->if_key[0] != '\0')) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(entry->if_key);
        if(netif == NULL) {
            DNS_SERVER_PRINTF_W("unknown if_key='%s'", entry->if_key);
            return false;
        }

        esp_netif_ip_info_t ip_info = {0};
        esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
        if(ret != ESP_OK) {
            DNS_SERVER_PRINTF_W("get_ip_info failed for if_key='%s': %d", entry->if_key, (int)ret);
            return false;
        }

        out_ip->addr = ip_info.ip.addr;
        return (out_ip->addr != IPADDR_ANY);
    }

    if(entry->ip.addr != IPADDR_ANY) {
        out_ip->addr = entry->ip.addr;
        return true;
    }

    return false;
}

static bool dns_server_find_answer_ip_(dns_server_handle_t handle,
                                       const char *name,
                                       esp_ip4_addr_t *out_ip)
{
    if((handle == NULL) || (name == NULL) || (out_ip == NULL)) {
        return false;
    }

    for(int i = 0; i < handle->num_of_entries; ++i) {
        if(!dns_server_name_matches_(handle->entry[i].name, name)) {
            continue;
        }

        if(dns_server_resolve_entry_ip_(&handle->entry[i], out_ip)) {
            return true;
        }
    }

    return false;
}

static const uint8_t *dns_server_parse_qname_(const uint8_t *qname,
                                              const uint8_t *packet_end,
                                              char *parsed_name,
                                              size_t parsed_name_size)
{
    size_t out_pos = 0U;
    const uint8_t *cursor = qname;

    if((qname == NULL) || (packet_end == NULL) || (parsed_name == NULL) || (parsed_name_size == 0U)) {
        return NULL;
    }

    while(cursor < packet_end) {
        uint8_t label_len = *cursor;
        cursor++;

        if(label_len == 0U) {
            if(out_pos >= parsed_name_size) {
                return NULL;
            }
            parsed_name[out_pos] = '\0';
            return cursor;
        }

        if((label_len & DNS_SERVER_LABEL_PTR_MASK) != 0U) {
            DNS_SERVER_PRINTF_W("compressed qname not supported");
            return NULL;
        }

        if((size_t)(packet_end - cursor) < label_len) {
            return NULL;
        }

        if(out_pos != 0U) {
            if((out_pos + 1U) >= parsed_name_size) {
                return NULL;
            }
            parsed_name[out_pos++] = '.';
        }

        if((out_pos + label_len) >= parsed_name_size) {
            return NULL;
        }

        memcpy(&parsed_name[out_pos], cursor, label_len);
        out_pos += label_len;
        cursor += label_len;
    }

    return NULL;
}

static int dns_server_prepare_reply_(const uint8_t *req,
                                     size_t req_len,
                                     uint8_t *reply,
                                     size_t reply_max_len,
                                     dns_server_handle_t handle)
{
    const uint8_t *req_end = req + req_len;
    const uint8_t *cur_qd_ptr = NULL;
    uint8_t *cur_ans_ptr = NULL;
    uint16_t qd_count = 0U;
    uint16_t an_count = 0U;
    char name[128];

    if((req == NULL) || (reply == NULL) || (handle == NULL)) {
        return -1;
    }
    if((req_len < sizeof(dns_header_t)) || (req_len > reply_max_len)) {
        return -1;
    }

    memset(reply, 0, reply_max_len);
    memcpy(reply, req, req_len);

    dns_header_t *header = (dns_header_t *)reply;
    uint16_t flags = ntohs(header->flags);

    if((flags & DNS_SERVER_OPCODE_MASK) != 0U) {
        return 0;
    }

    flags |= DNS_SERVER_QR_RESPONSE_BIT;
    header->flags = htons(flags);

    qd_count = ntohs(header->qd_count);
    header->an_count = 0U;

    cur_qd_ptr = req + sizeof(dns_header_t);
    cur_ans_ptr = reply + req_len;

    for(uint16_t qd_i = 0U; qd_i < qd_count; qd_i++) {
        const uint8_t *name_ptr = cur_qd_ptr;
        const uint8_t *name_end_ptr = dns_server_parse_qname_(name_ptr, req_end, name, sizeof(name));

        if(name_end_ptr == NULL) {
            DNS_SERVER_PRINTF_E("failed to parse qname");
            return -1;
        }
        if((size_t)(req_end - name_end_ptr) < sizeof(dns_question_t)) {
            DNS_SERVER_PRINTF_E("dns question too short");
            return -1;
        }

        const dns_question_t *question = (const dns_question_t *)name_end_ptr;
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class_);

        DNS_SERVER_PRINTF_D("query type=%u class=%u name=%s", qd_type, qd_class, name);

        cur_qd_ptr = name_end_ptr + sizeof(dns_question_t);

        if(qd_type != DNS_SERVER_QD_TYPE_A) {
            continue;
        }

        esp_ip4_addr_t ip = {.addr = IPADDR_ANY};
        if(!dns_server_find_answer_ip_(handle, name, &ip)) {
            continue;
        }

        if(((size_t)(cur_ans_ptr - reply) + sizeof(dns_answer_t)) > reply_max_len) {
            DNS_SERVER_PRINTF_E("reply buffer too small");
            return -1;
        }

        uint32_t qname_offset = (uint32_t)(name_ptr - req);
        if(qname_offset > DNS_SERVER_PTR_OFFSET_MAX) {
            return -1;
        }

        dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
        answer->ptr_offset = htons((uint16_t)(DNS_SERVER_PTR_OFFSET_MASK | qname_offset));
        answer->type = htons(DNS_SERVER_QD_TYPE_A);
        answer->class_ = htons(qd_class);
        answer->ttl = htonl(DNS_SERVER_ANS_TTL_SEC);
        answer->addr_len = htons(sizeof(ip.addr));
        answer->ip_addr = ip.addr;

        cur_ans_ptr += sizeof(dns_answer_t);
        an_count++;
    }

    header->an_count = htons(an_count);
    if(an_count == 0U) {
        return 0;
    }

    return (int)(cur_ans_ptr - reply);
}

static int dns_server_socket_open_(void)
{
    struct sockaddr_in dest_addr = {0};
    int sock = -1;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DNS_SERVER_PORT);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(sock < 0) {
        DNS_SERVER_PRINTF_E("socket create failed: errno=%d", errno);
        return -1;
    }

    if(bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        DNS_SERVER_PRINTF_E("socket bind failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    DNS_SERVER_PRINTF_I("socket bound on port %d", DNS_SERVER_PORT);
    return sock;
}

static void dns_server_socket_close_(dns_server_handle_t handle)
{
    if((handle != NULL) && (handle->sock >= 0)) {
        shutdown(handle->sock, 0);
        close(handle->sock);
        handle->sock = -1;
    }
}

static void dns_server_task_(void *param)
{
    dns_server_handle_t handle = (dns_server_handle_t)param;
    uint8_t rx_buffer[DNS_SERVER_PACKET_MAX_LEN];
    uint8_t reply[DNS_SERVER_PACKET_MAX_LEN];

    if(handle == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while(handle->started) {
        if(handle->sock < 0) {
            handle->sock = dns_server_socket_open_();
            if(handle->sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(handle->sock,
                           rx_buffer,
                           sizeof(rx_buffer),
                           0,
                           (struct sockaddr *)&source_addr,
                           &socklen);

        if(len < 0) {
            if(handle->started) {
                DNS_SERVER_PRINTF_E("recvfrom failed: errno=%d", errno);
                dns_server_socket_close_(handle);
            }
            continue;
        }

        int reply_len = dns_server_prepare_reply_(rx_buffer,
                                                  (size_t)len,
                                                  reply,
                                                  sizeof(reply),
                                                  handle);
        if(reply_len <= 0) {
            continue;
        }

        if(sendto(handle->sock,
                  reply,
                  (size_t)reply_len,
                  0,
                  (struct sockaddr *)&source_addr,
                  socklen) < 0) {
            DNS_SERVER_PRINTF_E("sendto failed: errno=%d", errno);
            dns_server_socket_close_(handle);
        }
    }

    dns_server_socket_close_(handle);
    handle->task = NULL;
    vTaskDelete(NULL);
}

/* =========================
 * Public API
 * ========================= */
dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    dns_server_handle_t handle = NULL;
    size_t alloc_size = 0U;

    if((config == NULL) ||
       (config->num_of_entries <= 0) ||
       (config->num_of_entries > DNS_SERVER_MAX_ITEMS)) {
        DNS_SERVER_PRINTF_E("invalid config");
        return NULL;
    }

    alloc_size = sizeof(struct dns_server_handle) +
                 ((size_t)config->num_of_entries * sizeof(dns_entry_pair_t));

    handle = (dns_server_handle_t)calloc(1, alloc_size);
    if(handle == NULL) {
        DNS_SERVER_PRINTF_E("failed to allocate server handle");
        return NULL;
    }

    handle->started = true;
    handle->sock = -1;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry,
           config->item,
           (size_t)config->num_of_entries * sizeof(dns_entry_pair_t));

    if(xTaskCreate(dns_server_task_,
                   DNS_SERVER_TASK_NAME,
                   DNS_SERVER_TASK_STACK,
                   handle,
                   DNS_SERVER_TASK_PRIORITY,
                   &handle->task) != pdPASS) {
        DNS_SERVER_PRINTF_E("failed to create dns task");
        free(handle);
        return NULL;
    }

    DNS_SERVER_PRINTF_I("started");
    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if(handle == NULL) {
        return;
    }

    handle->started = false;
    dns_server_socket_close_(handle);

    if(handle->task != NULL) {
        vTaskDelete(handle->task);
        handle->task = NULL;
    }

    free(handle);
    DNS_SERVER_PRINTF_I("stopped");
}
