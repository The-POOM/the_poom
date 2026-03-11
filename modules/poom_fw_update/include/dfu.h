#ifndef POOM_FW_UPDATE_H
#define POOM_FW_UPDATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* =========================
 * Compile-time constants
 * ========================= */
#define CURRENT_FW_VERSION "1.0"

/* =========================
 * Public types
 * ========================= */
typedef enum {
  POOM_FW_UPDATE_SHOW_PROGRESS_EVENT = 0, /*!< OTA upload progress event */
  POOM_FW_UPDATE_SHOW_START_EVENT,        /*!< OTA start event */
  POOM_FW_UPDATE_SHOW_RESULT_EVENT        /*!< OTA final result event */
} poom_fw_update_show_events_t;

typedef void (*poom_fw_update_show_event_cb_t)(uint8_t, void*);

/* =========================
 * Legacy aliases
 * =========================
 * Kept for compatibility with existing modules. New code should use
 * POOM_FW_UPDATE_* names.
 */
typedef poom_fw_update_show_events_t ota_show_events_t;
typedef poom_fw_update_show_event_cb_t ota_show_event_cb_t;
#define DFU_SHOW_PROGRESS_EVENT      POOM_FW_UPDATE_SHOW_PROGRESS_EVENT
#define DFU_SHOW_START_EVENT         POOM_FW_UPDATE_SHOW_START_EVENT
#define DFU_SHOW_RESULT_EVENT        POOM_FW_UPDATE_SHOW_RESULT_EVENT
#define POOM_DFU_SHOW_PROGRESS_EVENT POOM_FW_UPDATE_SHOW_PROGRESS_EVENT
#define POOM_DFU_SHOW_START_EVENT    POOM_FW_UPDATE_SHOW_START_EVENT
#define POOM_DFU_SHOW_RESULT_EVENT   POOM_FW_UPDATE_SHOW_RESULT_EVENT

/* =========================
 * Public API
 * ========================= */

/**
 * @brief Initializes firmware-update service in AP mode.
 *
 * This function starts the manager AP through `poom_wifi_ctrl`, initializes
 * mDNS, and starts the embedded HTTP server used for OTA upload.
 *
 * @return
 * - ESP_OK on success
 * - error propagated from Wi-Fi/mDNS/HTTP server setup
 */
esp_err_t poom_fw_update_init(void);

/**
 * @brief Registers callback to receive update UI events.
 *
 * @param[in] cb Callback function. Can be NULL to clear callback.
 * @return ESP_OK
 */
esp_err_t poom_fw_update_set_show_event_cb(poom_fw_update_show_event_cb_t cb);

/**
 * @brief Emits a firmware-update event to the registered callback.
 *
 * This is used internally by OTA flow modules (for example HTTP handler), but
 * is exposed for integration points that may need to relay update state.
 *
 * @param[in] event   Event ID from `poom_fw_update_show_events_t`.
 * @param[in] context Event-specific context pointer (nullable).
 */
void poom_fw_update_emit_event(uint8_t event, void* context);

/**
 * @brief Returns AP SSID used by firmware update mode.
 *
 * @return Null-terminated SSID string.
 */
const char* poom_fw_update_get_wifi_ap_ssid(void);

/**
 * @brief Returns AP password used by firmware update mode.
 *
 * @return Null-terminated password string.
 */
const char* poom_fw_update_get_wifi_ap_password(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_FW_UPDATE_H */
