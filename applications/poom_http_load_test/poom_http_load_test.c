// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "poom_wifi_ctrl.h"

#include "poom_http_load_test.h"


#if POOM_HTTP_LOAD_TEST_ENABLE_LOG
    static const char *POOM_HTTP_LOAD_TEST_TAG = "poom_http_load_test";

    #define POOM_HTTP_LOAD_TEST_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_HTTP_LOAD_TEST_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_HTTP_LOAD_TEST_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_HTTP_LOAD_TEST_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_HTTP_LOAD_TEST_DEBUG_LOG_ENABLED
        #define POOM_HTTP_LOAD_TEST_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_HTTP_LOAD_TEST_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_HTTP_LOAD_TEST_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_HTTP_LOAD_TEST_PRINTF_E(...) do { } while (0)
    #define POOM_HTTP_LOAD_TEST_PRINTF_I(...) do { } while (0)
    #define POOM_HTTP_LOAD_TEST_PRINTF_D(...) do { } while (0)
#endif

#define POOM_HTTP_LOAD_TEST_HOST_MAX_LEN 128
#define POOM_HTTP_LOAD_TEST_PORT_MAX_LEN 8
#define POOM_HTTP_LOAD_TEST_PATH_MAX_LEN 128
#define POOM_HTTP_LOAD_TEST_REQUEST_MAX_LEN 320
#define POOM_HTTP_LOAD_TEST_DEFAULT_WORKERS 8
#define POOM_HTTP_LOAD_TEST_MAX_WORKERS 16

static volatile bool s_poom_http_load_test_running = false;
static uint8_t s_poom_http_load_test_worker_count = POOM_HTTP_LOAD_TEST_DEFAULT_WORKERS;
static char s_poom_http_load_test_host[POOM_HTTP_LOAD_TEST_HOST_MAX_LEN];
static char s_poom_http_load_test_port[POOM_HTTP_LOAD_TEST_PORT_MAX_LEN];
static char s_poom_http_load_test_path[POOM_HTTP_LOAD_TEST_PATH_MAX_LEN];

/**
 * @brief Validate and copy active HTTP target into internal buffers.
 *
 * @param[in] host Hostname or IP. Must not be NULL.
 * @param[in] port TCP port as string. Must not be NULL.
 * @param[in] path HTTP path. Must not be NULL.
 * @return esp_err_t
 */
static esp_err_t poom_http_load_test_internal_set_target_(const char *host,
                                                           const char *port,
                                                           const char *path)
{
    size_t host_len;
    size_t port_len;
    size_t path_len;

    if(host == NULL || port == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    host_len = strlen(host);
    port_len = strlen(port);
    path_len = strlen(path);

    if(host_len == 0U || host_len >= sizeof(s_poom_http_load_test_host)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(port_len == 0U || port_len >= sizeof(s_poom_http_load_test_port)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(path_len == 0U || path_len >= sizeof(s_poom_http_load_test_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_poom_http_load_test_host, host, host_len + 1U);
    memcpy(s_poom_http_load_test_port, port, port_len + 1U);
    memcpy(s_poom_http_load_test_path, path, path_len + 1U);

    return ESP_OK;
}

/**
 * @brief Worker thread that performs repeated HTTP GET requests.
 *
 * @param[in] pv_parameters Unused thread argument.
 * @return void*
 */
static void *poom_http_load_test_internal_http_get_task_(void *pv_parameters)
{
    char request[POOM_HTTP_LOAD_TEST_REQUEST_MAX_LEN];
    struct addrinfo *result = NULL;
    int socket_fd;
    int status;

    (void)pv_parameters;

    snprintf(request,
             sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             s_poom_http_load_test_path,
             s_poom_http_load_test_host);

    while(s_poom_http_load_test_running) {
        const struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };

        status = getaddrinfo(s_poom_http_load_test_host,
                             s_poom_http_load_test_port,
                             &hints,
                             &result);
        if(status != 0 || result == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        socket_fd = socket(result->ai_family, result->ai_socktype, 0);
        if(socket_fd < 0) {
            freeaddrinfo(result);
            result = NULL;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if(connect(socket_fd, result->ai_addr, result->ai_addrlen) != 0) {
            close(socket_fd);
            freeaddrinfo(result);
            result = NULL;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        freeaddrinfo(result);
        result = NULL;

        write(socket_fd, request, strlen(request));
        close(socket_fd);
        vTaskDelay(1);
    }

    return NULL;
}

/**
 * @brief Spawn detached worker threads for load generation.
 *
 * @return void
 */
static void poom_http_load_test_internal_spawn_workers_(void)
{
    pthread_attr_t attr;
    uint8_t worker_index;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);

    for(worker_index = 0; worker_index < s_poom_http_load_test_worker_count; worker_index++) {
        pthread_t thread;
        pthread_create(&thread, &attr, poom_http_load_test_internal_http_get_task_, NULL);
        pthread_detach(thread);
    }

    pthread_attr_destroy(&attr);
}

/**
 * @brief Wi-Fi connected callback that starts worker threads.
 *
 * @return void
 */
static void poom_http_load_test_internal_on_wifi_connected_(void)
{
    if(s_poom_http_load_test_running) {
        POOM_HTTP_LOAD_TEST_PRINTF_D("workers already running");
        return;
    }

    s_poom_http_load_test_running = true;
    POOM_HTTP_LOAD_TEST_PRINTF_I("start target: http://%s:%s%s",
                                 s_poom_http_load_test_host,
                                 s_poom_http_load_test_port,
                                 s_poom_http_load_test_path);
    poom_http_load_test_internal_spawn_workers_();
}

/**
 * @brief Wi-Fi disconnected callback that stops worker threads.
 *
 * @return void
 */
static void poom_http_load_test_internal_on_wifi_disconnected_(void)
{
    s_poom_http_load_test_running = false;
    POOM_HTTP_LOAD_TEST_PRINTF_I("wifi disconnected, workers stopped");
}

/**
 * @brief Handle Wi-Fi control events from poom_wifi_ctrl.
 *
 * @param[in] info Event information. Must not be NULL.
 * @param[in] user_ctx User context pointer.
 * @return void
 */
static void poom_http_load_test_internal_wifi_ctrl_cb_(const poom_wifi_ctrl_evt_info_t *info,
                                                       void *user_ctx)
{
    (void)user_ctx;

    if(info == NULL) {
        return;
    }

    if(info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP) {
        poom_http_load_test_internal_on_wifi_connected_();
    } else if(info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED) {
        poom_http_load_test_internal_on_wifi_disconnected_();
    } else {
        POOM_HTTP_LOAD_TEST_PRINTF_D("wifi event ignored: %d", (int)info->evt);
    }
}

esp_err_t poom_http_load_test_start(const poom_http_load_test_config_t *config)
{
    esp_err_t status;

    if(config == NULL || config->ssid == NULL || config->password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    status = poom_http_load_test_internal_set_target_(config->host,
                                                      config->port,
                                                      config->path);
    if(status != ESP_OK) {
        return status;
    }

    if(config->worker_count == 0U) {
        s_poom_http_load_test_worker_count = POOM_HTTP_LOAD_TEST_DEFAULT_WORKERS;
    } else if(config->worker_count > POOM_HTTP_LOAD_TEST_MAX_WORKERS) {
        s_poom_http_load_test_worker_count = POOM_HTTP_LOAD_TEST_MAX_WORKERS;
    } else {
        s_poom_http_load_test_worker_count = config->worker_count;
    }

    status = poom_wifi_ctrl_register_cb(poom_http_load_test_internal_wifi_ctrl_cb_, NULL);
    if(status != ESP_OK) {
        POOM_HTTP_LOAD_TEST_PRINTF_E("poom_wifi_ctrl_register_cb failed: %d", (int)status);
        return status;
    }

    status = poom_wifi_ctrl_sta_connect(config->ssid, config->password);
    if(status != ESP_OK) {
        (void)poom_wifi_ctrl_unregister_cb();
        POOM_HTTP_LOAD_TEST_PRINTF_E("poom_wifi_ctrl_sta_connect failed: %d", (int)status);
        return status;
    }

    return ESP_OK;
}

esp_err_t poom_http_load_test_switch_target(const char *host,
                                            const char *port,
                                            const char *path)
{
    esp_err_t status = poom_http_load_test_internal_set_target_(host,
                                                                 port,
                                                                 path);
    if(status != ESP_OK) {
        return status;
    }

    if(s_poom_http_load_test_running) {
        s_poom_http_load_test_running = false;
        vTaskDelay(pdMS_TO_TICKS(50));
        s_poom_http_load_test_running = true;
        poom_http_load_test_internal_spawn_workers_();
    }

    POOM_HTTP_LOAD_TEST_PRINTF_I("switch target: http://%s:%s%s",
                                 s_poom_http_load_test_host,
                                 s_poom_http_load_test_port,
                                 s_poom_http_load_test_path);

    return ESP_OK;
}

void poom_http_load_test_stop(void)
{
    esp_err_t status;

    s_poom_http_load_test_running = false;
    status = poom_wifi_ctrl_sta_disconnect();
    if(status != ESP_OK) {
        POOM_HTTP_LOAD_TEST_PRINTF_E("poom_wifi_ctrl_sta_disconnect failed: %d", (int)status);
    }
    status = poom_wifi_ctrl_unregister_cb();
    if(status != ESP_OK) {
        POOM_HTTP_LOAD_TEST_PRINTF_E("poom_wifi_ctrl_unregister_cb failed: %d", (int)status);
    }
    POOM_HTTP_LOAD_TEST_PRINTF_I("workers stopped");
}
