// SPDX-License-Identifier: MIT
#include "poom_wifi_observer.h"
#include "poom_wifi_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include <string.h>

#define RX_SLOTS 10U
#define RX_BYTES 1536U
typedef struct {
    uint16_t length;
    uint8_t channel;
    int8_t rssi;
    uint32_t time_ms;
    uint8_t frame[RX_BYTES];
} observation_t;
struct poom_wifi_observer {
    poom_observer_model_t model;
    observation_t rx[RX_SLOTS], scratch;
    uint32_t dropped, tune_errors, last_hop;
    uint8_t head, tail, count, channel, hop_index;
    bool focused, radio_ready, closing;
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static poom_wifi_observer_t *s_receiver;
static bool s_claimed;
static const uint8_t s_channels[] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13,
#if CONFIG_IDF_TARGET_ESP32C5
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
    116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
#endif
};
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void saturate(uint32_t *value) { if(*value != UINT32_MAX) ++*value; }

static void receive(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if(!buffer || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
    const wifi_promiscuous_pkt_t *packet = buffer;
    if(packet->rx_ctrl.rx_state || packet->rx_ctrl.sig_len < 4U) return;
    size_t length = packet->rx_ctrl.sig_len - 4U; /* IDF sig_len includes FCS. */
    if(type == WIFI_PKT_DATA && length > 80U) length = 80U; /* Headers + LLC/EAPOL only. */
    const uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    poom_wifi_observer_t *s = s_receiver;
    if(s) {
        if(length > RX_BYTES || s->count == RX_SLOTS) saturate(&s->dropped);
        else {
            observation_t *item = &s->rx[s->head];
            item->length = (uint16_t)length; item->channel = packet->rx_ctrl.channel;
            item->rssi = packet->rx_ctrl.rssi; item->time_ms = now;
            memcpy(item->frame, packet->payload, length);
            s->head = (s->head + 1U) % RX_SLOTS; ++s->count;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t poom_wifi_observer_focus(poom_wifi_observer_t *s, uint8_t channel)
{
    if(!s || !s->radio_ready || s->closing) return ESP_ERR_INVALID_STATE;
    if(!channel) { s->focused = false; return ESP_OK; }
    const esp_err_t error = poom_wifi_ctrl_set_channel(channel);
    if(error != ESP_OK) { saturate(&s->tune_errors); return error; }
    s->channel = channel; s->focused = true; s->last_hop = now_ms(); return ESP_OK;
}
static esp_err_t hop(poom_wifi_observer_t *s)
{
    esp_err_t error = ESP_FAIL;
    for(size_t i = 0; i < sizeof(s_channels); ++i) {
        const uint8_t channel = s_channels[s->hop_index];
        s->hop_index = (s->hop_index + 1U) % sizeof(s_channels);
        error = poom_wifi_ctrl_set_channel(channel);
        if(error == ESP_OK) { s->channel = channel; break; }
        saturate(&s->tune_errors);
    }
    s->last_hop = now_ms(); return error;
}
esp_err_t poom_wifi_observer_start(poom_wifi_observer_t **out)
{
    if(!out || *out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_claimed;
    if(!busy) s_claimed = true;
    portEXIT_CRITICAL(&s_lock);
    if(busy) return ESP_ERR_INVALID_STATE;
    bool promiscuous = false;
    (void)esp_wifi_get_promiscuous(&promiscuous);
    if(promiscuous || poom_wifi_ctrl_sta_is_connected()) {
        portENTER_CRITICAL(&s_lock); s_claimed = false; portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    poom_wifi_observer_t *s = heap_caps_calloc(1U, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s) {
        portENTER_CRITICAL(&s_lock); s_claimed = false; portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    *out = s;
    poom_observer_model_init(&s->model, now_ms());
    s->radio_ready = true; /* Also unwind a partially completed controller init. */
    esp_err_t error = poom_wifi_ctrl_init_null();
    if(error != ESP_OK) { poom_wifi_observer_stop(out); return error; }
    const wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    error = esp_wifi_set_promiscuous_filter(&filter);
    if(error == ESP_OK) error = poom_wifi_ctrl_set_promiscuous_rx_cb(receive);
    portENTER_CRITICAL(&s_lock); s_receiver = s; portEXIT_CRITICAL(&s_lock);
    if(error == ESP_OK) error = poom_wifi_ctrl_set_promiscuous(true);
    if(error == ESP_OK) error = hop(s);
    if(error != ESP_OK) poom_wifi_observer_stop(out);
    return error;
}
void poom_wifi_observer_poll(poom_wifi_observer_t *s)
{
    if(!s || s->closing) return;
    for(size_t i = 0; i < RX_SLOTS; ++i) {
        portENTER_CRITICAL(&s_lock);
        const bool ready = s->count != 0U;
        if(ready) {
            s->scratch = s->rx[s->tail]; s->tail = (s->tail + 1U) % RX_SLOTS; --s->count;
        }
        portEXIT_CRITICAL(&s_lock);
        if(!ready) break;
        const observation_t *item = &s->scratch;
        (void)poom_observer_model_frame(&s->model, item->frame, item->length, item->channel, item->rssi, item->time_ms);
    }
    const uint32_t now = now_ms();
    poom_observer_model_tick(&s->model, now);
    if(!s->focused && now - s->last_hop >= 250U) (void)hop(s);
}
const poom_observer_model_t *poom_wifi_observer_model(const poom_wifi_observer_t *s) { return s ? &s->model : NULL; }
uint8_t poom_wifi_observer_channel(const poom_wifi_observer_t *s) { return s ? s->channel : 0U; }
uint32_t poom_wifi_observer_dropped(const poom_wifi_observer_t *s) {
    portENTER_CRITICAL(&s_lock); const uint32_t dropped = s ? s->dropped : 0U; portEXIT_CRITICAL(&s_lock); return dropped;
}
uint32_t poom_wifi_observer_tune_errors(const poom_wifi_observer_t *s) { return s ? s->tune_errors : 0U; }
static bool stopped(esp_err_t error) {
    return error==ESP_OK || error==ESP_ERR_WIFI_NOT_INIT || error==ESP_ERR_WIFI_NOT_STARTED;
}
esp_err_t poom_wifi_observer_stop(poom_wifi_observer_t **session)
{
    if(!session || !*session) return ESP_OK;
    poom_wifi_observer_t *s = *session;
    s->closing=true;
    /* The same lock encloses every callback access: no in-flight reader survives this detach. */
    portENTER_CRITICAL(&s_lock); if(s_receiver == s) s_receiver = NULL; portEXIT_CRITICAL(&s_lock);
    if(s->radio_ready) {
        const esp_err_t disable=poom_wifi_ctrl_set_promiscuous(false);
        const esp_err_t detach=poom_wifi_ctrl_set_promiscuous_rx_cb(NULL);
        const esp_err_t deinit=poom_wifi_ctrl_deinit();
        if(!stopped(disable))return disable;
        if(!stopped(detach))return detach;
        if(!stopped(deinit))return deinit;
    }
    *session = NULL; free(s);
    portENTER_CRITICAL(&s_lock); s_claimed = false; portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
