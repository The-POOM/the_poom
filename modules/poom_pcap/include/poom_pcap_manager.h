// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_pcap_manager.h
 * @brief PCAP capture manager (buffered PCAP writer with SD and UART outputs).
 *
 * `poom_pcap_manager` is a thin "capture layer" that sits above the `pcap` backend.
 * It provides:
 * - Buffered writes (reduces per-packet IO overhead)
 * - SD file output with automatic filename incrementing
 * - UART/stream fallback output using `printf`
 * - Small protocol adaptations (e.g. radiotap for WiFi)
 *
 * Output note:
 * - UART/stream outputs are **binary PCAP bytes** printed through `printf("%c", ...)`.
 *   A host tool must capture raw UART bytes (not line-based text).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Capture source type selection.
 *
 * This controls the PCAP DLT (link-layer type) and any framing adjustments.
 */
typedef enum
{
    POOM_PCAP_CAPTURE_WIFI = 0,
    /** Bluetooth capture. Payload format is user-provided; DLT configured by the manager. */
    POOM_PCAP_CAPTURE_BLUETOOTH,
    POOM_PCAP_CAPTURE_IEEE802154,
} poom_pcap_capture_type_t;

/**
 * @brief WiFi capture filtering modes for the sniffer helper.
 *
 * These modes apply additional software filtering on top of the ESP-IDF
 * promiscuous hardware filter mask.
 */
typedef enum
{
    /** Capture all WiFi frames passed by the hardware filter mask. */
    POOM_PCAP_WIFI_CAPTURE_RAW = 0,
    /** Capture only 802.11 Beacon frames (management subtype 8). */
    POOM_PCAP_WIFI_CAPTURE_BEACON,
    /** Capture only 802.11 Probe Request frames (management subtype 4). */
    POOM_PCAP_WIFI_CAPTURE_PROBE,
    /** Capture only 802.11 Deauthentication frames (management subtype 12). */
    POOM_PCAP_WIFI_CAPTURE_DEAUTH,
    /** Capture only EAPOL-Key frames (WPA/WPA2 handshakes, rekey, PMKID). */
    POOM_PCAP_WIFI_CAPTURE_EAPOL,
    /** Capture only WPS EAP (EAP-Expanded / WFA) frames over EAPOL. */
    POOM_PCAP_WIFI_CAPTURE_WPS,
    POOM_PCAP_WIFI_CAPTURE_COUNT,
} poom_pcap_wifi_capture_t;

/** IEEE 802.15.4/Zigbee channel range. */
#define POOM_PCAP_IEEE802154_CHANNEL_MIN (11U)
/** IEEE 802.15.4/Zigbee channel range. */
#define POOM_PCAP_IEEE802154_CHANNEL_MAX (26U)
/** Default channel used when not specified. */
#define POOM_PCAP_IEEE802154_CHANNEL_DEFAULT (11U)

/**
 * @brief Output mode selection.
 */
typedef enum
{
    /** Try SD file output, fallback to UART on failure. */
    POOM_PCAP_OUTPUT_AUTO = 0,
    /** Require SD file output; start fails if file cannot be created. */
    POOM_PCAP_OUTPUT_FILE,
    /** UART output with optional flush markers (`[BUFFER/INIT]`, `[BUFFER/CLOSE]`). */
    POOM_PCAP_OUTPUT_UART,
    /** Raw binary UART stream (no markers). */
    POOM_PCAP_OUTPUT_STREAM,
} poom_pcap_output_mode_t;

/**
 * @brief Initialization configuration.
 *
 * Any NULL/0 fields use defaults.
 */
typedef struct
{
    /** Buffer size in bytes; 0 uses an internal default. */
    size_t buffer_size;
    /** SD directory for capture files (e.g. "/pcaps"); NULL uses an internal default. */
    const char *sd_dir;
    /** Base name for capture files (e.g. "capture"); NULL uses an internal default. */
    const char *base_name;
    /** When true, wrap UART flushes with `[BUFFER/INIT]` / `[BUFFER/CLOSE]`. */
    bool uart_markers;
} poom_pcap_manager_config_t;

/**
 * @brief Initialize the PCAP manager (mutex + buffer).
 *
 * This does not start capturing; call one of the start functions after init.
 *
 * @param[in] config Optional configuration, or NULL for defaults.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_init(const poom_pcap_manager_config_t *config);

/**
 * @brief Deinitialize the PCAP manager and free resources.
 *
 * If capture is active, it will be closed first.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_deinit(void);

/**
 * @brief Start capture in auto mode (SD -> UART fallback).
 *
 * Uses config defaults for sd_dir/base_name when available.
 *
 * @param[in] capture_type Capture source type (DLT + framing rules).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_start_auto(poom_pcap_capture_type_t capture_type);

/**
 * @brief Start capture to a new SD file; optionally fallback to UART.
 *
 * @param base_name Base filename (without index/extension). NULL uses config/default.
 * @param sd_dir Capture directory under SD root (e.g. "/pcaps"). NULL uses config/default.
 * @param capture_type Link type for this capture.
 * @param allow_uart_fallback When true, failure to open SD file starts UART mode instead.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_start_file(const char *base_name,
                                      const char *sd_dir,
                                      poom_pcap_capture_type_t capture_type,
                                      bool allow_uart_fallback);

/**
 * @brief Start capture output via UART with markers.
 *
 * @param[in] capture_type Capture source type (DLT + framing rules).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_start_uart(poom_pcap_capture_type_t capture_type);

/**
 * @brief Start capture as a raw binary PCAP stream via UART (no markers).
 *
 * Output is binary PCAP bytes written through printf("%c", ...).
 *
 * @param[in] capture_type Capture source type (DLT + framing rules).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_start_stream(poom_pcap_capture_type_t capture_type);

/**
 * @brief Append one captured packet.
 *
 * @note `type` must match the capture type used when starting the capture.
 *
 * @param[in] packet Packet/frame bytes.
 * @param[in] len Packet length in bytes.
 * @param[in] type Capture type (must equal current capture type).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_write_packet(const void *packet, size_t len, poom_pcap_capture_type_t type);

/**
 * @brief Flush buffered packets to the current output target.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_flush(void);

/**
 * @brief Close current capture, flush remaining data, and reset state.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_close(void);

/**
 * @brief Returns whether a capture session is active.
 */
bool poom_pcap_manager_is_active(void);

/**
 * @brief Returns current output mode.
 */
poom_pcap_output_mode_t poom_pcap_manager_get_mode(void);

/**
 * @brief Returns whether current capture is in stream mode.
 */
bool poom_pcap_manager_is_stream(void);

/**
 * @brief Returns whether current capture is in UART-marker mode.
 */
bool poom_pcap_manager_is_uart(void);

/**
 * @brief Get the current output file path (SD mode only).
 *
 * @return Pointer to internal buffer, or NULL when not in file mode.
 */
const char *poom_pcap_manager_get_file_path(void);

/**
 * @brief Helper: start WiFi monitor/promiscuous mode for capture.
 *
 * This is a convenience wrapper that:
 * - Initializes WiFi in STA mode
 * - Disconnects STA (to avoid channel lock issues)
 * - Sets a hardware promiscuous filter mask
 * - Enables promiscuous mode and registers the RX callback
 *
 * @note This function configures the WiFi radio, but does not start/close a PCAP
 *       session. Call `poom_pcap_manager_start_*()` separately.
 *
 * @param[in] callback Promiscuous RX callback (must not be NULL).
 * @param[in] filter_mask Hardware filter mask (e.g. `WIFI_PROMIS_FILTER_MASK_ALL`).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_wifi_start_monitor_mode(wifi_promiscuous_cb_t callback,
                                                    uint32_t filter_mask);

/**
 * @brief Helper: stop WiFi monitor/promiscuous mode for capture.
 *
 * Disables promiscuous mode and clears the RX callback.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_wifi_stop_monitor_mode(void);

/**
 * @brief Starts a WiFi PCAP sniffer session (auto output) and enables monitor mode.
 *
 * This starts the PCAP manager in AUTO output mode and registers an internal
 * promiscuous callback that writes 802.11 frames into the capture buffer.
 *
 * @param[in] channel Primary WiFi channel (1..13). Pass 0 to keep current.
 * @param[in] filter_mask Hardware filter mask (e.g. `WIFI_PROMIS_FILTER_MASK_ALL`).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_sniffer_start_wifi(uint8_t channel, uint32_t filter_mask);

/**
 * @brief Starts a WiFi PCAP sniffer session with a capture filter mode.
 *
 * @param[in] channel Primary WiFi channel (1..13). Pass 0 to keep current.
 * @param[in] filter_mask Hardware filter mask (e.g. `WIFI_PROMIS_FILTER_MASK_ALL`).
 * @param[in] capture_mode Software capture filter mode.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_sniffer_start_wifi_capture(uint8_t channel,
                                                       uint32_t filter_mask,
                                                       poom_pcap_wifi_capture_t capture_mode);

/**
 * @brief Starts a BLE PCAP sniffer session (auto output) using `poom_ble_scan`.
 *
 * Captures HCI LE Advertising Reports (H4 event packets) suitable for Wireshark.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_sniffer_start_ble(void);

/**
 * @brief Starts an IEEE 802.15.4 PCAP sniffer session (auto output).
 *
 * @param[in] channel IEEE 802.15.4 channel (11..26). Pass 0 to start at 11.
 * @param[in] enable_hopping When true, channel hops 11..26 periodically.
 * @param[in] hop_ms Hop dwell time in milliseconds (0 uses a default).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_sniffer_start_zigbee(uint8_t channel, bool enable_hopping, uint32_t hop_ms);

/**
 * @brief Stops the active sniffer session and releases capture resources.
 */
esp_err_t poom_pcap_manager_sniffer_stop(void);

/**
 * @brief Returns whether a sniffer session is active.
 */
bool poom_pcap_manager_sniffer_is_active(void);

/**
 * @brief Set IEEE 802.15.4 channel while Zigbee sniffer is running (fixed-channel mode).
 *
 * @param[in] channel Channel number (11..26).
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_pcap_manager_sniffer_zigbee_set_channel(uint8_t channel);

/**
 * @brief Get the current Zigbee sniffer channel.
 *
 * @return Channel number, or 0 when not in Zigbee mode.
 */
uint8_t poom_pcap_manager_sniffer_zigbee_get_channel(void);

/**
 * @brief Get most recent RSSI from the IEEE 802.15.4 driver.
 *
 * @return RSSI in dBm, or -127 when unavailable.
 */
int8_t poom_pcap_manager_sniffer_zigbee_get_rssi(void);

#ifdef __cplusplus
}
#endif
