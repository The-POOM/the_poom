// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mdns_manager.h"
#include "nvs.h"
#include "poom_web_http_server.h"
#include "poom_web_log.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"

static const char* POOM_WEB_TAG = "poom_web";
static bool s_poom_web_started = false;
static bool s_poom_web_sd_mounted_by_component = false;

typedef enum
{
    POOM_WEB_WIFI_MODE_NONE = 0,
    POOM_WEB_WIFI_MODE_AP,
    POOM_WEB_WIFI_MODE_STA,
} poom_web_wifi_mode_t;

static poom_web_wifi_mode_t s_poom_web_wifi_mode = POOM_WEB_WIFI_MODE_NONE;
static char s_poom_web_ap_ip[16] = {0};
static char s_poom_web_sta_ssid[33] = {0};
static char s_poom_web_sta_ip[16] = {0};

#define POOM_WEB_STA_CONNECT_TIMEOUT_MS (15000U)
#define POOM_WEB_STA_CONNECT_ATTEMPTS   (2U)

/**
 * @brief Internal helper for `poom_web_log_heap`.
 *
 * @param[in] stage Parameter passed to the function.
 * @return void
 */
static void poom_web_log_heap_(const char* stage)
{
    POOM_WEB_PRINTF_I(POOM_WEB_TAG,
                      "HEAP %s: free=%u min=%u internal=%u largest=%u",
                      (stage != NULL) ? stage : "?",
                      (unsigned)esp_get_free_heap_size(),
                      (unsigned)esp_get_minimum_free_heap_size(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/**
 * @brief Internal helper for `poom_web_get_sta_netif`.
 *
 * @return esp_netif_t*
 */
static esp_netif_t* poom_web_get_sta_netif_(void)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif != NULL) {
        return netif;
    }

    static const char needle[] = "WIFI_STA";
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
 * @brief Internal helper for `poom_web_get_ap_netif`.
 *
 * @return esp_netif_t*
 */
static esp_netif_t* poom_web_get_ap_netif_(void)
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
static esp_err_t poom_web_get_sta_ip_str_(char* out_ip, size_t out_ip_len)
{
    esp_netif_ip_info_t info;
    esp_netif_t* netif;
    ip4_addr_t ip;

    if((out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';

    netif = poom_web_get_sta_netif_();
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
 * @brief Returns the text representation for the current state.
 *
 * @param[in] out_ip Parameter passed to the function.
 * @param[in] out_ip_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_web_get_ap_ip_str_(char* out_ip, size_t out_ip_len)
{
    esp_netif_ip_info_t info;
    esp_netif_t* netif;
    ip4_addr_t ip;

    if((out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';

    netif = poom_web_get_ap_netif_();
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
 * @brief Internal helper for `poom_web_wait_sta_ip`.
 *
 * @param[in] out_ip Parameter passed to the function.
 * @param[in] out_ip_len Parameter passed to the function.
 * @param[in] timeout_ms Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_web_wait_sta_ip_(char* out_ip, size_t out_ip_len, uint32_t timeout_ms)
{
    uint32_t waited = 0U;
    esp_netif_t* netif;
    esp_netif_ip_info_t info;

    if((out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';
    netif = poom_web_get_sta_netif_();
    if(netif == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    while(waited < timeout_ms) {
        if(esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            if(info.ip.addr != 0U) {
                esp_err_t ip_err = poom_web_get_sta_ip_str_(out_ip, out_ip_len);
                return ip_err;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200U));
        waited += 200U;
    }

    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return void
 */
static void poom_web_cleanup_ap_init_failure_(void)
{
    teardown_mdns();
    (void)poom_wifi_ctrl_deinit();
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return void
 */
static void poom_web_cleanup_sta_init_failure_(void)
{
    teardown_mdns();
    (void)poom_wifi_ctrl_sta_disconnect();
    (void)poom_wifi_ctrl_deinit();
}

esp_err_t poom_web_init(void)
{
    esp_err_t err;

    if(s_poom_web_started) {
        return ESP_OK;
    }

    poom_web_log_heap_("init:entry");

    err = poom_wifi_ctrl_manager_ap_start(NULL);
    if(err != ESP_OK) {
        POOM_WEB_PRINTF_E(POOM_WEB_TAG, "Manager AP start failed: %s", esp_err_to_name(err));
        poom_web_log_heap_("init:ap_fail");
        return err;
    }
    poom_web_log_heap_("init:ap_ok");

    err = setup_mdns();
    if(err != ESP_OK) {
        POOM_WEB_PRINTF_W(POOM_WEB_TAG, "mDNS setup failed: %s", esp_err_to_name(err));
    }
    poom_web_log_heap_("init:mdns_done");

    err = poom_web_http_server_start();
    if(err != ESP_OK) {
        POOM_WEB_PRINTF_E(POOM_WEB_TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        poom_web_log_heap_("init:http_fail");
        poom_web_cleanup_ap_init_failure_();
        return err;
    }
    poom_web_log_heap_("init:http_ok");

    s_poom_web_started = true;
    s_poom_web_wifi_mode = POOM_WEB_WIFI_MODE_AP;
    if(poom_web_get_ap_ip_str_(s_poom_web_ap_ip, sizeof(s_poom_web_ap_ip)) != ESP_OK) {
        s_poom_web_ap_ip[0] = '\0';
    }
    s_poom_web_sta_ssid[0] = '\0';
    s_poom_web_sta_ip[0] = '\0';
    POOM_WEB_PRINTF_I(POOM_WEB_TAG, "POOM Web started at http://poom.local");
    poom_web_log_heap_("init:done");
    return ESP_OK;
}

esp_err_t poom_web_init_sta(const char* ssid,
                           const char* password,
                           char* out_ip,
                           size_t out_ip_len)
{
    esp_err_t err;
    esp_err_t stop_err;
    char ip_buf[16] = {0};
    char pass_buf[65] = {0};

    if((ssid == NULL) || (ssid[0] == '\0') || (out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';

    if(s_poom_web_started) {
        (void)poom_web_deinit();
    }

    (void)strncpy(s_poom_web_sta_ssid, ssid, sizeof(s_poom_web_sta_ssid) - 1U);
    s_poom_web_sta_ssid[sizeof(s_poom_web_sta_ssid) - 1U] = '\0';
    s_poom_web_sta_ip[0] = '\0';

    if((password != NULL) && (password[0] != '\0')) {
        (void)strncpy(pass_buf, password, sizeof(pass_buf) - 1U);
        pass_buf[sizeof(pass_buf) - 1U] = '\0';
    } else {
        pass_buf[0] = '\0';
    }

    for(uint32_t attempt = 0U; attempt < POOM_WEB_STA_CONNECT_ATTEMPTS; attempt++) {
        err = poom_wifi_ctrl_sta_connect(ssid, (pass_buf[0] != '\0') ? pass_buf : NULL);
        if(err != ESP_OK) {
            POOM_WEB_PRINTF_W(POOM_WEB_TAG, "STA connect start failed (attempt %u/%u): %s",
                                  (unsigned)(attempt + 1U),
                                  (unsigned)POOM_WEB_STA_CONNECT_ATTEMPTS,
                                  esp_err_to_name(err));
            (void)poom_wifi_ctrl_sta_disconnect();
            vTaskDelay(pdMS_TO_TICKS(250U));
            continue;
        }

        err = poom_web_wait_sta_ip_(ip_buf, sizeof(ip_buf), POOM_WEB_STA_CONNECT_TIMEOUT_MS);
        if(err == ESP_OK) {
            break;
        }

        POOM_WEB_PRINTF_W(POOM_WEB_TAG, "STA connect timeout (attempt %u/%u)",
                              (unsigned)(attempt + 1U),
                              (unsigned)POOM_WEB_STA_CONNECT_ATTEMPTS);
        (void)poom_wifi_ctrl_sta_disconnect();
        vTaskDelay(pdMS_TO_TICKS(250U));
    }

    if(err != ESP_OK) {
        poom_web_cleanup_sta_init_failure_();
        return err;
    }

    (void)strncpy(s_poom_web_sta_ip, ip_buf, sizeof(s_poom_web_sta_ip) - 1U);
    s_poom_web_sta_ip[sizeof(s_poom_web_sta_ip) - 1U] = '\0';
    (void)snprintf(out_ip, out_ip_len, "%s", s_poom_web_sta_ip);

    stop_err = poom_secrets_init();
    if(stop_err == ESP_OK) {
        (void)poom_secrets_set_wifi_ssid(ssid);
        (void)poom_secrets_set_wifi_pass(pass_buf);
    }

    err = setup_mdns();
    if(err != ESP_OK) {
        POOM_WEB_PRINTF_W(POOM_WEB_TAG, "mDNS setup failed: %s", esp_err_to_name(err));
    }

    err = poom_web_http_server_start();
    if(err != ESP_OK) {
        POOM_WEB_PRINTF_E(POOM_WEB_TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        poom_web_cleanup_sta_init_failure_();
        return err;
    }

    s_poom_web_started = true;
    s_poom_web_wifi_mode = POOM_WEB_WIFI_MODE_STA;
    POOM_WEB_PRINTF_I(POOM_WEB_TAG, "POOM Web started on STA: http://%s (poom.local if mDNS works)", s_poom_web_sta_ip);
    return ESP_OK;
}

esp_err_t poom_web_init_sta_saved(char* out_ip, size_t out_ip_len)
{
    esp_err_t err;
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    if((out_ip == NULL) || (out_ip_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_ip[0] = '\0';

    err = poom_secrets_init();
    if(err != ESP_OK) {
        return err;
    }

    err = poom_secrets_get_wifi_ssid(ssid, &ssid_len);
    if(err != ESP_OK) {
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    err = poom_secrets_get_wifi_pass(pass, &pass_len);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
        err = ESP_OK;
    }
    if(err != ESP_OK) {
        return err;
    }

    return poom_web_init_sta(ssid, pass, out_ip, out_ip_len);
}

esp_err_t poom_web_deinit(void)
{
    esp_err_t err = ESP_OK;
    esp_err_t stop_err;

    if(!s_poom_web_started) {
        return ESP_OK;
    }

    poom_web_log_heap_("deinit:entry");

    stop_err = poom_web_http_server_stop();
    if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
        POOM_WEB_PRINTF_W(POOM_WEB_TAG, "HTTP server stop warning: %s", esp_err_to_name(stop_err));
        err = stop_err;
    }
    poom_web_log_heap_("deinit:http_stopped");
    vTaskDelay(pdMS_TO_TICKS(100U));

    teardown_mdns();

    if(s_poom_web_sd_mounted_by_component) {
        stop_err = sd_card_unmount();
        if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
            POOM_WEB_PRINTF_W(POOM_WEB_TAG, "SD unmount warning: %s", esp_err_to_name(stop_err));
            if(err == ESP_OK) {
                err = stop_err;
            }
        }
        s_poom_web_sd_mounted_by_component = false;
    }

    if(s_poom_web_wifi_mode == POOM_WEB_WIFI_MODE_STA) {
        (void)poom_wifi_ctrl_sta_disconnect();
        stop_err = poom_wifi_ctrl_deinit();
        if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
            POOM_WEB_PRINTF_W(POOM_WEB_TAG, "WiFi deinit warning: %s", esp_err_to_name(stop_err));
            if(err == ESP_OK) {
                err = stop_err;
            }
        }
    } else {
        stop_err = poom_wifi_ctrl_deinit();
        if((stop_err != ESP_OK) && (stop_err != ESP_ERR_INVALID_STATE)) {
            POOM_WEB_PRINTF_W(POOM_WEB_TAG, "WiFi deinit warning: %s", esp_err_to_name(stop_err));
            if(err == ESP_OK) {
                err = stop_err;
            }
        }
    }

    s_poom_web_started = false;
    s_poom_web_wifi_mode = POOM_WEB_WIFI_MODE_NONE;
    s_poom_web_ap_ip[0] = '\0';
    s_poom_web_sta_ssid[0] = '\0';
    s_poom_web_sta_ip[0] = '\0';
    POOM_WEB_PRINTF_I(POOM_WEB_TAG, "POOM Web stopped");
    poom_web_log_heap_("deinit:done");
    return err;
}

esp_err_t poom_web_set_command_cb(poom_web_command_cb_t cb, void* user_ctx)
{
    return poom_web_http_server_set_command_cb(cb, user_ctx);
}

esp_err_t poom_web_send_text(const char* text)
{
    if(!s_poom_web_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return poom_web_http_server_send_text(text);
}

const char* poom_web_get_wifi_ap_ssid(void)
{
    return POOM_WIFI_CTRL_MANAGER_AP_SSID;
}

const char* poom_web_get_wifi_ap_password(void)
{
    return POOM_WIFI_CTRL_MANAGER_AP_PASSWORD;
}

const char* poom_web_get_wifi_ap_ip(void)
{
    if(poom_web_get_ap_ip_str_(s_poom_web_ap_ip, sizeof(s_poom_web_ap_ip)) != ESP_OK) {
        s_poom_web_ap_ip[0] = '\0';
    }
    return s_poom_web_ap_ip;
}

const char* poom_web_get_wifi_sta_ssid(void)
{
    return s_poom_web_sta_ssid;
}

const char* poom_web_get_wifi_sta_ip(void)
{
    return s_poom_web_sta_ip;
}
