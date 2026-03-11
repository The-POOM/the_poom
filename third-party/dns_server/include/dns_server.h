/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dns_server.h
 * @brief Public API for a lightweight DNS A-record responder.
 */

#include "esp_netif_ip_addr.h"

/* =========================
 * Logging control
 * ========================= */
#ifndef DNS_SERVER_LOG_ENABLED
    #if defined(CONFIG_DNS_SERVER_DEBUG)
        #define DNS_SERVER_LOG_ENABLED           (1)
    #else
        #define DNS_SERVER_LOG_ENABLED           (0)
    #endif
#endif

#ifndef DNS_SERVER_DEBUG_LOG_ENABLED
#define DNS_SERVER_DEBUG_LOG_ENABLED             (0)
#endif

/* =========================
 * Config limits
 * ========================= */
#ifndef DNS_SERVER_MAX_ITEMS
#define DNS_SERVER_MAX_ITEMS                     (1)
#endif

/**
 * @brief Helper for a single wildcard/exact-name DNS rule bound to one netif.
 *
 * Example:
 * @code{.c}
 * dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
 * @endcode
 */
#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key) \
    { \
        .num_of_entries = 1, \
        .item = { \
            { \
                .name = queried_name, \
                .if_key = netif_key, \
            }, \
        }, \
    }

/**
 * @brief One DNS routing rule.
 *
 * Match is exact name or wildcard `"*"`.
 * If `if_key` is provided, response IP is taken from that netif.
 * Otherwise `ip` is used.
 */
typedef struct dns_entry_pair {
    /** Exact queried domain name or `"*"` wildcard. */
    const char *name;
    /** Netif key used to resolve current IPv4 response (optional). */
    const char *if_key;
    /** Static IPv4 response when `if_key == NULL`. */
    esp_ip4_addr_t ip;
} dns_entry_pair_t;

/**
 * @brief DNS server start configuration.
 */
typedef struct dns_server_config {
    /** Number of valid entries in @ref item. */
    int num_of_entries;
    /** Routing rules table (size bounded by @ref DNS_SERVER_MAX_ITEMS). */
    dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS];
} dns_server_config_t;

/**
 * @brief Opaque DNS server handle.
 */
typedef struct dns_server_handle *dns_server_handle_t;

/**
 * @brief Start DNS server task.
 *
 * @param config Pointer to configuration.
 * @return Server handle on success, NULL on failure.
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config);

/**
 * @brief Stop DNS server task and free its resources.
 *
 * @param handle Server handle.
 */
void stop_dns_server(dns_server_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* DNS_SERVER_H */
