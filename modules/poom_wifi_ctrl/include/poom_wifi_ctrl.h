#ifndef POOM_WIFI_CTRL_H
#define POOM_WIFI_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"

/* =========================
 * Compile-time constants
 * ========================= */
#define POOM_WIFI_CTRL_MAC_LEN               (6U)
#define POOM_WIFI_CTRL_WIFI_CHANNEL_MIN      (1U)
#define POOM_WIFI_CTRL_WIFI_CHANNEL_MAX      (13U)

/* =========================
 * Kconfig mapping (project)
 * =========================
 */
#define POOM_WIFI_CTRL_MANAGER_AP_SSID            CONFIG_POOM_WIFI_CTRL_MANAGER_AP_SSID
#define POOM_WIFI_CTRL_MANAGER_AP_PASSWORD        CONFIG_POOM_WIFI_CTRL_MANAGER_AP_PASSWORD
#define POOM_WIFI_CTRL_MANAGER_AP_MAX_CONNECTIONS CONFIG_POOM_WIFI_CTRL_MANAGER_AP_MAX_CONNECTIONS
#define POOM_WIFI_CTRL_MANAGER_AP_CHANNEL         CONFIG_POOM_WIFI_CTRL_MANAGER_AP_CHANNEL

#if defined(CONFIG_POOM_WIFI_CTRL_MANAGER_AP_AUTH_ENABLE) && (CONFIG_POOM_WIFI_CTRL_MANAGER_AP_AUTH_ENABLE == 1)
  #define POOM_WIFI_CTRL_MANAGER_AP_AUTHMODE WIFI_AUTH_WPA2_PSK
#else
  #define POOM_WIFI_CTRL_MANAGER_AP_AUTHMODE WIFI_AUTH_OPEN
#endif

/* =========================
 * Public API
 * ========================= */
typedef enum
{
    POOM_WIFI_CTRL_EVT_STA_CONNECTED = 0,
    POOM_WIFI_CTRL_EVT_STA_DISCONNECTED,
    POOM_WIFI_CTRL_EVT_STA_GOT_IP,
} poom_wifi_ctrl_evt_t;

typedef struct
{
    poom_wifi_ctrl_evt_t evt;
    int32_t reason;              /* DISCONNECTED (wifi_err_reason_t) */
    esp_ip4_addr_t ip;           /* GOT_IP */
    esp_ip4_addr_t netmask;      /* GOT_IP */
    esp_ip4_addr_t gw;           /* GOT_IP */
} poom_wifi_ctrl_evt_info_t;

typedef void (*poom_wifi_ctrl_evt_cb_t)(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx);

/**
 * @brief Builds the default "Manager AP" configuration using Kconfig values.
 *
 * @param[out] out_cfg Output AP config. Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_manager_ap_config_default(wifi_config_t *out_cfg);

/**
 * @brief Starts the POOM "Manager AP" using the default configuration (Kconfig).
 *
 * @param[out] out_cfg Optional. If non-NULL, receives the config used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_manager_ap_start(wifi_config_t *out_cfg);

/**
 * @brief Starts Access Point with provided config.
 *
 * @param[in] ap_cfg AP config. Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_ap_start(wifi_config_t *ap_cfg);

/**
 * @brief Stops Access Point (sets max_connection=0).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_ap_stop(void);

/**
 * @brief Initializes Wi-Fi with AP+STA mode and starts Wi-Fi driver.
 *
 * @note Creates default netifs (AP+STA) once. Idempotent.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_init_apsta(void);

/**
 * @brief Initializes Wi-Fi with STA mode and starts Wi-Fi driver.
 *
 * @note Creates default STA netif once. Idempotent.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_init_sta(void);

/**
 * @brief Initializes Wi-Fi in NULL mode (Wi-Fi disabled but driver init done).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_init_null(void);

/**
 * @brief Deinitializes Wi-Fi driver (stop + deinit).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_deinit(void);

/**
 * @brief Disconnect STA (if connected/connecting).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_sta_disconnect(void);

/**
 * @brief Override AP MAC address.
 *
 * @param[in] ap_mac MAC (6 bytes). Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_set_ap_mac(const uint8_t *ap_mac);

/**
 * @brief Read AP MAC address.
 *
 * @param[out] ap_mac MAC (6 bytes). Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_get_ap_mac(uint8_t *ap_mac);

/**
 * @brief Restore original AP MAC (saved at first init_apsta()).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_restore_ap_mac(void);

/**
 * @brief Read STA MAC address.
 *
 * @param[out] sta_mac MAC (6 bytes). Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_get_sta_mac(uint8_t *sta_mac);

/**
 * @brief Set Wi-Fi primary channel (1..13).
 *
 * @param[in] channel Primary channel.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel);

/**
 * @brief Connect to Wi-Fi in STA mode.
 *
 * @param[in] ssid     Target SSID.
 * @param[in] password WPA2 password (NULL for open).
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_sta_connect(const char *ssid,
                                     const char *password);
/**
 * @brief Register an event callback for STA connect/disconnect/IP.
 *
 * @param[in] cb Callback (NULL to disable).
 * @param[in] user_ctx User context pointer passed back to callback.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_register_cb(poom_wifi_ctrl_evt_cb_t cb, void *user_ctx);

/**
 * @brief Unregister event callback and internal handlers.
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_unregister_cb(void);

/**
 * @brief Returns whether STA is connected (link-level).
 */
bool poom_wifi_ctrl_sta_is_connected(void);

/**
 * @brief Returns whether STA has a valid IP (DHCP done).
 */
bool poom_wifi_ctrl_sta_has_ip(void);

/**
 * @brief Enables or disables promiscuous mode.
 *
 * @param[in] enabled true to enable, false to disable.
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_set_promiscuous(bool enabled);

/**
 * @brief Sets promiscuous RX callback.
 *
 * @param[in] cb Callback function (NULL to clear).
 * @return esp_err_t
 */
esp_err_t poom_wifi_ctrl_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_CTRL_H */
