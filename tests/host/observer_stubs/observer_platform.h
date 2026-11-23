// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_IDF_TARGET_ESP32C5 1
#define CONFIG_ESP_WIFI_CSI_ENABLED 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int portMUX_TYPE;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define WIFI_PROMIS_FILTER_MASK_MGMT 1
#define WIFI_PROMIS_FILTER_MASK_DATA 2
#define ESP_CSI_ACQUIRE_STBC_HELTF1 0
typedef enum { WIFI_PKT_MGMT, WIFI_PKT_DATA, WIFI_PKT_CTRL } wifi_promiscuous_pkt_type_t;
typedef struct {unsigned rx_state, sig_len;uint8_t channel;int8_t rssi;} wifi_pkt_rx_ctrl_t;
typedef struct {wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t payload[1800];} wifi_promiscuous_pkt_t;
typedef void (*wifi_promiscuous_cb_t)(void *,wifi_promiscuous_pkt_type_t);
typedef struct {unsigned filter_mask;} wifi_promiscuous_filter_t;
typedef struct {wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t mac[6];bool first_word_invalid;int8_t *buf;uint16_t len;} wifi_csi_info_t;
typedef void (*wifi_csi_cb_t)(void *,wifi_csi_info_t *);
typedef struct {unsigned enable,acquire_csi_legacy,acquire_csi_force_lltf,acquire_csi_ht20,acquire_csi_ht40,acquire_csi_vht,acquire_csi_su,acquire_csi_mu,acquire_csi_dcm,acquire_csi_beamformed,acquire_csi_he_stbc_mode,lltf_bit_mode;} wifi_csi_config_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_wifi_get_promiscuous(bool *);
esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *);
esp_err_t poom_wifi_ctrl_init_null(void);
esp_err_t poom_wifi_ctrl_init_sta(void);
esp_err_t poom_wifi_ctrl_deinit(void);
esp_err_t poom_wifi_ctrl_set_channel(uint8_t);
esp_err_t poom_wifi_ctrl_set_promiscuous(bool);
esp_err_t poom_wifi_ctrl_set_promiscuous_rx_cb(wifi_promiscuous_cb_t);
bool poom_wifi_ctrl_sta_is_connected(void);
bool poom_wifi_ctrl_channel_is_valid(uint8_t);
void *heap_caps_calloc(size_t,size_t,unsigned);
int64_t esp_timer_get_time(void);
esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t *);
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t,void *);
esp_err_t esp_wifi_set_csi(bool);
#ifdef __cplusplus
}
#endif
