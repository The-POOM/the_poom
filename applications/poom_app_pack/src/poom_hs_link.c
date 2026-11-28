// SPDX-License-Identifier: MIT
/*
 * Private station lifecycle for Handshake Vault's Wi-Fi export.
 *
 * Trimmed, renamed copy of the proven wardrive/WiGLE transfer connect path
 * (modules/poom_wardrive/wardrive_wifi.c). It never touches the stock Wi-Fi
 * manager's callback slot, mDNS service or saved profiles; it owns its own
 * driver instance, event handlers and (borrowed or created) station netif, and
 * restores the prior netif state on teardown. The BT-controller exclusivity
 * guard from the wardrive version is intentionally dropped: Handshake Vault only
 * needs a station link and must coexist with any BLE activity rather than refuse.
 */
#include "poom_hs_link.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static bool owned, loop_owned, netif_owned, wifi_handler, ip_handler;
static esp_netif_t *station, *previous_default;
static esp_event_handler_instance_t wifi_instance, ip_instance;
static atomic_bool has_ip;
static bool borrowed_snapshot;
static esp_netif_ip_info_t previous_ip;
static esp_netif_dns_info_t previous_dns[ESP_NETIF_DNS_MAX];
static esp_netif_dhcp_status_t previous_dhcp;
static _Atomic(poom_wifi_ctrl_evt_cb_t) callback;
static _Atomic(void *) callback_context;

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    poom_wifi_ctrl_evt_info_t info = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        info.evt = POOM_WIFI_CTRL_EVT_STA_CONNECTED;
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        atomic_store(&has_ip, false);
        info.evt = POOM_WIFI_CTRL_EVT_STA_DISCONNECTED;
        if (data)
        {
            info.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ip = data;
        if (!ip || ip->esp_netif != station)
        {
            return;
        }
        atomic_store(&has_ip, true);
        info.evt = POOM_WIFI_CTRL_EVT_STA_GOT_IP;
        info.ip = ip->ip_info.ip;
        info.netmask = ip->ip_info.netmask;
        info.gw = ip->ip_info.gw;
    }
    else
    {
        return;
    }
    poom_wifi_ctrl_evt_cb_t cb = atomic_load(&callback);
    if (cb)
    {
        cb(&info, atomic_load(&callback_context));
    }
}

bool poom_hs_link_owned(void)
{
    return owned || loop_owned || netif_owned || wifi_handler || ip_handler ||
           borrowed_snapshot || station != NULL || previous_default != NULL;
}

bool poom_hs_link_sta_has_ip(void)
{
    return owned && atomic_load(&has_ip);
}

esp_err_t poom_hs_link_register_cb(poom_wifi_ctrl_evt_cb_t cb, void *context)
{
    atomic_store(&callback_context, context);
    atomic_store(&callback, cb);
    return ESP_OK;
}

esp_err_t poom_hs_link_unregister_cb(void)
{
    callback = NULL;
    callback_context = NULL;
    return ESP_OK;
}

esp_err_t poom_hs_link_sta_disconnect(void)
{
    if (!owned)
    {
        return ESP_OK;
    }
    atomic_store(&has_ip, false);
    return esp_wifi_disconnect();
}

esp_err_t poom_hs_link_deinit(void)
{
    esp_err_t result = ESP_OK;
    callback = NULL;
    callback_context = NULL;
    if (owned)
    {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        result = esp_wifi_deinit();
        if (result != ESP_OK)
        {
            return result; /* Retain ownership for a cleanup retry. */
        }
        owned = false;
    }
    if (wifi_handler)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_instance);
        wifi_handler = false;
    }
    if (ip_handler)
    {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_instance);
        ip_handler = false;
    }
    if (borrowed_snapshot && station)
    {
        (void)esp_netif_dhcpc_stop(station);
        result = esp_netif_set_ip_info(station, &previous_ip);
        if (result != ESP_OK)
        {
            return result;
        }
        for (unsigned i = 0; i < ESP_NETIF_DNS_MAX; i++)
        {
            result = esp_netif_set_dns_info(station, (esp_netif_dns_type_t)i, &previous_dns[i]);
            if (result != ESP_OK)
            {
                return result;
            }
        }
        if (previous_dhcp == ESP_NETIF_DHCP_STARTED)
        {
            result = esp_netif_dhcpc_start(station);
            if (result != ESP_OK)
            {
                return result;
            }
        }
        borrowed_snapshot = false;
    }
    if (netif_owned)
    {
        esp_netif_destroy_default_wifi(station);
        netif_owned = false;
    }
    station = NULL;
    if (previous_default && esp_netif_get_default_netif() != previous_default)
    {
        result = esp_netif_set_default_netif(previous_default);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    previous_default = NULL;
    if (loop_owned)
    {
        result = esp_event_loop_delete_default();
        if (result != ESP_OK)
        {
            return result;
        }
        loop_owned = false;
    }
    atomic_store(&has_ip, false);
    return result;
}

esp_err_t poom_hs_link_release_idle_stock(void)
{
    if (poom_hs_link_owned())
    {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT)
    {
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }
    /* NULL has no station/AP interface. Promiscuous capture must also be off;
     * otherwise this may still belong to an active sniffer. */
    if (mode != WIFI_MODE_NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool promiscuous;
    err = esp_wifi_get_promiscuous(&promiscuous);
    if (err != ESP_OK)
    {
        return err;
    }
    if (promiscuous)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Cooperate through the manager, not raw esp_wifi_deinit: its initialised
     * flag and event-handler bookkeeping must stay consistent. If it does not
     * own this driver it leaves it alone, and the mode check below refuses it. */
    err = poom_wifi_ctrl_deinit();
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT)
    {
        /* Driver task stack reclamation is deferred to the FreeRTOS idle task. */
        vTaskDelay(pdMS_TO_TICKS(200));
        return ESP_OK;
    }
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
}

esp_err_t poom_hs_link_init_sta_with_config(const wifi_init_config_t *config)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (owned)
    {
        return ESP_OK;
    }
    if (poom_hs_link_owned())
    {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_ERR_WIFI_NOT_INIT)
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_netif_init();
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK)
    {
        loop_owned = true;
    }
    else if (err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }
    previous_default = esp_netif_get_default_netif();
    station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station)
    {
        err = esp_netif_get_ip_info(station, &previous_ip);
        if (err != ESP_OK)
        {
            goto failed;
        }
        err = esp_netif_dhcpc_get_status(station, &previous_dhcp);
        if (err != ESP_OK)
        {
            goto failed;
        }
        for (unsigned i = 0; i < ESP_NETIF_DNS_MAX; i++)
        {
            err = esp_netif_get_dns_info(station, (esp_netif_dns_type_t)i, &previous_dns[i]);
            if (err != ESP_OK)
            {
                goto failed;
            }
        }
        borrowed_snapshot = true;
    }
    else
    {
        station = esp_netif_create_default_wifi_sta();
        netif_owned = station != NULL;
    }
    if (!station)
    {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    err = esp_wifi_init(config);
    if (err != ESP_OK)
    {
        goto failed;
    }
    owned = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL, &wifi_instance);
    if (err != ESP_OK)
    {
        goto failed;
    }
    wifi_handler = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL, &ip_instance);
    if (err != ESP_OK)
    {
        goto failed;
    }
    ip_handler = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK)
    {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
    }
    if (err == ESP_OK)
    {
        return ESP_OK;
    }
failed:
    (void)poom_hs_link_deinit();
    return err;
}

esp_err_t poom_hs_link_init_sta(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    /* Lean Wi-Fi buffers (the same profile wardrive's "Transfer over Wi-Fi"
     * uses) keep the footprint small, so that after the export tears the radio
     * down another radio app — the detector's handshake capture, which needs a
     * big monitor-mode RX allocation — can still get its memory. The default
     * config reserves far more RX/TX buffer RAM than a file transfer needs. */
    config.static_rx_buf_num = 2;
    config.dynamic_rx_buf_num = 8;
    if (config.dynamic_tx_buf_num > 8)
    {
        config.dynamic_tx_buf_num = 8;
    }
    config.rx_ba_win = 2;
    config.mgmt_sbuf_num = 6;
    config.rx_mgmt_buf_num = 2;
    return poom_hs_link_init_sta_with_config(&config);
}

esp_err_t poom_hs_link_sta_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strnlen(ssid, 33) > 32 ||
        (password && strnlen(password, 65) > 64))
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = poom_hs_link_init_sta();
    if (err != ESP_OK)
    {
        return err;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    size_t n = password ? strlen(password) : 0;
    if (n)
    {
        memcpy(config.sta.password, password, n);
    }
    config.sta.threshold.authmode = n ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    volatile unsigned char *bytes = (volatile unsigned char *)&config;
    for (size_t i = 0; i < sizeof(config); ++i)
    {
        bytes[i] = 0;
    }
    if (err != ESP_OK)
    {
        return err;
    }
    (void)esp_wifi_disconnect();
    return esp_wifi_connect();
}
