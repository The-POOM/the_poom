/* poom_wifi_ctrl.c
 *
 * POOM Wi-Fi Controller
 * - AP / STA / APSTA / NULL mode control
 * - Optional mDNS hook
 * - STA connect/disconnect + event callbacks (CONNECTED / DISCONNECTED / GOT_IP)
 *
 */

#include "poom_wifi_ctrl.h"

#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

/* =========================
 * Optional mDNS hook
 * ========================= */
#if defined(CONFIG_POOM_WIFI_CTRL_ENABLE_MDNS) && (CONFIG_POOM_WIFI_CTRL_ENABLE_MDNS == 1)
  #if defined(__has_include)
    #if __has_include("mdns_manager.h")
      #include "mdns_manager.h"
      #define POOM_WIFI_CTRL_HAS_MDNS (1)
    #else
      #define POOM_WIFI_CTRL_HAS_MDNS (0)
    #endif
  #else
    #include "mdns_manager.h"
    #define POOM_WIFI_CTRL_HAS_MDNS (1)
  #endif
#else
  #define POOM_WIFI_CTRL_HAS_MDNS (0)
#endif

/* =========================
 * Local log macros (printf)
 * ========================= */
static const char *POOM_WIFI_CTRL_TAG = "poom_wifi_ctrl";
//#define CONFIG_POOM_WIFI_CTRL_ENABLE_LOG 1
#if CONFIG_POOM_WIFI_CTRL_ENABLE_LOG

    #define POOM_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_WIFI_CTRL_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_WIFI_CTRL_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_WIFI_CTRL_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_WIFI_CTRL_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

    #define POOM_PRINTF_E(...)
    #define POOM_PRINTF_W(...)
    #define POOM_PRINTF_I(...)
    #define POOM_PRINTF_D(...)

#endif


/* =========================
 * Local constants/state
 * ========================= */

/* App callback */
static poom_wifi_ctrl_evt_cb_t s_evt_cb = NULL;
static void *s_evt_user_ctx = NULL;

/* Event handler instances */
static esp_event_handler_instance_t s_wifi_any_id_inst;
static esp_event_handler_instance_t s_ip_got_ip_inst;
static esp_event_handler_instance_t s_ip_assigned_client_inst;
static bool s_evt_handlers_registered = false;

/* STA state flags */
static bool s_sta_connected = false;
static bool s_sta_has_ip = false;

typedef struct
{
    bool wifi_initialized;
    bool netif_ap_created;
    bool netif_sta_created;
    bool default_ap_mac_saved;
    uint8_t default_ap_mac[POOM_WIFI_CTRL_MAC_LEN];
} poom_wifi_ctrl_state_t;

static poom_wifi_ctrl_state_t s_wifi = {0};

/* =========================
 * Local helpers
 * ========================= */

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] dst Parameter passed to the function.
 * @param[in] dst_size Parameter passed to the function.
 * @param[in] src Parameter passed to the function.
 * @return void
 */
static void poom_wifi_ctrl_safe_copy_str_(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t n = 0U;

    if ((dst == NULL) || (dst_size == 0U))
    {
        return;
    }

    (void)memset(dst, 0, dst_size);

    if (src == NULL)
    {
        return;
    }

    while ((n < (dst_size - 1U)) && (src[n] != '\0'))
    {
        dst[n] = (uint8_t)src[n];
        n++;
    }
}

/**
 * @brief Internal helper for `poom_wifi_ctrl_hostname_or_dash`.
 *
 * @param[in] hostname Parameter passed to the function.
 * @return const char *
 */
static const char *poom_wifi_ctrl_hostname_or_dash_(const char *hostname)
{
    if ((hostname == NULL) || (hostname[0] == '\0'))
    {
        return "-";
    }
    return hostname;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_ctrl_nvs_init_(void)
{
    esp_err_t err = nvs_flash_init();

    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        esp_err_t e2 = nvs_flash_erase();
        if (e2 != ESP_OK)
        {
            return e2;
        }
        err = nvs_flash_init();
    }
    return err;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_ctrl_event_loop_init_(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE)
    {
        err = ESP_OK;
    }
    return err;
}

/**
 * @brief Internal helper for `poom_wifi_ctrl_event_handler`.
 *
 * @param[in] arg Parameter passed to the function.
 * @param[in] event_base Parameter passed to the function.
 * @param[in] event_id Parameter passed to the function.
 * @param[in] event_data Parameter passed to the function.
 * @return void
 */
static void poom_wifi_ctrl_event_handler_(void *arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void *event_data)
{
    poom_wifi_ctrl_evt_info_t info;

    (void)arg;
    (void)memset(&info, 0, sizeof(info));

    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_CONNECTED)
        {
            s_sta_connected = true;
            s_sta_has_ip = false;

            info.evt = POOM_WIFI_CTRL_EVT_STA_CONNECTED;

            if (s_evt_cb != NULL)
            {
                s_evt_cb(&info, s_evt_user_ctx);
            }
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            const wifi_event_sta_disconnected_t *disc =
                (const wifi_event_sta_disconnected_t *)event_data;

            s_sta_connected = false;
            s_sta_has_ip = false;

            info.evt = POOM_WIFI_CTRL_EVT_STA_DISCONNECTED;
            info.reason = (disc != NULL) ? (int32_t)disc->reason : 0;

            POOM_PRINTF_W("STA disconnected (reason=%ld)", (long)info.reason);

            if (s_evt_cb != NULL)
            {
                s_evt_cb(&info, s_evt_user_ctx);
            }

        }
        else if (event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            const wifi_event_ap_staconnected_t *ap_evt =
                (const wifi_event_ap_staconnected_t *)event_data;

            if (ap_evt != NULL)
            {
                POOM_PRINTF_I("AP client connected: mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u",
                              ap_evt->mac[0], ap_evt->mac[1], ap_evt->mac[2],
                              ap_evt->mac[3], ap_evt->mac[4], ap_evt->mac[5],
                              (unsigned)ap_evt->aid);
            }
            else
            {
                POOM_PRINTF_I("AP client connected");
            }
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            const wifi_event_ap_stadisconnected_t *ap_evt =
                (const wifi_event_ap_stadisconnected_t *)event_data;

            if (ap_evt != NULL)
            {
                POOM_PRINTF_W("AP client disconnected: mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u",
                              ap_evt->mac[0], ap_evt->mac[1], ap_evt->mac[2],
                              ap_evt->mac[3], ap_evt->mac[4], ap_evt->mac[5],
                              (unsigned)ap_evt->aid);
            }
            else
            {
                POOM_PRINTF_W("AP client disconnected");
            }
        }
        else
        {
        }
    }
    else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
    {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;

        s_sta_has_ip = true;

        info.evt = POOM_WIFI_CTRL_EVT_STA_GOT_IP;
        if (ev != NULL)
        {
            info.ip      = ev->ip_info.ip;
            info.netmask = ev->ip_info.netmask;
            info.gw      = ev->ip_info.gw;
        }

        POOM_PRINTF_I("STA got IP: %u.%u.%u.%u",
                      (unsigned)(info.ip.addr & 0xFFU),
                      (unsigned)((info.ip.addr >> 8) & 0xFFU),
                      (unsigned)((info.ip.addr >> 16) & 0xFFU),
                      (unsigned)((info.ip.addr >> 24) & 0xFFU));

        if (s_evt_cb != NULL)
        {
            s_evt_cb(&info, s_evt_user_ctx);
        }
    }
    else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT))
    {
        const ip_event_assigned_ip_to_client_t *ev =
            (const ip_event_assigned_ip_to_client_t *)event_data;

        if (ev != NULL)
        {
            POOM_PRINTF_I("AP DHCP assigned: ip=%u.%u.%u.%u mac=%02X:%02X:%02X:%02X:%02X:%02X host=%s",
                          (unsigned)(ev->ip.addr & 0xFFU),
                          (unsigned)((ev->ip.addr >> 8) & 0xFFU),
                          (unsigned)((ev->ip.addr >> 16) & 0xFFU),
                          (unsigned)((ev->ip.addr >> 24) & 0xFFU),
                          ev->mac[0], ev->mac[1], ev->mac[2],
                          ev->mac[3], ev->mac[4], ev->mac[5],
                          poom_wifi_ctrl_hostname_or_dash_(ev->hostname));
        }
        else
        {
            POOM_PRINTF_I("AP DHCP assigned IP to client");
        }
    }
    else
    {
    }
}

/* Common init:
 * - idempotent
 * - creates netifs incrementally
 * - init wifi driver once
 * - set mode + start
 */

/**
 * @brief Initializes internal resources for this module.
 *
 * @param[in] mode Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_wifi_ctrl_common_init_(wifi_mode_t mode)
{
    esp_err_t err;

    err = poom_wifi_ctrl_nvs_init_();
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        POOM_PRINTF_E("netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = poom_wifi_ctrl_event_loop_init_();
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    if ((mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA))
    {
        if (!s_wifi.netif_ap_created)
        {
            (void)esp_netif_create_default_wifi_ap();
            s_wifi.netif_ap_created = true;
            POOM_PRINTF_D("Created default netif AP");
        }
    }

    if ((mode == WIFI_MODE_STA) || (mode == WIFI_MODE_APSTA))
    {
        if (!s_wifi.netif_sta_created)
        {
            (void)esp_netif_create_default_wifi_sta();
            s_wifi.netif_sta_created = true;
            POOM_PRINTF_D("Created default netif STA");
        }
    }

    if (!s_wifi.wifi_initialized)
    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            POOM_PRINTF_E("wifi init failed: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK)
        {
            POOM_PRINTF_E("wifi set storage failed: %s", esp_err_to_name(err));
            return err;
        }

        s_wifi.wifi_initialized = true;
        POOM_PRINTF_D("Wi-Fi driver initialized");
    }

    err = esp_wifi_set_mode(mode);
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("wifi set mode failed: %s", esp_err_to_name(err));
        return err;
    }

    if (((mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA)) && (!s_wifi.default_ap_mac_saved))
    {
        err = esp_wifi_get_mac(WIFI_IF_AP, s_wifi.default_ap_mac);
        if (err == ESP_OK)
        {
            s_wifi.default_ap_mac_saved = true;
            POOM_PRINTF_D("Saved default AP MAC");
        }
    }

    err = esp_wifi_start();
    if (err == ESP_ERR_INVALID_STATE)
    {
        err = ESP_OK; /* already started */
    }
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("wifi start failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* =========================
 * Public API
 * ========================= */
esp_err_t poom_wifi_ctrl_register_cb(poom_wifi_ctrl_evt_cb_t cb, void *user_ctx)
{
    esp_err_t err;

    s_evt_cb = cb;
    s_evt_user_ctx = user_ctx;

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        POOM_PRINTF_E("netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = poom_wifi_ctrl_event_loop_init_();
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_evt_handlers_registered)
    {
        return ESP_OK;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &poom_wifi_ctrl_event_handler_,
                                              NULL,
                                              &s_wifi_any_id_inst);
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &poom_wifi_ctrl_event_handler_,
                                              NULL,
                                              &s_ip_got_ip_inst);
    if (err != ESP_OK)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_any_id_inst);
        POOM_PRINTF_E("register IP_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                              &poom_wifi_ctrl_event_handler_,
                                              NULL,
                                              &s_ip_assigned_client_inst);
    if (err != ESP_OK)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_any_id_inst);
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_got_ip_inst);
        POOM_PRINTF_E("register AP DHCP IP handler failed: %s", esp_err_to_name(err));
        return err;
    }

    s_evt_handlers_registered = true;
    POOM_PRINTF_D("Event handlers registered");
    return ESP_OK;
}

esp_err_t poom_wifi_ctrl_unregister_cb(void)
{
    if (!s_evt_handlers_registered)
    {
        s_evt_cb = NULL;
        s_evt_user_ctx = NULL;
        return ESP_OK;
    }

    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_any_id_inst);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_got_ip_inst);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, s_ip_assigned_client_inst);

    s_evt_handlers_registered = false;
    s_evt_cb = NULL;
    s_evt_user_ctx = NULL;

    POOM_PRINTF_D("Event handlers unregistered");
    return ESP_OK;
}

bool poom_wifi_ctrl_sta_is_connected(void)
{
    return s_sta_connected;
}

bool poom_wifi_ctrl_sta_has_ip(void)
{
    return s_sta_has_ip;
}

esp_err_t poom_wifi_ctrl_set_promiscuous(bool enabled)
{
    return esp_wifi_set_promiscuous(enabled);
}

esp_err_t poom_wifi_ctrl_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb)
{
    return esp_wifi_set_promiscuous_rx_cb(cb);
}

esp_err_t poom_wifi_ctrl_init_apsta(void)
{
    return poom_wifi_ctrl_common_init_(WIFI_MODE_APSTA);
}

esp_err_t poom_wifi_ctrl_init_sta(void)
{
    return poom_wifi_ctrl_common_init_(WIFI_MODE_STA);
}

esp_err_t poom_wifi_ctrl_init_null(void)
{
    return poom_wifi_ctrl_common_init_(WIFI_MODE_NULL);
}

esp_err_t poom_wifi_ctrl_deinit(void)
{
    esp_err_t err1;
    esp_err_t err2;

    if (!s_wifi.wifi_initialized)
    {
        return ESP_OK;
    }

    (void)poom_wifi_ctrl_unregister_cb();

    err1 = esp_wifi_stop();
    if (err1 != ESP_OK)
    {
        POOM_PRINTF_W("wifi stop: %s", esp_err_to_name(err1));
    }

    err2 = esp_wifi_deinit();
    if (err2 != ESP_OK)
    {
        POOM_PRINTF_W("wifi deinit: %s", esp_err_to_name(err2));
    }

    s_wifi.wifi_initialized = false;
    s_sta_connected = false;
    s_sta_has_ip = false;

    return (err2 != ESP_OK) ? err2 : err1;
}

esp_err_t poom_wifi_ctrl_manager_ap_config_default(wifi_config_t *out_cfg)
{
    if (out_cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)memset(out_cfg, 0, sizeof(*out_cfg));

    poom_wifi_ctrl_safe_copy_str_(out_cfg->ap.ssid,
                                  sizeof(out_cfg->ap.ssid),
                                  POOM_WIFI_CTRL_MANAGER_AP_SSID);

    out_cfg->ap.ssid_len = (uint8_t)strlen(POOM_WIFI_CTRL_MANAGER_AP_SSID);

    poom_wifi_ctrl_safe_copy_str_(out_cfg->ap.password,
                                  sizeof(out_cfg->ap.password),
                                  POOM_WIFI_CTRL_MANAGER_AP_PASSWORD);

    out_cfg->ap.max_connection = (uint8_t)POOM_WIFI_CTRL_MANAGER_AP_MAX_CONNECTIONS;
    out_cfg->ap.authmode       = POOM_WIFI_CTRL_MANAGER_AP_AUTHMODE;
    out_cfg->ap.channel        = (uint8_t)POOM_WIFI_CTRL_MANAGER_AP_CHANNEL;

    return ESP_OK;
}

esp_err_t poom_wifi_ctrl_manager_ap_start(wifi_config_t *out_cfg)
{
    esp_err_t err;
    wifi_config_t cfg;

    err = poom_wifi_ctrl_manager_ap_config_default(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    err = poom_wifi_ctrl_ap_start(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    if (out_cfg != NULL)
    {
        *out_cfg = cfg;
    }

    return ESP_OK;
}

esp_err_t poom_wifi_ctrl_ap_start(wifi_config_t *ap_cfg)
{
    esp_err_t err;

    if (ap_cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    POOM_PRINTF_I("Starting AP SSID: %s", (const char *)ap_cfg->ap.ssid);

    err = poom_wifi_ctrl_common_init_(WIFI_MODE_AP);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, ap_cfg);
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("AP set config failed: %s", esp_err_to_name(err));
        return err;
    }

#if (POOM_WIFI_CTRL_HAS_MDNS == 1)
    {
        esp_err_t m = setup_mdns();
        if (m != ESP_OK)
        {
            POOM_PRINTF_W("mDNS init failed: %s", esp_err_to_name(m));
        }
    }
#endif

    POOM_PRINTF_I("AP started");
    return ESP_OK;
}

esp_err_t poom_wifi_ctrl_ap_stop(void)
{
    esp_err_t err;
    wifi_config_t ap_cfg;

    (void)memset(&ap_cfg, 0, sizeof(ap_cfg));
    ap_cfg.ap.max_connection = 0U;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK)
    {
        POOM_PRINTF_E("AP stop failed: %s", esp_err_to_name(err));
        return err;
    }

    POOM_PRINTF_I("AP stopped (no clients allowed)");
    return ESP_OK;
}

esp_err_t poom_wifi_ctrl_sta_disconnect(void)
{
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK)
    {
        POOM_PRINTF_W("sta disconnect: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_wifi_ctrl_start_(void)
{
    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_INVALID_STATE)
    {
        err = ESP_OK;
    }
    return err;
}

esp_err_t poom_wifi_ctrl_sta_connect(const char *ssid, const char *password)
{
    esp_err_t err;
    wifi_config_t cfg;

    if (ssid == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_wifi_ctrl_init_sta();
    if (err != ESP_OK)
    {
        return err;
    }

    (void)memset(&cfg, 0, sizeof(cfg));
    poom_wifi_ctrl_safe_copy_str_(cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);

    if (password != NULL)
    {
        poom_wifi_ctrl_safe_copy_str_(cfg.sta.password, sizeof(cfg.sta.password), password);
    }

    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    (void)esp_wifi_disconnect();

    return esp_wifi_connect();
}


esp_err_t poom_wifi_ctrl_set_ap_mac(const uint8_t *ap_mac)
{
    if (ap_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    POOM_PRINTF_D("Setting AP MAC...");
    return esp_wifi_set_mac(WIFI_IF_AP, ap_mac);
}

esp_err_t poom_wifi_ctrl_get_ap_mac(uint8_t *ap_mac)
{
    if (ap_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
}

esp_err_t poom_wifi_ctrl_restore_ap_mac(void)
{
    if (!s_wifi.default_ap_mac_saved)
    {
        return ESP_ERR_INVALID_STATE;
    }

    POOM_PRINTF_D("Restoring AP MAC...");
    return esp_wifi_set_mac(WIFI_IF_AP, s_wifi.default_ap_mac);
}

esp_err_t poom_wifi_ctrl_get_sta_mac(uint8_t *sta_mac)
{
    if (sta_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
}

esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel)
{
    if ((channel < POOM_WIFI_CTRL_WIFI_CHANNEL_MIN) || (channel > POOM_WIFI_CTRL_WIFI_CHANNEL_MAX))
    {
        POOM_PRINTF_E("Channel out of range. Expected <%u..%u> got %u",
                      (unsigned)POOM_WIFI_CTRL_WIFI_CHANNEL_MIN,
                      (unsigned)POOM_WIFI_CTRL_WIFI_CHANNEL_MAX,
                      (unsigned)channel);
        return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}
