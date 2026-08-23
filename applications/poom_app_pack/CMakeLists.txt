file(GLOB POOM_APP_PACK_SRCS
    "${COMPONENT_DIR}/src/*.c"
)

if(NOT CONFIG_ZB_ENABLED)
    list(REMOVE_ITEM POOM_APP_PACK_SRCS
        "${COMPONENT_DIR}/src/menu_cli_zigbee.c"
    )
endif()

if(NOT CONFIG_OPENTHREAD_ENABLED OR NOT CONFIG_OPENTHREAD_CLI)
    list(REMOVE_ITEM POOM_APP_PACK_SRCS
        "${COMPONENT_DIR}/src/menu_cli_ot.c"
    )
endif()

list(REMOVE_DUPLICATES POOM_APP_PACK_SRCS)

set(POOM_APP_PACK_REQUIRES
    app_update
    button_driver
    buzzer
    cjson
    poom_cli
    driver
    esp_app_format
    i2c
    ir
    poom_nfc
    poom_picopass
    poom_motion_midi
    poom_ble_keyboard
    poom_ble_plot
    poom_ble_scan
    poom_ble_spam
    poom_ble_tracker
    poom_buz_theme
    poom_drone
    poom_drone_emul
    poom_ieee802154_sniffer
    poom_web
    poom_edge_impulse
    poom_fw_update
    poom_http_load_test
	    poom_imu_stream
	    poom_lua
	    poom_led_rainbow
	    poom_midi
	    poom_arduboy_display
	    poom_sbus
	    poom_sd_browser
    poom_secrets_store
    poom_sniffer_device
    poom_scanner_core
    poom_pcap
    poom_ui_keyboard
    poom_wifi_attacks
    poom_wifi_arp
    poom_wifi_captive
    poom_wifi_ctrl
    poom_wifi_deauth
    poom_wifi_deauth_detector
    poom_wifi_karma
    poom_wifi_scanner
    poom_wifi_spam
    poom_wii
    sd_card
)

set(POOM_APP_PACK_PRIV_REQUIRES)
set(POOM_APP_PACK_PRIV_INCLUDE_DIRS)

if(CONFIG_OPENTHREAD_ENABLED AND CONFIG_OPENTHREAD_CLI)
    list(APPEND POOM_APP_PACK_PRIV_REQUIRES
        console
        esp_driver_gpio
        esp_driver_spi
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_event
        esp_netif
        nvs_flash
        openthread
        vfs
    )

    idf_component_get_property(OPENTHREAD_INCLUDE_DIRS openthread INCLUDE_DIRS)
    list(APPEND POOM_APP_PACK_PRIV_INCLUDE_DIRS
        ${OPENTHREAD_INCLUDE_DIRS}
        "${IDF_PATH}/components/openthread/include"
        "${IDF_PATH}/components/openthread/openthread/include"
        "${IDF_PATH}/components/esp_driver_gpio/include"
        "${IDF_PATH}/components/esp_driver_spi/include"
        "${IDF_PATH}/components/esp_driver_uart/include"
        "${IDF_PATH}/components/esp_driver_usb_serial_jtag/include"
    )
endif()

idf_component_register(
    SRCS ${POOM_APP_PACK_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${POOM_APP_PACK_PRIV_INCLUDE_DIRS}
    REQUIRES ${POOM_APP_PACK_REQUIRES}
    PRIV_REQUIRES ${POOM_APP_PACK_PRIV_REQUIRES}
)
