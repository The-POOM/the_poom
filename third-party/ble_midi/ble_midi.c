/*
 * BLE MIDI Driver
 *
 * See README.md for usage hints
 *
 * =============================================================================
 *
 * MIT License
 *
 * Copyright (c) 2019 Thorsten Klose (tk@midibox.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * =============================================================================
 */

#include "../include/ble_midi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_bt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"

// -----------------------------------------------------------------------------
// Defaults (por si no están en el header)
// -----------------------------------------------------------------------------
#ifndef BLEMIDI_NUM_PORTS
#define BLEMIDI_NUM_PORTS 1
#endif

#ifndef BLEMIDI_OUTBUFFER_FLUSH_MS
#define BLEMIDI_OUTBUFFER_FLUSH_MS 10
#endif

#ifndef BLEMIDI_TAG
#define BLEMIDI_TAG "BLEMIDI"
#endif

#ifndef BLEMIDI_DEVICE_NAME
#define BLEMIDI_DEVICE_NAME "ESP32-BLE-MIDI"
#endif
// -----------------------------------------------------------------------------

/* =========================
 * Local log macros (printf)
 * ========================= */

#if BLEMIDI_LOG_ENABLED
  static const char *BLEMIDI_LOG_TAG = BLEMIDI_TAG;

  #define BLEMIDI_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", BLEMIDI_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

  #define BLEMIDI_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", BLEMIDI_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

  #define BLEMIDI_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", BLEMIDI_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)

  #if BLEMIDI_DEBUG_LOG_ENABLED
    #define BLEMIDI_PRINTF_D(fmt, ...) \
      printf("[D] [%s] %s:%d: " fmt "\n", BLEMIDI_LOG_TAG, __func__, __LINE__, ##__VA_ARGS__)
  #else
    #define BLEMIDI_PRINTF_D(...) do { } while (0)
  #endif

#else

  #define BLEMIDI_PRINTF_E(...) do { } while (0)
  #define BLEMIDI_PRINTF_W(...) do { } while (0)
  #define BLEMIDI_PRINTF_I(...) do { } while (0)
  #define BLEMIDI_PRINTF_D(...) do { } while (0)

#endif

#define PROFILE_NUM 1
#define PROFILE_APP_IDX 0
#define ESP_APP_ID 0x55
#define SVC_INST_ID 0

/* The max length of characteristic value. When the GATT client performs a write
 * or prepare write operation, the data length must be less than
 * GATTS_MIDI_CHAR_VAL_LEN_MAX.
 */
#define GATTS_MIDI_CHAR_VAL_LEN_MAX 100
#define PREPARE_BUF_MAX_SIZE 2048
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))

#define ADV_CONFIG_FLAG (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

static uint8_t adv_config_done = 0;

// the MTU can be changed by the client during runtime
static size_t blemidi_mtu = GATTS_MIDI_CHAR_VAL_LEN_MAX - 3;

// we buffer outgoing MIDI messages for ~10 ms
static uint8_t blemidi_outbuffer[BLEMIDI_NUM_PORTS][GATTS_MIDI_CHAR_VAL_LEN_MAX];
static uint32_t blemidi_outbuffer_len[BLEMIDI_NUM_PORTS];

// to handled continued SysEx
static size_t blemidi_continued_sysex_pos[BLEMIDI_NUM_PORTS];

/* Timer de flush periódico */
static esp_timer_handle_t s_flush_timer = NULL;

/* Attributes State Machine */
enum {
  IDX_SVC,
  IDX_CHAR_A,
  IDX_CHAR_VAL_A,
  IDX_CHAR_CFG_A,

  HRS_IDX_NB,
};
static uint16_t midi_handle_table[HRS_IDX_NB];

typedef struct {
  uint8_t *prepare_buf;
  int prepare_len;
} prepare_type_env_t;

static prepare_type_env_t prepare_write_env;

static uint8_t midi_service_uuid[16] = {
    /* LSB                                                      MSB */
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03};

static const uint8_t midi_characteristics_uuid[16] = {
    /* LSB                                                      MSB */
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77};

/* The length of adv data must be less than 31 bytes */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(midi_service_uuid),
    .p_service_uuid = midi_service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(midi_service_uuid),
    .p_service_uuid = midi_service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst {
  esp_gatts_cb_t gatts_cb;
  uint16_t gatts_if;
  uint16_t app_id;
  uint16_t conn_id;
  uint16_t service_handle;
  esp_gatt_srvc_id_t service_id;
  uint16_t char_handle;
  esp_bt_uuid_t char_uuid;
  esp_gatt_perm_t perm;
  esp_gatt_char_prop_t property;
  uint16_t descr_handle;
  esp_bt_uuid_t descr_uuid;
};

static struct gatts_profile_inst midi_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = NULL, // se setea más abajo
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

int32_t blemidi_outbuffer_flush(uint8_t blemidi_port);

/* Protos */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param);
static void blemidi_prepare_write_event_env(esp_gatt_if_t gatts_if,
                                            prepare_type_env_t *prepare_write_env,
                                            esp_ble_gatts_cb_param_t *param);
static void blemidi_exec_write_event_env(prepare_type_env_t *prepare_write_env,
                                         esp_ble_gatts_cb_param_t *param);

/* Callback de Usuario */
void (*blemidi_callback_midi_message_received)(
    uint8_t blemidi_port, uint16_t timestamp, uint8_t midi_status,
    uint8_t *remaining_message, size_t len, size_t continued_sysex_pos);

    /* Helpers de tiempo/timestamps */
    static uint32_t get_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
    static uint8_t blemidi_timestamp_high(uint32_t ts) { return (0x80 | ((ts >> 7) & 0x3f)); }
    static uint8_t blemidi_timestamp_low (uint32_t ts) { return (0x80 | ( ts       & 0x7f)); }
    static void blemidi_log_buffer_hex(const uint8_t *data, size_t len) {
      if ((data == NULL) || (len == 0)) {
        BLEMIDI_PRINTF_D("HEX dump empty");
        return;
      }

      for (size_t i = 0; i < len; i += 16) {
        char line[16 * 3 + 1] = {0};
        size_t off = 0;
        size_t chunk = (len - i > 16) ? 16 : (len - i);
        for (size_t j = 0; j < chunk; ++j) {
          off += (size_t)snprintf(&line[off], sizeof(line) - off, "%02X ", data[i + j]);
          if (off >= sizeof(line)) break;
        }
        BLEMIDI_PRINTF_D("HEX[%u]: %s", (unsigned)i, line);
      }
    }

    /* Flush periódico con esp_timer */
    static void flush_timer_cb(void *arg) {
      blemidi_outbuffer_flush(0);
    }

    /* Base GATT - DB */
    static const uint16_t primary_service_uuid             = ESP_GATT_UUID_PRI_SERVICE;
    static const uint16_t character_declaration_uuid       = ESP_GATT_UUID_CHAR_DECLARE;
    static const uint16_t character_client_config_uuid     = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    static const uint8_t  char_prop_read_write_writenr_notify =
        ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ |
        ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

    static const uint8_t char_value[3] = {0x80, 0x80, 0xfe};
    static const uint8_t blemidi_ccc[2] = {0x00, 0x00};

/* Full GATT DB */
static const esp_gatts_attr_db_t gatt_db[HRS_IDX_NB] = {
    [IDX_SVC] = {{ESP_GATT_AUTO_RSP},
                 {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid,
                  ESP_GATT_PERM_READ, 16, sizeof(midi_service_uuid),
                  (uint8_t *)&midi_service_uuid}},

    [IDX_CHAR_A] = {{ESP_GATT_AUTO_RSP},
                    {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                     ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE,
                     CHAR_DECLARATION_SIZE,
                     (uint8_t *)&char_prop_read_write_writenr_notify}},

    [IDX_CHAR_VAL_A] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_128,
                         (uint8_t *)&midi_characteristics_uuid,
                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                         GATTS_MIDI_CHAR_VAL_LEN_MAX, sizeof(char_value),
                         (uint8_t *)char_value}},

    [IDX_CHAR_CFG_A] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16,
                         (uint8_t *)&character_client_config_uuid,
                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                         sizeof(uint16_t), sizeof(blemidi_ccc),
                         (uint8_t *)blemidi_ccc}},
};

/* GAP */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
      adv_config_done &= (~ADV_CONFIG_FLAG);
      if (adv_config_done == 0) {
        esp_ble_gap_start_advertising(&adv_params);
      }
      break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
      adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
      if (adv_config_done == 0) {
        esp_ble_gap_start_advertising(&adv_params);
      }
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        BLEMIDI_PRINTF_E("advertising start failed");
      } else {
        BLEMIDI_PRINTF_I("advertising start ok");
      }
      break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        BLEMIDI_PRINTF_E("advertising stop failed");
      } else {
        BLEMIDI_PRINTF_I("advertising stopped");
      }
      break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      BLEMIDI_PRINTF_I(
          "conn params: status=%d, min_int=%d, max_int=%d, conn_int=%d, latency=%d, timeout=%d",
          param->update_conn_params.status,
          param->update_conn_params.min_int,
          param->update_conn_params.max_int,
          param->update_conn_params.conn_int,
          param->update_conn_params.latency,
          param->update_conn_params.timeout);
      break;
    default:
      break;
  }
}

/* Flush de buffer (desde tarea/timer) */
int32_t blemidi_outbuffer_flush(uint8_t blemidi_port) {
  if (blemidi_outbuffer_len[blemidi_port] > 0) {
    esp_ble_gatts_send_indicate(midi_profile_tab[PROFILE_APP_IDX].gatts_if,
                                midi_profile_tab[PROFILE_APP_IDX].conn_id,
                                midi_handle_table[IDX_CHAR_VAL_A],
                                blemidi_outbuffer_len[blemidi_port],
                                blemidi_outbuffer[blemidi_port], false);
    blemidi_outbuffer_len[blemidi_port] = 0;
  }
  return 0;
}

/* Push en buffer TX */
static int32_t blemidi_outbuffer_push(uint8_t blemidi_port,
                                      uint8_t *stream, size_t len) {
  const uint32_t timestamp = get_ms();

  if (blemidi_outbuffer_len[blemidi_port] == 0) {
    blemidi_outbuffer[blemidi_port][blemidi_outbuffer_len[blemidi_port]++] =
        blemidi_timestamp_high(timestamp);
    if (stream[0] >= 0x80) {
      blemidi_outbuffer[blemidi_port][blemidi_outbuffer_len[blemidi_port]++] =
          blemidi_timestamp_low(timestamp);
    }
  } else {
    blemidi_outbuffer[blemidi_port][blemidi_outbuffer_len[blemidi_port]++] =
        blemidi_timestamp_low(timestamp);
  }

  memcpy(&blemidi_outbuffer[blemidi_port][blemidi_outbuffer_len[blemidi_port]],
         stream, len);
  blemidi_outbuffer_len[blemidi_port] += len;

  return 0;
}

/* API pública de envío */
int32_t blemidi_send_message(uint8_t blemidi_port, uint8_t *stream, size_t len) {
  const size_t max_header_size = 2;
  if (blemidi_port >= BLEMIDI_NUM_PORTS) return -1;

  if (len < (blemidi_mtu - max_header_size)) {
    blemidi_outbuffer_push(blemidi_port, stream, len);
  } else {
    BLEMIDI_PRINTF_W("MTU full");
  }
  return 0;
}

/* (Opcional) RX parser interno si habilitas escritura del cliente */
static __attribute__((unused)) int32_t blemidi_receive_packet(
    uint8_t blemidi_port, uint8_t *stream, size_t len,
    void *_callback_midi_message_received) {
  void (*cb)(uint8_t, uint16_t, uint8_t, uint8_t*, size_t, size_t) =
      _callback_midi_message_received;

  if (blemidi_port >= BLEMIDI_NUM_PORTS) return -1;

  // detect continued SysEx
  uint8_t continued_sysex = 0;
  if (len > 2 && (stream[0] & 0x80) && !(stream[1] & 0x80)) {
    continued_sysex = 1;
  } else {
    blemidi_continued_sysex_pos[blemidi_port] = 0;
  }

  if (len < 3) return -1;
  if (!(stream[0] & 0x80)) return -2;

  size_t pos = 0;
  uint16_t timestamp = (stream[pos++] & 0x3f) << 7;

  const uint8_t midi_expected_bytes_common[8] = {2,2,2,2,1,1,2,0};
  const uint8_t midi_expected_bytes_system[16]= {1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0};

  uint8_t midi_status = continued_sysex ? 0xf0 : 0x00;

  while (pos < len) {
    if (!(stream[pos] & 0x80)) {
      if (!continued_sysex) return -3;
    } else {
      timestamp &= ~0x7f;
      timestamp |= stream[pos++] & 0x7f;
      continued_sysex = 0;
      blemidi_continued_sysex_pos[blemidi_port] = 0;
    }

    if (stream[pos] & 0x80) {
      midi_status = stream[pos++];
    }

    if (midi_status == 0xf0) {
      size_t n;
      for (n = 0; (pos + n) < len && stream[pos + n] < 0x80; ++n) { /* scan */ }
      if (cb) cb(blemidi_port, timestamp, midi_status, &stream[pos], n,
                 blemidi_continued_sysex_pos[blemidi_port]);
      pos += n;
      blemidi_continued_sysex_pos[blemidi_port] += n;
    } else {
      uint8_t n = midi_expected_bytes_common[(midi_status >> 4) & 0x7];
      if (n == 0) n = midi_expected_bytes_system[midi_status & 0xf];
      if ((pos + n) > len) return -5;
      if (cb) cb(blemidi_port, timestamp, midi_status, &stream[pos], n, 0);
      pos += n;
    }
  }

  return 0;
}

/* Prepare/Exec Write helpers (para GATTS prepare write flow) */
static void blemidi_prepare_write_event_env(esp_gatt_if_t gatts_if,
                                            prepare_type_env_t *env,
                                            esp_ble_gatts_cb_param_t *param) {
  esp_gatt_status_t status = ESP_GATT_OK;
  if (env->prepare_buf == NULL) {
    env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE);
    env->prepare_len = 0;
    if (env->prepare_buf == NULL) {
      BLEMIDI_PRINTF_E("%s no mem", __func__);
      status = ESP_GATT_NO_RESOURCES;
    }
  } else {
    if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
      status = ESP_GATT_INVALID_OFFSET;
    } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
      status = ESP_GATT_INVALID_ATTR_LEN;
    }
  }

  if (param->write.need_rsp) {
    esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
    if (gatt_rsp != NULL) {
      gatt_rsp->attr_value.len     = param->write.len;
      gatt_rsp->attr_value.handle  = param->write.handle;
      gatt_rsp->attr_value.offset  = param->write.offset;
      gatt_rsp->attr_value.auth_req= ESP_GATT_AUTH_REQ_NONE;
      memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
      esp_err_t rsp_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                      param->write.trans_id, status, gatt_rsp);
      if (rsp_err != ESP_OK) {
        BLEMIDI_PRINTF_E("Send response error");
      }
      free(gatt_rsp);
    } else {
      BLEMIDI_PRINTF_E("%s, malloc failed", __func__);
    }
  }
  if (status != ESP_GATT_OK) {
    return;
  }

  memcpy(env->prepare_buf + param->write.offset, param->write.value, param->write.len);
  env->prepare_len += param->write.len;
}

static void blemidi_exec_write_event_env(prepare_type_env_t *env,
                                         esp_ble_gatts_cb_param_t *param) {
  if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC &&
      env->prepare_buf) {
    // Aquí podrías parsear lo recibido si quisieras soportar RX por write long
    blemidi_log_buffer_hex(env->prepare_buf, env->prepare_len);
  } else {
    BLEMIDI_PRINTF_D("ESP_GATT_PREP_WRITE_CANCEL");
  }

  if (env->prepare_buf) {
    free(env->prepare_buf);
    env->prepare_buf = NULL;
  }
  env->prepare_len = 0;
}

/* GATTS profile */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_REG_EVT: {
      esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(BLEMIDI_DEVICE_NAME);
      if (set_dev_name_ret) {
        BLEMIDI_PRINTF_E("set device name failed: %x", set_dev_name_ret);
      }

      esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
      if (ret) {
        BLEMIDI_PRINTF_E("adv data failed: %x", ret);
      }
      adv_config_done |= ADV_CONFIG_FLAG;

      ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
      if (ret) {
        BLEMIDI_PRINTF_E("scan rsp data failed: %x", ret);
      }
      adv_config_done |= SCAN_RSP_CONFIG_FLAG;

      esp_err_t create_attr_ret =
          esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
      if (create_attr_ret) {
        BLEMIDI_PRINTF_E("attr table failed: %x", create_attr_ret);
      }
    } break;

    case ESP_GATTS_READ_EVT:
      BLEMIDI_PRINTF_D("READ_EVT");
      break;

    case ESP_GATTS_WRITE_EVT:
      if (param->write.is_prep) {
        blemidi_prepare_write_event_env(gatts_if, &prepare_write_env, param);
      } else {
        if (midi_handle_table[IDX_CHAR_VAL_A] == param->write.handle) {
          // Si quisieras soportar RX desde el cliente:
          // blemidi_receive_packet(0, param->write.value, param->write.len,
          //                        blemidi_callback_midi_message_received);
        }
        if (param->write.need_rsp) {
          esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                      param->write.trans_id, ESP_GATT_OK, NULL);
        }
      }
      break;

    case ESP_GATTS_EXEC_WRITE_EVT:
      blemidi_exec_write_event_env(&prepare_write_env, param);
      break;

    case ESP_GATTS_MTU_EVT:
      if (param->mtu.mtu <= 3) {
        blemidi_mtu = 3;
      } else {
        blemidi_mtu = param->mtu.mtu - 3;
        if (blemidi_mtu > (GATTS_MIDI_CHAR_VAL_LEN_MAX - 3))
          blemidi_mtu = (GATTS_MIDI_CHAR_VAL_LEN_MAX - 3);
      }
      BLEMIDI_PRINTF_I("MTU updated: %d (usable %d)", param->mtu.mtu, (int)blemidi_mtu);
      break;

    case ESP_GATTS_CONF_EVT:
      BLEMIDI_PRINTF_D("CONF_EVT status=%d handle=%d",
                       param->conf.status, param->conf.handle);
      break;

    case ESP_GATTS_START_EVT:
      BLEMIDI_PRINTF_I("SERVICE_START status=%d handle=%d",
                       param->start.status, param->start.service_handle);
      break;

    case ESP_GATTS_CONNECT_EVT: {
      BLEMIDI_PRINTF_I("CONNECT conn_id=%d", param->connect.conn_id);
      midi_profile_tab[PROFILE_APP_IDX].conn_id = param->connect.conn_id;

      esp_ble_conn_update_params_t conn_params = {0};
      memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      conn_params.latency = 0;
      conn_params.max_int = 0x10;  // 20ms
      conn_params.min_int = 0x0b;  // 15ms
      conn_params.timeout = 400;   // 4s
      esp_ble_gap_update_conn_params(&conn_params);
      break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
      BLEMIDI_PRINTF_I("DISCONNECT reason=0x%x", param->disconnect.reason);
      esp_ble_gap_start_advertising(&adv_params);
      break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
      if (param->add_attr_tab.status != ESP_GATT_OK) {
        BLEMIDI_PRINTF_E("attr table err=0x%x", param->add_attr_tab.status);
      } else if (param->add_attr_tab.num_handle != HRS_IDX_NB) {
        BLEMIDI_PRINTF_E("attr table num_handle=%d != %d",
                         param->add_attr_tab.num_handle, HRS_IDX_NB);
      } else {
        memcpy(midi_handle_table, param->add_attr_tab.handles, sizeof(midi_handle_table));
        esp_ble_gatts_start_service(midi_handle_table[IDX_SVC]);
      }
      break;
    }

    default:
      break;
  }
}

/* Dispatcher GATTS */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) {
  if (event == ESP_GATTS_REG_EVT) {
    if (param->reg.status == ESP_GATT_OK) {
      midi_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
      midi_profile_tab[PROFILE_APP_IDX].gatts_cb = gatts_profile_event_handler;
    } else {
      BLEMIDI_PRINTF_E("reg app failed, app_id %04x, status %d",
                       param->reg.app_id, param->reg.status);
      return;
    }
  }

  for (int i = 0; i < PROFILE_NUM; ++i) {
    if (gatts_if == ESP_GATT_IF_NONE ||
        gatts_if == midi_profile_tab[i].gatts_if) {
      if (midi_profile_tab[i].gatts_cb) {
        midi_profile_tab[i].gatts_cb(event, gatts_if, param);
      }
    }
  }
}

/* Init público */
int32_t blemidi_init(void *_callback_midi_message_received) {
  esp_err_t ret;

  blemidi_callback_midi_message_received = NULL;

  ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
  ESP_ERROR_CHECK(esp_bluedroid_init());
  ESP_ERROR_CHECK(esp_bluedroid_enable());

  ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
  ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
  ESP_ERROR_CHECK(esp_ble_gatts_app_register(ESP_APP_ID));

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(GATTS_MIDI_CHAR_VAL_LEN_MAX);
  if (local_mtu_ret) {
    BLEMIDI_PRINTF_E("set local MTU failed: %x", local_mtu_ret);
    return -1;
  }

  for (uint32_t p = 0; p < BLEMIDI_NUM_PORTS; ++p) {
    blemidi_outbuffer_len[p] = 0;
    blemidi_continued_sysex_pos[p] = 0;
  }

  // Instala callback de usuario
  blemidi_callback_midi_message_received = _callback_midi_message_received;

  // Iniciar timer periódico de flush
  {
    const esp_timer_create_args_t targs = {
        .callback = &flush_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "midi_flush",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_flush_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_flush_timer,
                                             (uint64_t)BLEMIDI_OUTBUFFER_FLUSH_MS * 1000ULL)); // us
  }

  return 0;
}
