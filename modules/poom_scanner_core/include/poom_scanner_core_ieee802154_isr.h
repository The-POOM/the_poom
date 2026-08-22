#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Optional ISR consumer invoked from `esp_ieee802154_receive_done()`.
 *
 * The consumer must be ISR-safe:
 * - Do not block.
 * - Do not call `esp_ieee802154_receive_handle_done()` (handled by the core).
 * - Do not call `esp_ieee802154_receive()` (handled by the core).
 *
 * @param[in] frame Frame pointer as provided by the driver.
 * @param[in] frame_info Extra frame metadata (may be NULL depending on IDF).
 * @param[in,out] woken Set to pdTRUE when a higher-priority task should be woken.
 * @param[in] user User context pointer.
 */
typedef void (*poom_scanner_core_ieee802154_isr_consumer_t)(
    uint8_t* frame,
    esp_ieee802154_frame_info_t* frame_info,
    BaseType_t* woken,
    void* user);

/**
 * @brief Register a single optional ISR consumer.
 *
 * Used by other modules (e.g. PCAP capture) to consume frames while the scanner
 * core owns the global callback symbol.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already registered.
 */
esp_err_t poom_scanner_core_ieee802154_register_isr_consumer(
    poom_scanner_core_ieee802154_isr_consumer_t cb,
    void* user);

/**
 * @brief Unregister the current ISR consumer.
 */
void poom_scanner_core_ieee802154_unregister_isr_consumer(
    poom_scanner_core_ieee802154_isr_consumer_t cb);

#ifdef __cplusplus
}
#endif
