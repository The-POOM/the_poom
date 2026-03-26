# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(MODULES
    # Core
    main

    # Drivers/board-level components
    board
    button_driver
    i2c
    sd_card
    ws2812

    # Local modules
    poom_buz_theme
    poom_fw_update
    poom_oled_screen
    poom_sbus
    poom_wifi_attacks
    poom_wifi_ctrl

    # Third-party components
    ble_midi
    lsm6ds3
    mdns

    # Applications
    poom_ble_hid
    poom_ble_keyboard
    poom_ble_plot
    poom_ble_spam
    poom_ble_tracker
    poom_edge_impulse
    poom_game_snake
    poom_http_load_test
    poom_imu_stream
    poom_led_rainbow
    poom_sd_browser
    poom_wifi_arp_poisoner
    poom_wifi_captive
    poom_wifi_deauth
    poom_wifi_spam
    poom_air_drums
)

set(COMPONENTS ${MODULES})

set(POOM_MODULE_DIRS
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_gatt_client
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_gatt_server
    ${CMAKE_SOURCE_DIR}/modules/poom_buz_theme
    ${CMAKE_SOURCE_DIR}/modules/poom_fw_update
    ${CMAKE_SOURCE_DIR}/modules/poom_oled_screen
    ${CMAKE_SOURCE_DIR}/modules/poom_sbus
    ${CMAKE_SOURCE_DIR}/modules/poom_secrets_store
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_attacks
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_ctrl
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_scanner
)

set(POOM_THIRD_PARTY_DIRS
    ${CMAKE_SOURCE_DIR}/third-party/ble_midi
    ${CMAKE_SOURCE_DIR}/third-party/dns_server
    ${CMAKE_SOURCE_DIR}/third-party/drone_id
    ${CMAKE_SOURCE_DIR}/third-party/lsm6ds3
    ${CMAKE_SOURCE_DIR}/third-party/mdns
    ${CMAKE_SOURCE_DIR}/third-party/mdns_manager
    ${CMAKE_SOURCE_DIR}/third-party/oled
)

set(POOM_APPLICATION_DIRS
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_hid
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_keyboard
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_plot
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_spam
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_tracker
    ${CMAKE_SOURCE_DIR}/applications/poom_edge_impulse
    ${CMAKE_SOURCE_DIR}/applications/poom_game_snake
    ${CMAKE_SOURCE_DIR}/applications/poom_http_load_test
    ${CMAKE_SOURCE_DIR}/applications/poom_imu_stream
    ${CMAKE_SOURCE_DIR}/applications/poom_led_rainbow
    ${CMAKE_SOURCE_DIR}/applications/poom_sd_browser
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_arp_poisoner
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_captive
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_deauth
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_spam
    ${CMAKE_SOURCE_DIR}/applications/poom_air_drums
)

set(POOM_DRIVER_DIRS
    ${CMAKE_SOURCE_DIR}/board
    ${CMAKE_SOURCE_DIR}/drivers/button_driver
    ${CMAKE_SOURCE_DIR}/drivers/buzzer
    ${CMAKE_SOURCE_DIR}/drivers/i2c
    ${CMAKE_SOURCE_DIR}/drivers/sd_card
    ${CMAKE_SOURCE_DIR}/drivers/ws2812
)

list(APPEND EXTRA_COMPONENT_DIRS
    ${POOM_MODULE_DIRS}
    ${POOM_THIRD_PARTY_DIRS}
    ${POOM_APPLICATION_DIRS}
    ${POOM_DRIVER_DIRS}
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(poom)
