// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "dfu.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_netif.h"
#include "http_server.h"
#include "lwip/ip4_addr.h"
#include "mdns_manager.h"
#include "poom_dfu_log.h"
#include "poom_wifi_ctrl.h"

static poom_fw_update_show_event_cb_t s_poom_fw_update_show_event_cb = NULL;
#if CONFIG_POOM_DFU_ENABLE_LOG
static const char* POOM_DFU_TAG = "poom_fw_update";
#endif
static bool s_poom_fw_update_started = false;
static char s_poom_fw_update_ap_ip[16] = {0};

/**
 * @brief Internal helper for `poom_fw_update_get_ap_netif`.
 *
 * @return esp_netif_t*
 */
static esp_netif_t* poom_fw_update_get_ap_netif_(void)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if(netif != NULL) {
        return netif;
    }

    static const char needle[] = "WIFI_AP";
    esp_netif_t* cur = NULL;
    while((cur = esp_netif_next_unsafe(cur)) != NULL) {
        const char* key = esp_netif_get_ifkey(cur);
        if((key != NULL) && (strstr(key, needle) != NULL)) {
            return cur;
        }
    }

    return NULL;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] out_ip Parameter passed to the function.
 * @param[in] out_ip_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_fw_update_get_ap_ip_str_(char* out_ip, size_t out_ip_len)
{
    esp_netif_ip_info_t info;
    esp_netif_t* netif;
    ip4_addr_t ip;

    if((out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';

    netif = poom_fw_update_get_ap_netif_();
    if(netif == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if(esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return ESP_FAIL;
    }

    ip.addr = info.ip.addr;
    if(ip4addr_ntoa_r(&ip, out_ip, out_ip_len) == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return void
 */
static void poom_fw_update_cleanup_init_failure_(void)
{
    (void)http_server_stop();
    teardown_mdns();
    (void)poom_wifi_ctrl_deinit();
    s_poom_fw_update_ap_ip[0] = '\0';
}

esp_err_t poom_fw_update_init(void)
{
    esp_err_t err;

    if(s_poom_fw_update_started) {
        return ESP_OK;
    }

    err = poom_wifi_ctrl_manager_ap_start(NULL);
    if(err != ESP_OK) {
        POOM_DFU_PRINTF_E(POOM_DFU_TAG, "AP start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = setup_mdns();
    if(err != ESP_OK) {
        POOM_DFU_PRINTF_W(POOM_DFU_TAG, "mDNS setup failed: %s", esp_err_to_name(err));
    }

    err = http_server_start();
    if(err != ESP_OK) {
        POOM_DFU_PRINTF_E(POOM_DFU_TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        poom_fw_update_cleanup_init_failure_();
        return err;
    }

    if(poom_fw_update_get_ap_ip_str_(s_poom_fw_update_ap_ip, sizeof(s_poom_fw_update_ap_ip)) != ESP_OK) {
        s_poom_fw_update_ap_ip[0] = '\0';
    }

    s_poom_fw_update_started = true;
    return ESP_OK;
}

esp_err_t poom_fw_update_deinit(void)
{
    esp_err_t err = ESP_OK;
    esp_err_t stop_err;

    if(!s_poom_fw_update_started) {
        return ESP_ERR_INVALID_STATE;
    }

    stop_err = http_server_stop();
    if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
        POOM_DFU_PRINTF_W(POOM_DFU_TAG, "HTTP server stop warning: %s", esp_err_to_name(stop_err));
        err = stop_err;
    }

    teardown_mdns();

    stop_err = poom_wifi_ctrl_deinit();
    if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
        POOM_DFU_PRINTF_W(POOM_DFU_TAG, "WiFi deinit warning: %s", esp_err_to_name(stop_err));
        if(err == ESP_OK) {
            err = stop_err;
        }
    }

    s_poom_fw_update_started = false;
    s_poom_fw_update_ap_ip[0] = '\0';
    return err;
}

esp_err_t poom_fw_update_set_show_event_cb(poom_fw_update_show_event_cb_t cb)
{
    s_poom_fw_update_show_event_cb = cb;
    return ESP_OK;
}

void poom_fw_update_emit_event(uint8_t event, void* context)
{
    poom_fw_update_show_event_cb_t cb = s_poom_fw_update_show_event_cb;
    if(cb != NULL) {
        cb(event, context);
    }
}

const char* poom_fw_update_get_wifi_ap_ssid(void)
{
    return POOM_WIFI_CTRL_MANAGER_AP_SSID;
}

const char* poom_fw_update_get_wifi_ap_password(void)
{
    return POOM_WIFI_CTRL_MANAGER_AP_PASSWORD;
}

const char* poom_fw_update_get_wifi_ap_ip(void)
{
    if(poom_fw_update_get_ap_ip_str_(s_poom_fw_update_ap_ip, sizeof(s_poom_fw_update_ap_ip)) != ESP_OK) {
        s_poom_fw_update_ap_ip[0] = '\0';
    }
    return s_poom_fw_update_ap_ip;
}
