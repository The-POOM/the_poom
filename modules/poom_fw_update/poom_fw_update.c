
#include "dfu.h"
#include "http_server.h"
#include "mdns_manager.h"
#include "poom_dfu_log.h"
#include "poom_wifi_ctrl.h"

static poom_fw_update_show_event_cb_t s_poom_fw_update_show_event_cb = NULL;
static const char* POOM_DFU_TAG = "poom_fw_update";

esp_err_t poom_fw_update_init(void) {
  esp_err_t err = poom_wifi_ctrl_manager_ap_start(NULL);
  if (err != ESP_OK) {
    POOM_DFU_PRINTF_E(POOM_DFU_TAG, "AP start failed: %s", esp_err_to_name(err));
    return err;
  }

  err = setup_mdns();
  if (err != ESP_OK) {
    POOM_DFU_PRINTF_W(POOM_DFU_TAG, "mDNS setup failed: %s", esp_err_to_name(err));
  }

  err = http_server_start();
  if (err != ESP_OK) {
    POOM_DFU_PRINTF_E(POOM_DFU_TAG, "HTTP server start failed: %s", esp_err_to_name(err));
    return err;
  }
  return ESP_OK;
}

esp_err_t poom_fw_update_set_show_event_cb(poom_fw_update_show_event_cb_t cb) {
  s_poom_fw_update_show_event_cb = cb;
  return ESP_OK;
}

void poom_fw_update_emit_event(uint8_t event, void* context) {
  poom_fw_update_show_event_cb_t cb = s_poom_fw_update_show_event_cb;
  if (cb) {
    cb(event, context);
  }
}

const char* poom_fw_update_get_wifi_ap_ssid(void) {
  return POOM_WIFI_CTRL_MANAGER_AP_SSID;
}

const char* poom_fw_update_get_wifi_ap_password(void) {
  return POOM_WIFI_CTRL_MANAGER_AP_PASSWORD;
}
