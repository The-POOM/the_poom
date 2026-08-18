

/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdio.h>

#include "zb_cli.h"

static const char* TAG = "zb_cli";
static TaskHandle_t s_zb_stack_task = NULL;

// Replace ESP_LOG* with simple printf tags (no log colors / levels).
#define ZB_PRINTF_I(fmt, ...) printf("[IN] [%s] " fmt "\n", TAG, ##__VA_ARGS__)
#define ZB_PRINTF_W(fmt, ...) printf("[WR] [%s] " fmt "\n", TAG, ##__VA_ARGS__)
#define ZB_PRINTF_E(fmt, ...) printf("[ER] [%s] " fmt "\n", TAG, ##__VA_ARGS__)

static void log_nwk_info(const char* status_string) {
  esp_zb_ieee_addr_t extended_pan_id;
  esp_zb_get_extended_pan_id(extended_pan_id);
  ZB_PRINTF_I(
      "%s (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN "
      "ID: 0x%04hx, "
      "Channel:%d, Short Address: 0x%04hx)",
      status_string, extended_pan_id[7], extended_pan_id[6], extended_pan_id[5],
      extended_pan_id[4], extended_pan_id[3], extended_pan_id[2],
      extended_pan_id[1], extended_pan_id[0], esp_zb_get_pan_id(),
      esp_zb_get_current_channel(), esp_zb_get_short_address());
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal_struct) {
  uint32_t* p_sg_p = signal_struct->p_app_signal;
  esp_err_t err_status = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig_type = *p_sg_p;
  esp_zb_zdo_signal_device_annce_params_t* dev_annce_params = NULL;
  esp_zb_zdo_signal_leave_indication_params_t* leave_ind_params = NULL;
  const char* err_name = esp_err_to_name(err_status);
  switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      ZB_PRINTF_I("Initialize Zigbee stack");
      break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      if (err_status == ESP_OK) {
        ZB_PRINTF_I("Device started up in%s factory-reset mode",
                    esp_zb_bdb_is_factory_new() ? "" : " non");
      } else {
        ZB_PRINTF_E("%s failed with status: %s, please retry",
                    esp_zb_zdo_signal_to_string(sig_type),
                    esp_err_to_name(err_status));
      }
      break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
      if (err_status == ESP_OK) {
        log_nwk_info("Formed network successfully");
      } else {
        ZB_PRINTF_I("Failed to form network (status: %s)", err_name);
      }
      break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
      if (err_status == ESP_OK) {
        log_nwk_info("Joined network successfully");
      } else {
        ZB_PRINTF_I("Failed to join network (status: %s)", err_name);
      }
      break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE:
      if (err_status == ESP_OK) {
        ZB_PRINTF_I("Left network successfully");
      } else {
        ZB_PRINTF_E("Failed to leave network (status: %s)", err_name);
      }
      break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION:
      leave_ind_params = (esp_zb_zdo_signal_leave_indication_params_t*)
          esp_zb_app_signal_get_params(p_sg_p);
      ZB_PRINTF_I("Zigbee Node (0x%04hx) is leaving network",
                  leave_ind_params->short_addr);
      break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
      dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t*)
          esp_zb_app_signal_get_params(p_sg_p);
      ZB_PRINTF_I("New device commissioned or rejoined (short: 0x%04hx)",
                  dev_annce_params->device_short_addr);
      break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
      if (err_status == ESP_OK) {
        if (*(uint8_t*) esp_zb_app_signal_get_params(p_sg_p)) {
          ZB_PRINTF_I("Network(0x%04hx) is open for %d seconds",
                      esp_zb_get_pan_id(),
                      *(uint8_t*) esp_zb_app_signal_get_params(p_sg_p));
        } else {
          ZB_PRINTF_W("Network(0x%04hx) closed, devices joining not allowed.",
                      esp_zb_get_pan_id());
        }
      }
      break;
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_TARGET:
      ZB_PRINTF_I("Touchlink target is ready, waiting for commissioning");
      break;
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK:
      if (err_status == ESP_OK) {
        log_nwk_info("Touchlink commissioning successfully");
      } else {
        ZB_PRINTF_W("Touchlink commissioning failed (status: %s)", err_name);
      }
      break;
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_TARGET_FINISHED:
      ZB_PRINTF_I("Touchlink target finished (status: %s)", err_name);
      break;
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_STARTED:
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_JOINED_ROUTER:
      ZB_PRINTF_I("Touchlink initiator receives the response for %s network",
                  sig_type == ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_STARTED
                      ? "started"
                      : "router joining");
      esp_zb_bdb_signal_touchlink_nwk_params_t* sig_params =
          (esp_zb_bdb_signal_touchlink_nwk_params_t*)
              esp_zb_app_signal_get_params(p_sg_p);
      ZB_PRINTF_I(
          "Response is from profile: 0x%04hx, endpoint: %d, address: "
          "0x%16" PRIx64,
          sig_params->profile_id, sig_params->endpoint,
          *(uint64_t*) sig_params->device_ieee_addr);
      break;
    case ESP_ZB_BDB_SIGNAL_TOUCHLINK:
      if (err_status == ESP_OK) {
        log_nwk_info("Touchlink commissioning successfully");
      } else {
        ZB_PRINTF_W("No Touchlink target devices found");
      }
      break;
    default:
      ZB_PRINTF_I("ZDO signal: %s (0x%x), status: %s",
                  esp_zb_zdo_signal_to_string(sig_type), sig_type, err_name);
      break;
  }
}

void zb_stack_init(void) {
  /* Initialize Zigbee stack with default configuraion */
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
  esp_zb_init(&zb_nwk_cfg);

  /* Set default allowed network channels */
  esp_zb_set_channel_mask(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

  /* Set default scan channels */
  esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
  esp_zb_set_secondary_network_channel_set(
      ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

  /* Enable CLI managed ep_list */
  esp_zb_console_manage_ep_list(NULL);
}

static void zb_stack_main_task(void* pvParameters) {
  zb_stack_init();

  /* Do not call `esp_zb_start()`.
   *
   * We want the timing of starting the stack to be mananged by CLI,
   * so that we have a chance to do configurations on the stack.
   *
   */

  esp_zb_stack_main_loop();

  esp_zb_console_deinit();
  s_zb_stack_task = NULL;
  vTaskDelete(NULL);
}

void zb_cli_begin(void) {
  if (s_zb_stack_task != NULL) {
    ZB_PRINTF_W("Zigbee CLI already running");
    return;
  }

  esp_zb_platform_config_t config = {
      .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
      .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
  };
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_zb_console_init());
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));
  (void)xTaskCreate(zb_stack_main_task, "Zigbee_main", 4096, NULL, 5, &s_zb_stack_task);
  ESP_ERROR_CHECK(esp_zb_console_start());
}

void zb_cli_stop() {
  (void)esp_zb_console_deinit();
  if (s_zb_stack_task != NULL) {
    vTaskDelete(s_zb_stack_task);
    s_zb_stack_task = NULL;
  }
}

void zb_cli_cmd_register(void) {
  esp_console_cmd_t cmd_start = {
      .command = "zb_init",
      .help = "Initialize the Zigbee console",
//      .category = NULL,
      .hint = NULL,
      .func = (esp_console_cmd_func_t)&zb_cli_begin,
      .argtable = NULL,
  };
  esp_console_cmd_t cmd_stop = {
      .command = "zb_deinit",
      .help = "Deinitialize the Zigbee console",
//      .category = NULL,
      .hint = NULL,
      .func = (esp_console_cmd_func_t)&zb_cli_stop,
      .argtable = NULL,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_start));
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_stop));
}
