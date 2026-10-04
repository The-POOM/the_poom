// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_captive.h"
#include "captive_clients_internal.h"
#include "captive_client_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static poom_wifi_captive_client_t *s_clients;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static uint32_t s_revision;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

/* All table access is bounded and protected; no driver calls under this lock. */
static poom_wifi_captive_client_t *find_client_(const uint8_t *mac, bool create)
{
    poom_wifi_captive_client_t *free_slot = NULL;
    for(size_t i = 0; i < POOM_WIFI_CAPTIVE_MAX_CLIENTS; ++i)
    {
        if(memcmp(s_clients[i].mac, mac, 6) == 0) return &s_clients[i];
        if(!s_clients[i].online && (free_slot == NULL)) free_slot = &s_clients[i];
    }
    if(create && (free_slot != NULL))
    {
        memset(free_slot, 0, sizeof(*free_slot));
        memcpy(free_slot->mac, mac, 6);
        return free_slot;
    }
    return NULL;
}

#ifdef CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME
static bool hostname_duplicate_(const poom_wifi_captive_client_t *client, const char *hostname)
{
    if((client == NULL) || (hostname == NULL) || (hostname[0] == '\0')) return false;
    for(size_t i = 0; i < POOM_WIFI_CAPTIVE_MAX_CLIENTS; ++i)
    {
        if((&s_clients[i] != client) && s_clients[i].online &&
           (s_clients[i].name[0] != '\0') && (strcmp(s_clients[i].name, hostname) == 0))
        {
            return true;
        }
    }
    return false;
}

static void update_hostname_(poom_wifi_captive_client_t *client, const char *hostname)
{
    char clean[POOM_WIFI_CAPTIVE_CLIENT_NAME_LEN];

    if(client == NULL) return;
    client->name[0] = '\0';
    if((hostname == NULL) || (hostname[0] == '\0')) return;

    captive_client_copy_name(clean, sizeof(clean), hostname, strlen(hostname));
    if((clean[0] == '\0') || hostname_duplicate_(client, clean)) return;

    memcpy(client->name, clean, sizeof(client->name));
}
#endif

static void client_event_(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if(data == NULL) return;
    char ip[16] = {0};
    if(base == IP_EVENT)
    {
        const ip_event_assigned_ip_to_client_t *event = data;
        if(event->esp_netif != esp_netif_get_handle_from_ifkey(CAPTIVE_PORTAL_NET_NAME)) return;
        (void)snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip));
    }

    portENTER_CRITICAL(&s_lock);
    if(s_active)
    {
        if((base == WIFI_EVENT) && (id == WIFI_EVENT_AP_STACONNECTED))
        {
            const wifi_event_ap_staconnected_t *event = data;
            poom_wifi_captive_client_t *client = find_client_(event->mac, true);
            if(client != NULL)
            {
                memset(client, 0, sizeof(*client));
                memcpy(client->mac, event->mac, 6);
                client->online = true;
            }
        }
        else if((base == WIFI_EVENT) && (id == WIFI_EVENT_AP_STADISCONNECTED))
        {
            const wifi_event_ap_stadisconnected_t *event = data;
            poom_wifi_captive_client_t *client = find_client_(event->mac, false);
            if(client != NULL)
            {
                client->online = false;
                client->rssi_known = false;
            }
        }
        else if(base == IP_EVENT)
        {
            const ip_event_assigned_ip_to_client_t *event = data;
            poom_wifi_captive_client_t *client = find_client_(event->mac, true);
            if(client != NULL)
            {
                client->online = true;
                memcpy(client->ip, ip, sizeof(client->ip));
#ifdef CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME
                update_hostname_(client, event->hostname);
#endif
            }
        }
        ++s_revision;
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t captive_clients_start(void)
{
    poom_wifi_captive_client_t *clients = heap_caps_calloc(
        POOM_WIFI_CAPTIVE_MAX_CLIENTS, sizeof(*clients), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(clients == NULL) clients = calloc(POOM_WIFI_CAPTIVE_MAX_CLIENTS, sizeof(*clients));
    if(clients == NULL) return ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_lock);
    if(s_clients != NULL)
    {
        portEXIT_CRITICAL(&s_lock);
        free(clients);
        return ESP_ERR_INVALID_STATE;
    }
    s_clients = clients;
    s_active = true;
    ++s_revision;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         client_event_, NULL, &s_wifi_handler);
    if(err == ESP_OK)
    {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                  client_event_, NULL, &s_ip_handler);
    }
    if(err != ESP_OK) captive_clients_stop();
    return err;
}

void captive_clients_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    ++s_revision;
    portEXIT_CRITICAL(&s_lock);
    if(s_wifi_handler != NULL)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if(s_ip_handler != NULL)
    {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, s_ip_handler);
        s_ip_handler = NULL;
    }
    portENTER_CRITICAL(&s_lock);
    poom_wifi_captive_client_t *clients = s_clients;
    s_clients = NULL;
    portEXIT_CRITICAL(&s_lock);
    free(clients);
}

esp_err_t poom_wifi_captive_get_clients(poom_wifi_captive_client_t *clients,
                                       size_t capacity, size_t *count)
{
    if((clients == NULL) || (count == NULL) || (capacity < POOM_WIFI_CAPTIVE_MAX_CLIENTS))
        return ESP_ERR_INVALID_ARG;
    *count = 0U;
    portENTER_CRITICAL(&s_lock);
    bool active = s_active;
    uint32_t revision = s_revision;
    portEXIT_CRITICAL(&s_lock);
    if(!active) return ESP_ERR_INVALID_STATE;

    wifi_sta_list_t stations = {0};
    esp_err_t err = esp_wifi_ap_get_sta_list(&stations);
    if(err != ESP_OK) return err;
    size_t total = (size_t)stations.num;
    if(total > POOM_WIFI_CAPTIVE_MAX_CLIENTS) total = POOM_WIFI_CAPTIVE_MAX_CLIENTS;
    esp_netif_pair_mac_ip_t pairs[POOM_WIFI_CAPTIVE_MAX_CLIENTS] = {0};
    char ips[POOM_WIFI_CAPTIVE_MAX_CLIENTS][16] = {0};
    for(size_t i = 0; i < total; ++i) memcpy(pairs[i].mac, stations.sta[i].mac, 6);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(CAPTIVE_PORTAL_NET_NAME);
    if((total > 0U) && (netif != NULL) &&
       (esp_netif_dhcps_get_clients_by_mac(netif, (int)total, pairs) == ESP_OK))
    {
        for(size_t i = 0; i < total; ++i)
        {
            if(pairs[i].ip.addr == 0U) continue;
            for(size_t j = 0; j < total; ++j)
            {
                if(memcmp(pairs[i].mac, stations.sta[j].mac, 6) == 0)
                {
                    (void)snprintf(ips[j], sizeof(ips[j]), IPSTR, IP2STR(&pairs[i].ip));
                    break;
                }
            }
        }
    }

    portENTER_CRITICAL(&s_lock);
    if(!s_active)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Do not overwrite a connect/disconnect or lease event with an older poll. */
    if(revision == s_revision)
    {
        for(size_t i = 0; i < POOM_WIFI_CAPTIVE_MAX_CLIENTS; ++i)
        {
            bool found = false;
            for(size_t j = 0; j < total; ++j)
                if(memcmp(s_clients[i].mac, stations.sta[j].mac, 6) == 0) found = true;
            if(!found)
            {
                s_clients[i].online = false;
                s_clients[i].rssi_known = false;
            }
        }
        for(size_t i = 0; i < total; ++i)
        {
            poom_wifi_captive_client_t *client = find_client_(stations.sta[i].mac, true);
            if(client == NULL) continue;
            client->online = true;
            client->rssi = stations.sta[i].rssi;
            client->rssi_known = true;
            if(ips[i][0] != '\0')
            {
                if((client->ip[0] != '\0') && (strcmp(client->ip, ips[i]) != 0))
                {
                    client->name[0] = '\0';
                    client->device_type[0] = '\0';
                }
                memcpy(client->ip, ips[i], sizeof(client->ip));
            }
        }
    }
    for(size_t i = 0; i < POOM_WIFI_CAPTIVE_MAX_CLIENTS; ++i)
        if(s_clients[i].online) clients[(*count)++] = s_clients[i];
    portEXIT_CRITICAL(&s_lock);

    /* Stable ordering, independent of the driver's association-list order. */
    for(size_t i = 1; i < *count; ++i)
    {
        poom_wifi_captive_client_t item = clients[i];
        size_t j = i;
        while((j > 0U) && (memcmp(clients[j - 1U].mac, item.mac, 6) > 0))
        {
            clients[j] = clients[j - 1U];
            --j;
        }
        clients[j] = item;
    }
    return ESP_OK;
}

const char *poom_wifi_captive_client_label(const poom_wifi_captive_client_t *client)
{
    if(client->name[0] != '\0') return client->name;
    if(client->device_type[0] != '\0') return client->device_type;
    if(client->ip[0] != '\0') return client->ip;
    return "Pending IP";
}

static bool captive_clients_find_mac_for_ip_(const char *ip, uint8_t out_mac[6])
{
    if((ip == NULL) || (ip[0] == '\0') || (out_mac == NULL)) return false;

    wifi_sta_list_t stations = {0};
    if(esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return false;

    size_t total = (size_t)stations.num;
    if(total > POOM_WIFI_CAPTIVE_MAX_CLIENTS) total = POOM_WIFI_CAPTIVE_MAX_CLIENTS;
    if(total == 0U) return false;

    esp_netif_pair_mac_ip_t pairs[POOM_WIFI_CAPTIVE_MAX_CLIENTS] = {0};
    for(size_t i = 0; i < total; ++i) memcpy(pairs[i].mac, stations.sta[i].mac, 6);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(CAPTIVE_PORTAL_NET_NAME);
    if((netif == NULL) ||
       (esp_netif_dhcps_get_clients_by_mac(netif, (int)total, pairs) != ESP_OK))
    {
        return false;
    }

    for(size_t i = 0; i < total; ++i)
    {
        char pair_ip[16] = {0};
        if(pairs[i].ip.addr == 0U) continue;
        (void)snprintf(pair_ip, sizeof(pair_ip), IPSTR, IP2STR(&pairs[i].ip));
        if(strcmp(pair_ip, ip) == 0)
        {
            memcpy(out_mac, pairs[i].mac, 6);
            return true;
        }
    }

    return false;
}

static bool captive_clients_set_type_by_ip_(const char *ip, const char *type)
{
    if((ip == NULL) || (ip[0] == '\0') || (type == NULL) || (type[0] == '\0')) return false;

    for(size_t i = 0; i < POOM_WIFI_CAPTIVE_MAX_CLIENTS; ++i)
    {
        if(s_clients[i].online && (strcmp(s_clients[i].ip, ip) == 0))
        {
            captive_client_copy_name(s_clients[i].device_type,
                                     sizeof(s_clients[i].device_type), type, strlen(type));
            return true;
        }
    }

    return false;
}

void captive_clients_observe_http(httpd_req_t *req)
{
    if(req == NULL) return;
    struct sockaddr_storage peer;
    socklen_t length = sizeof(peer);
    if(getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &length) != 0) return;
    if(peer.ss_family != AF_INET) return;
    char ip[16];
    const struct sockaddr_in *addr = (const struct sockaddr_in *)&peer;
    if(inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL) return;
    char ua[512];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    if((err != ESP_OK) && (err != ESP_ERR_HTTPD_RESULT_TRUNC)) return;
    ua[sizeof(ua) - 1U] = '\0';
    const char *type = captive_client_type(ua);
    if(type[0] == '\0') return;

    portENTER_CRITICAL(&s_lock);
    bool updated = s_active && captive_clients_set_type_by_ip_(ip, type);
    portEXIT_CRITICAL(&s_lock);
    if(updated) return;

    uint8_t mac[6] = {0};
    if(!captive_clients_find_mac_for_ip_(ip, mac)) return;

    portENTER_CRITICAL(&s_lock);
    if(s_active)
    {
        poom_wifi_captive_client_t *client = find_client_(mac, true);
        if(client != NULL)
        {
            client->online = true;
            memcpy(client->ip, ip, sizeof(client->ip));
            captive_client_copy_name(client->device_type,
                                     sizeof(client->device_type), type, strlen(type));
            ++s_revision;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
