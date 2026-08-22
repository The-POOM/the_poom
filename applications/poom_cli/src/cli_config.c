// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "argtable3/argtable3.h"
#include "cli.h"
#include "cli_config.h"
#include "esp_console.h"
#include "esp_err.h"
#include "poom_pcap_manager.h"
#include "poom_http_load_test.h"
#include "poom_secrets_store.h"
#include "poom_wifi_spam.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    struct arg_str* ssid;
    struct arg_str* password;
    struct arg_end* end;
} cli_cfg_wifi_set_args_t;

typedef struct
{
    struct arg_str* token;
    struct arg_end* end;
} cli_cfg_edge_token_set_args_t;

static cli_cfg_wifi_set_args_t s_cli_cfg_wifi_set_args;
static cli_cfg_edge_token_set_args_t s_cli_cfg_edge_token_set_args;

typedef struct
{
    struct arg_str* host;
    struct arg_str* port;
    struct arg_str* path;
    struct arg_int* workers;
    struct arg_end* end;
} cli_cfg_load_target_set_args_t;

static cli_cfg_load_target_set_args_t s_cli_cfg_load_target_set_args;

typedef struct
{
    struct arg_int* channel;
    struct arg_end* end;
} cli_cfg_154_channel_args_t;

static cli_cfg_154_channel_args_t s_cli_cfg_154_channel_args;

#define CLI_CFG_LOAD_KEY_HOST "ld_host"
#define CLI_CFG_LOAD_KEY_PORT "ld_port"
#define CLI_CFG_LOAD_KEY_PATH "ld_path"
#define CLI_CFG_LOAD_KEY_WORKERS "ld_workers"

#define CLI_CFG_LOAD_HOST_BUF_LEN (128U)
#define CLI_CFG_LOAD_PORT_BUF_LEN (16U)
#define CLI_CFG_LOAD_PATH_BUF_LEN (128U)
#define CLI_CFG_LOAD_WIFI_SSID_BUF_LEN (64U)
#define CLI_CFG_LOAD_WIFI_PASS_BUF_LEN (128U)
#define CLI_CFG_LOAD_DEFAULT_WORKERS (8U)
#define CLI_CFG_LOAD_MAX_WORKERS (16U)
#define CLI_CFG_LOAD_SCHEME_HTTP_STR "http"

typedef enum
{
    CLI_CFG_154_UNUSED = 0,
} cli_cfg_154_mode_t;

static uint8_t s_cli_cfg_154_channel = POOM_PCAP_IEEE802154_CHANNEL_DEFAULT;

/**
 * @brief Internal helper for `cmd_cfg_wifi_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_wifi_set(int argc, char** argv)
{
    int parse_errors;
    esp_err_t err;

    parse_errors = arg_parse(argc, argv, (void**)&s_cli_cfg_wifi_set_args);
    if(parse_errors != 0)
    {
        arg_print_errors(stderr, s_cli_cfg_wifi_set_args.end, argv[0]);
        printf("Usage: cfg-wifi-set <ssid> <password>\n");
        return 1;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_wifi_ssid(s_cli_cfg_wifi_set_args.ssid->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save SSID: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_wifi_pass(s_cli_cfg_wifi_set_args.password->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save password: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Wi-Fi credentials saved.\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_cfg_edge_token_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_edge_token_set(int argc, char** argv)
{
    int parse_errors;
    esp_err_t err;

    parse_errors = arg_parse(argc, argv, (void**)&s_cli_cfg_edge_token_set_args);
    if(parse_errors != 0)
    {
        arg_print_errors(stderr, s_cli_cfg_edge_token_set_args.end, argv[0]);
        printf("Usage: cfg-edge-token-set <token>\n");
        return 1;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_api_token(s_cli_cfg_edge_token_set_args.token->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save Edge token: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Edge token saved.\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_cfg_wifi_get`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_wifi_get(int argc, char** argv)
{
    char ssid[128];
    size_t ssid_len;
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    ssid_len = sizeof(ssid);
    err = poom_secrets_get_wifi_ssid(ssid, &ssid_len);

    if(err == ESP_ERR_NOT_FOUND)
    {
        printf("SSID not found.\n");
        return 0;
    }

    printf("SSID: %s\n", ssid);
    return 0;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] key Parameter passed to the function.
 * @param[in] out_value Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return int
 */
static int cli_cfg_load_read_str_(const char* key, char* out_value, size_t out_len)
{
    size_t value_len = out_len;
    esp_err_t err;

    if((key == NULL) || (out_value == NULL) || (out_len == 0U))
    {
        return 1;
    }

    err = poom_secrets_get_str(key, out_value, &value_len);
    if(err != ESP_OK)
    {
        return 1;
    }

    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_load_target_set(int argc, char** argv)
{
    int parse_errors;
    int workers;
    esp_err_t err;

    parse_errors = arg_parse(argc, argv, (void**)&s_cli_cfg_load_target_set_args);
    if(parse_errors != 0)
    {
        arg_print_errors(stderr, s_cli_cfg_load_target_set_args.end, argv[0]);
        printf("Usage: cfg-load-target-set <host> <port> <path> [workers]\n");
        return 1;
    }

    workers = (s_cli_cfg_load_target_set_args.workers->count > 0)
                  ? s_cli_cfg_load_target_set_args.workers->ival[0]
                  : (int)CLI_CFG_LOAD_DEFAULT_WORKERS;
    if((workers <= 0) || (workers > (int)CLI_CFG_LOAD_MAX_WORKERS))
    {
        printf("Invalid workers. Valid range: 1..%u.\n", (unsigned)CLI_CFG_LOAD_MAX_WORKERS);
        return 1;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_str(CLI_CFG_LOAD_KEY_HOST, s_cli_cfg_load_target_set_args.host->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save host: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_str(CLI_CFG_LOAD_KEY_PORT, s_cli_cfg_load_target_set_args.port->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save port: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_str(CLI_CFG_LOAD_KEY_PATH, s_cli_cfg_load_target_set_args.path->sval[0]);
    if(err != ESP_OK)
    {
        printf("Failed to save path: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = poom_secrets_set_u32(CLI_CFG_LOAD_KEY_WORKERS, (uint32_t)workers);
    if(err != ESP_OK)
    {
        printf("Failed to save workers: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Load target saved (%s).\n", CLI_CFG_LOAD_SCHEME_HTTP_STR);
    return 0;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_load_get(int argc, char** argv)
{
    char ssid[CLI_CFG_LOAD_WIFI_SSID_BUF_LEN];
    char host[CLI_CFG_LOAD_HOST_BUF_LEN];
    char port[CLI_CFG_LOAD_PORT_BUF_LEN];
    char path[CLI_CFG_LOAD_PATH_BUF_LEN];
    uint32_t workers = CLI_CFG_LOAD_DEFAULT_WORKERS;
    size_t ssid_len;
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    ssid_len = sizeof(ssid);
    err = poom_secrets_get_wifi_ssid(ssid, &ssid_len);
    if(err == ESP_OK)
    {
        printf("SSID: %s\n", ssid);
    }
    else
    {
        printf("SSID: <not set>\n");
    }

    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_HOST, host, sizeof(host)) == 0)
    {
        printf("Host: %s\n", host);
    }
    else
    {
        printf("Host: <not set>\n");
    }

    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_PORT, port, sizeof(port)) == 0)
    {
        printf("Port: %s\n", port);
    }
    else
    {
        printf("Port: <not set>\n");
    }

    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_PATH, path, sizeof(path)) == 0)
    {
        printf("Path: %s\n", path);
    }
    else
    {
        printf("Path: <not set>\n");
    }

    err = poom_secrets_get_u32(CLI_CFG_LOAD_KEY_WORKERS, &workers);
    if(err != ESP_OK)
    {
        workers = CLI_CFG_LOAD_DEFAULT_WORKERS;
    }
    printf("Workers: %u\n", (unsigned)workers);

    printf("Scheme: %s\n", CLI_CFG_LOAD_SCHEME_HTTP_STR);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_load_start(int argc, char** argv)
{
    char ssid[CLI_CFG_LOAD_WIFI_SSID_BUF_LEN];
    char password[CLI_CFG_LOAD_WIFI_PASS_BUF_LEN];
    char host[CLI_CFG_LOAD_HOST_BUF_LEN];
    char port[CLI_CFG_LOAD_PORT_BUF_LEN];
    char path[CLI_CFG_LOAD_PATH_BUF_LEN];
    size_t ssid_len;
    size_t pass_len;
    uint32_t workers = CLI_CFG_LOAD_DEFAULT_WORKERS;
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        printf("NVS init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    ssid_len = sizeof(ssid);
    err = poom_secrets_get_wifi_ssid(ssid, &ssid_len);
    if(err != ESP_OK)
    {
        printf("Missing SSID. Use cfg-wifi-set first.\n");
        return 1;
    }

    pass_len = sizeof(password);
    err = poom_secrets_get_wifi_pass(password, &pass_len);
    if(err != ESP_OK)
    {
        printf("Missing Wi-Fi password. Use cfg-wifi-set first.\n");
        return 1;
    }

    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_HOST, host, sizeof(host)) != 0)
    {
        printf("Missing host. Use cfg-load-target-set first.\n");
        return 1;
    }
    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_PORT, port, sizeof(port)) != 0)
    {
        printf("Missing port. Use cfg-load-target-set first.\n");
        return 1;
    }
    if(cli_cfg_load_read_str_(CLI_CFG_LOAD_KEY_PATH, path, sizeof(path)) != 0)
    {
        printf("Missing path. Use cfg-load-target-set first.\n");
        return 1;
    }

    err = poom_secrets_get_u32(CLI_CFG_LOAD_KEY_WORKERS, &workers);
    if((err != ESP_OK) || (workers == 0U) || (workers > CLI_CFG_LOAD_MAX_WORKERS))
    {
        workers = CLI_CFG_LOAD_DEFAULT_WORKERS;
    }

    poom_http_load_test_config_t cfg = {
        .ssid = ssid,
        .password = password,
        .host = host,
        .port = port,
        .path = path,
        .worker_count = (uint8_t)workers,
    };

    err = poom_http_load_test_start(&cfg);
    if(err != ESP_OK)
    {
        printf("Load test start failed (%s): %s\n",
               CLI_CFG_LOAD_SCHEME_HTTP_STR,
               esp_err_to_name(err));
        return 1;
    }

    printf("Load test started: http://%s:%s%s (workers=%u)\n",
           host,
           port,
           path,
           (unsigned)cfg.worker_count);
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_load_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_http_load_test_stop();
    printf("Load test stopped (%s).\n", CLI_CFG_LOAD_SCHEME_HTTP_STR);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_154_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(poom_pcap_manager_sniffer_is_active())
    {
        printf("PCAP sniffer already running.\n");
        return 0;
    }

    if((s_cli_cfg_154_channel < POOM_PCAP_IEEE802154_CHANNEL_MIN) || (s_cli_cfg_154_channel > POOM_PCAP_IEEE802154_CHANNEL_MAX))
    {
        s_cli_cfg_154_channel = POOM_PCAP_IEEE802154_CHANNEL_DEFAULT;
    }

    esp_err_t err = poom_pcap_manager_sniffer_start_zigbee(s_cli_cfg_154_channel, false, 0U);
    if(err != ESP_OK)
    {
        printf("IEEE 802.15.4 PCAP start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    const char* path = poom_pcap_manager_get_file_path();
    printf("IEEE 802.15.4 PCAP started (%s) on channel %u.\n",
           (path != NULL) ? "SD" : "UART",
           (unsigned)poom_pcap_manager_sniffer_zigbee_get_channel());
    if(path != NULL)
    {
        printf("File: %s\n", path);
    }
    return 0;
#else
    printf("IEEE 802.15.4 capture not supported on this target.\n");
    return 1;
#endif
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_154_hop_start(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(poom_pcap_manager_sniffer_is_active())
    {
        printf("PCAP sniffer already running.\n");
        return 0;
    }

    esp_err_t err = poom_pcap_manager_sniffer_start_zigbee(0U, true, 0U);
    if(err != ESP_OK)
    {
        printf("IEEE 802.15.4 hopping PCAP start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    const char* path = poom_pcap_manager_get_file_path();
    printf("IEEE 802.15.4 PCAP started (%s) in hopping mode (11..26).\n", (path != NULL) ? "SD" : "UART");
    if(path != NULL)
    {
        printf("File: %s\n", path);
    }
    return 0;
#else
    printf("IEEE 802.15.4 capture not supported on this target.\n");
    return 1;
#endif
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_154_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(poom_pcap_manager_sniffer_zigbee_get_channel() == 0U)
    {
        printf("IEEE 802.15.4 sniffer is not running.\n");
        return 0;
    }

    (void)poom_pcap_manager_sniffer_stop();
    printf("IEEE 802.15.4 sniffer stopped.\n");
    return 0;
#else
    printf("IEEE 802.15.4 capture not supported on this target.\n");
    return 1;
#endif
}

/**
 * @brief Internal helper for `cmd_cfg_154_channel`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_154_channel(int argc, char** argv)
{
    int parse_errors;
    int requested_channel;

    parse_errors = arg_parse(argc, argv, (void**)&s_cli_cfg_154_channel_args);
    if(parse_errors != 0)
    {
        arg_print_errors(stderr, s_cli_cfg_154_channel_args.end, argv[0]);
        printf("Usage: cfg-154-ch <11..26>\n");
        return 1;
    }

    requested_channel = s_cli_cfg_154_channel_args.channel->ival[0];
    if((requested_channel < (int)POOM_PCAP_IEEE802154_CHANNEL_MIN) || (requested_channel > (int)POOM_PCAP_IEEE802154_CHANNEL_MAX))
    {
        printf("Invalid channel. Valid range: %d..%d.\n",
               (int)POOM_PCAP_IEEE802154_CHANNEL_MIN,
               (int)POOM_PCAP_IEEE802154_CHANNEL_MAX);
        return 1;
    }

    s_cli_cfg_154_channel = (uint8_t)requested_channel;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(poom_pcap_manager_sniffer_is_active() && (poom_pcap_manager_sniffer_zigbee_get_channel() != 0U))
    {
        esp_err_t err = poom_pcap_manager_sniffer_zigbee_set_channel((uint8_t)requested_channel);
        if(err != ESP_OK)
        {
            printf("IEEE 802.15.4 channel set failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("IEEE 802.15.4 channel set to %u.\n", (unsigned)poom_pcap_manager_sniffer_zigbee_get_channel());
        return 0;
    }
#endif

    printf("IEEE 802.15.4 channel set to %u (will apply on next start).\n", (unsigned)s_cli_cfg_154_channel);
    return 0;
}

/**
 * @brief Internal helper for `cmd_cfg_154_rssi`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_cfg_154_rssi(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if(!poom_pcap_manager_sniffer_is_active() || (poom_pcap_manager_sniffer_zigbee_get_channel() == 0U))
    {
        printf("IEEE 802.15.4 sniffer is not running.\n");
        return 0;
    }

    printf("IEEE 802.15.4 RSSI: %d dBm (channel %u)\n",
           poom_pcap_manager_sniffer_zigbee_get_rssi(),
           (unsigned)poom_pcap_manager_sniffer_zigbee_get_channel());
    return 0;
#else
    printf("IEEE 802.15.4 capture not supported on this target.\n");
    return 0;
#endif
}

// =========================================================
// Wi-Fi spam SSID list (persisted)
// =========================================================

/**
 * @brief Internal helper for `cmd_wifi_spam_ssids_show`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_ssids_show(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    poom_wifi_spam_ssid_list_t list;
    (void)memset(&list, 0, sizeof(list));

    const esp_err_t err = poom_wifi_spam_ssids_get(&list);
    if(err != ESP_OK)
    {
        printf("wifi-spam-ssids-show: failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("wifi spam ssids: %u/%u\n", (unsigned)list.count, (unsigned)POOM_WIFI_SPAM_SSIDS_MAX);
    for(uint8_t i = 0U; i < list.count; i++)
    {
        printf("  [%u] %s\n", (unsigned)i, list.ssids[i]);
    }
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_spam_ssids_reset`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_ssids_reset(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t err = poom_wifi_spam_ssids_reset_defaults();
    if(err != ESP_OK)
    {
        printf("wifi-spam-ssids-reset: failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("wifi spam ssids reset to defaults\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_spam_ssids_del`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_ssids_del(int argc, char** argv)
{
    if(argc < 2)
    {
        printf("Usage: wifi-spam-ssids-del <index>\n");
        return 1;
    }

    char* endp = NULL;
    const long idx = strtol(argv[1], &endp, 10);
    if((endp == NULL) || (*endp != '\0') || (idx < 0) || (idx > 255))
    {
        printf("Invalid index\n");
        return 1;
    }

    const esp_err_t err = poom_wifi_spam_ssids_remove((uint8_t)idx);
    if(err != ESP_OK)
    {
        printf("wifi-spam-ssids-del: failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("deleted index %ld\n", idx);
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_spam_ssids_add`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_ssids_add(int argc, char** argv)
{
    if(argc < 2)
    {
        printf("Usage: wifi-spam-ssids-add <ssid>\n");
        return 1;
    }

    const esp_err_t err = poom_wifi_spam_ssids_add(argv[1]);
    if(err != ESP_OK)
    {
        printf("wifi-spam-ssids-add: failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("added ssid\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_wifi_spam_ssids_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_wifi_spam_ssids_set(int argc, char** argv)
{
    if(argc < 2)
    {
        printf("Usage: wifi-spam-ssids-set <ssid1> [ssid2] ...\n");
        printf("Max %u entries, each up to %u chars.\n",
               (unsigned)POOM_WIFI_SPAM_SSIDS_MAX,
               (unsigned)POOM_WIFI_SPAM_SSID_MAX_LEN);
        return 1;
    }

    const int n = argc - 1;
    if(n > (int)POOM_WIFI_SPAM_SSIDS_MAX)
    {
        printf("Too many SSIDs (max %u)\n", (unsigned)POOM_WIFI_SPAM_SSIDS_MAX);
        return 1;
    }

    poom_wifi_spam_ssid_list_t list;
    (void)memset(&list, 0, sizeof(list));
    list.count = (uint8_t)n;

    for(int i = 0; i < n; i++)
    {
        const char* s = argv[i + 1];
        if((s == NULL) || (s[0] == '\0'))
        {
            printf("Empty SSID at position %d\n", i + 1);
            return 1;
        }
        if(strlen(s) > (size_t)POOM_WIFI_SPAM_SSID_MAX_LEN)
        {
            printf("SSID too long at position %d (max %u)\n", i + 1, (unsigned)POOM_WIFI_SPAM_SSID_MAX_LEN);
            return 1;
        }
        (void)snprintf(list.ssids[i], sizeof(list.ssids[i]), "%s", s);
    }

    const esp_err_t err = poom_wifi_spam_ssids_set(&list);
    if(err != ESP_OK)
    {
        printf("wifi-spam-ssids-set: failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("wifi spam ssids updated (%d)\n", n);
    return 0;
}

void cli_poom_config_register_cmds(void)
{
    s_cli_cfg_wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "Wi-Fi SSID");
    s_cli_cfg_wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "Wi-Fi password");
    s_cli_cfg_wifi_set_args.end = arg_end(2);

    const esp_console_cmd_t cfg_wifi_set_cmd = {
        .command = "cfg-wifi-set",
        .help = "Save Wi-Fi credentials to secrets store",
        .hint = NULL,
        .func = &cmd_cfg_wifi_set,
        .argtable = &s_cli_cfg_wifi_set_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_wifi_set_cmd));

    const esp_console_cmd_t cfg_wifi_get_cmd = {
        .command = "cfg-wifi-get",
        .help = "Show stored Wi-Fi SSID",
        .hint = NULL,
        .func = &cmd_cfg_wifi_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_wifi_get_cmd));

    s_cli_cfg_edge_token_set_args.token = arg_str1(NULL, NULL, "<token>", "Edge Impulse API token");
    s_cli_cfg_edge_token_set_args.end = arg_end(1);

    s_cli_cfg_load_target_set_args.host = arg_str1(NULL, NULL, "<host>", "Target host or IP");
    s_cli_cfg_load_target_set_args.port = arg_str1(NULL, NULL, "<port>", "Target TCP port");
    s_cli_cfg_load_target_set_args.path = arg_str1(NULL, NULL, "<path>", "HTTP path (e.g. /)");
    s_cli_cfg_load_target_set_args.workers = arg_int0(NULL, NULL, "<workers>", "Parallel workers (1..16)");
    s_cli_cfg_load_target_set_args.end = arg_end(3);

    const esp_console_cmd_t cfg_edge_token_set_cmd = {
        .command = "cfg-edge-token-set",
        .help = "Save Edge Impulse API token to secrets store",
        .hint = NULL,
        .func = &cmd_cfg_edge_token_set,
        .argtable = &s_cli_cfg_edge_token_set_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_edge_token_set_cmd));

    const esp_console_cmd_t cfg_edge_api_key_set_cmd = {
        .command = "cfg-edge-api-key-set",
        .help = "Save POOM_EDGE_IMPULSE_API_KEY token to secrets store",
        .hint = NULL,
        .func = &cmd_cfg_edge_token_set,
        .argtable = &s_cli_cfg_edge_token_set_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_edge_api_key_set_cmd));

    const esp_console_cmd_t cfg_load_target_set_cmd = {
        .command = "cfg-load-target-set",
        .help = "Save HTTP load-test target/profile (host/port/path/workers)",
        .hint = NULL,
        .func = &cmd_cfg_load_target_set,
        .argtable = &s_cli_cfg_load_target_set_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_load_target_set_cmd));

    const esp_console_cmd_t cfg_load_get_cmd = {
        .command = "cfg-load-get",
        .help = "Show stored load-test config",
        .hint = NULL,
        .func = &cmd_cfg_load_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_load_get_cmd));

    const esp_console_cmd_t cfg_load_start_cmd = {
        .command = "cfg-load-start",
        .help = "Start HTTP load test with stored config",
        .hint = NULL,
        .func = &cmd_cfg_load_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_load_start_cmd));

    const esp_console_cmd_t cfg_load_stop_cmd = {
        .command = "cfg-load-stop",
        .help = "Stop HTTP load test",
        .hint = NULL,
        .func = &cmd_cfg_load_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_load_stop_cmd));

    s_cli_cfg_154_channel_args.channel = arg_int1(NULL, NULL, "<11..26>", "IEEE 802.15.4 channel");
    s_cli_cfg_154_channel_args.end = arg_end(1);

    const esp_console_cmd_t cfg_154_start_cmd = {
        .command = "cfg-154-start",
        .help = "Start IEEE 802.15.4 sniffer in fixed channel mode",
        .hint = NULL,
        .func = &cmd_cfg_154_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_154_start_cmd));

    const esp_console_cmd_t cfg_154_hop_start_cmd = {
        .command = "cfg-154-hop-start",
        .help = "Start IEEE 802.15.4 sniffer in channel hopping mode",
        .hint = NULL,
        .func = &cmd_cfg_154_hop_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_154_hop_start_cmd));

    const esp_console_cmd_t cfg_154_stop_cmd = {
        .command = "cfg-154-stop",
        .help = "Stop IEEE 802.15.4 sniffer",
        .hint = NULL,
        .func = &cmd_cfg_154_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_154_stop_cmd));

    const esp_console_cmd_t cfg_154_ch_cmd = {
        .command = "cfg-154-ch",
        .help = "Set IEEE 802.15.4 channel (11..26)",
        .hint = NULL,
        .func = &cmd_cfg_154_channel,
        .argtable = &s_cli_cfg_154_channel_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_154_ch_cmd));

    const esp_console_cmd_t cfg_154_rssi_cmd = {
        .command = "cfg-154-rssi",
        .help = "Read current IEEE 802.15.4 RSSI",
        .hint = NULL,
        .func = &cmd_cfg_154_rssi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_154_rssi_cmd));

    const esp_console_cmd_t wifi_spam_ssids_show_cmd = {
        .command = "wifi-spam-ssids-show",
        .help = "Show SSID list used by poom_wifi_spam (persisted)",
        .hint = NULL,
        .func = &cmd_wifi_spam_ssids_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_ssids_show_cmd));

    const esp_console_cmd_t wifi_spam_ssids_set_cmd = {
        .command = "wifi-spam-ssids-set",
        .help = "Replace SSID list used by poom_wifi_spam (persisted)",
        .hint = NULL,
        .func = &cmd_wifi_spam_ssids_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_ssids_set_cmd));

    const esp_console_cmd_t wifi_spam_ssids_add_cmd = {
        .command = "wifi-spam-ssids-add",
        .help = "Append one SSID to poom_wifi_spam list (persisted)",
        .hint = NULL,
        .func = &cmd_wifi_spam_ssids_add,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_ssids_add_cmd));

    const esp_console_cmd_t wifi_spam_ssids_del_cmd = {
        .command = "wifi-spam-ssids-del",
        .help = "Delete one SSID by index from poom_wifi_spam list (persisted)",
        .hint = NULL,
        .func = &cmd_wifi_spam_ssids_del,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_ssids_del_cmd));

    const esp_console_cmd_t wifi_spam_ssids_reset_cmd = {
        .command = "wifi-spam-ssids-reset",
        .help = "Reset poom_wifi_spam SSID list to built-in defaults",
        .hint = NULL,
        .func = &cmd_wifi_spam_ssids_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_spam_ssids_reset_cmd));
}

void cli_poom_config_option(void)
{
    poom_console_begin(cli_poom_config_register_cmds);
}
