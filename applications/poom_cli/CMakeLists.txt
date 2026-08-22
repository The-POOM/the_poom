set(srcs
    "src/cli.c"
    "src/cli_nfc.c"
    "src/cli_apps.c"
    "src/cli_config.c"
    "src/cli_drone.c"
)

set(cli_requires
    board
    poom_sbus
    console
    esp_netif
    ir
    nvs_flash
    poom_nfc
    poom_secrets_store
    poom_pcap
    poom_http_load_test
    poom_scanner_core
    poom_drone_emul
    poom_clipper
    poom_ble_spam
    poom_ble_gatt_dynamic
    poom_ble_tracker
    poom_lua
    sd_card
    poom_wifi_captive
    poom_wifi_ctrl
    poom_wifi_deauth
    poom_wifi_deauth_detector
    poom_wifi_karma
    poom_wifi_scanner
    poom_sniffer_device
    poom_wifi_spam
)

if(CONFIG_ZB_ENABLED)
    list(APPEND srcs "src/cli_zigbee.c")
    list(APPEND cli_requires zb_cli)
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES ${cli_requires}
    WHOLE_ARCHIVE
)
