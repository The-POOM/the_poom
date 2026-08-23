# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

#
# Firmware version (PROJECT_VER)
# - Source of truth for runtime reporting via esp_app_get_description()->version
# - Default: <version>-r<release>-<commit_count>, prefixed with D when dirty
#            Example: 1.0-r3-428, 1.0-r3-D428
# - Override: set env var POOM_FIRMWARE_VERSION
#
set(POOM_FIRMWARE_VERSION "")
set(POOM_FW_VERSION "1.0")
set(POOM_FW_RELEASE "4")

if(DEFINED ENV{POOM_FIRMWARE_VERSION} AND NOT "$ENV{POOM_FIRMWARE_VERSION}" STREQUAL "")
    set(POOM_FIRMWARE_VERSION "$ENV{POOM_FIRMWARE_VERSION}")
else()
    set(POOM_GIT_COMMIT_COUNT "0")
    set(POOM_GIT_DIRTY_PREFIX "")

    execute_process(
        COMMAND git rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE POOM_GIT_COMMIT_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE POOM_GIT_COMMIT_COUNT_RES
        ERROR_QUIET
    )

    if(NOT POOM_GIT_COMMIT_COUNT_RES EQUAL 0 OR "${POOM_GIT_COMMIT_COUNT}" STREQUAL "")
        set(POOM_GIT_COMMIT_COUNT "0")
    endif()

    execute_process(
        COMMAND git diff --quiet --ignore-submodules HEAD --
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE POOM_GIT_DIRTY_RES
        ERROR_QUIET
    )
    if(POOM_GIT_DIRTY_RES EQUAL 1)
        set(POOM_GIT_DIRTY_PREFIX "D")
    endif()

    string(STRIP "${POOM_FW_VERSION}" POOM_FW_VERSION)
    string(STRIP "${POOM_FW_RELEASE}" POOM_FW_RELEASE)
    string(REGEX REPLACE "[^0-9A-Za-z._-]" "_" POOM_FW_VERSION "${POOM_FW_VERSION}")
    string(REGEX REPLACE "[^0-9A-Za-z._-]" "_" POOM_FW_RELEASE "${POOM_FW_RELEASE}")
    string(REGEX REPLACE "[^0-9]" "" POOM_GIT_COMMIT_COUNT "${POOM_GIT_COMMIT_COUNT}")

    if("${POOM_FW_VERSION}" STREQUAL "")
        set(POOM_FW_VERSION "1.0")
    endif()
    if("${POOM_FW_RELEASE}" STREQUAL "")
        set(POOM_FW_RELEASE "0")
    endif()
    if("${POOM_GIT_COMMIT_COUNT}" STREQUAL "")
        set(POOM_GIT_COMMIT_COUNT "0")
    endif()

    set(POOM_FIRMWARE_VERSION
        "${POOM_FW_VERSION}-r${POOM_FW_RELEASE}-${POOM_GIT_DIRTY_PREFIX}${POOM_GIT_COMMIT_COUNT}")
endif()

string(REGEX REPLACE "[^0-9A-Za-z.+-]" "_" POOM_FIRMWARE_VERSION "${POOM_FIRMWARE_VERSION}")
string(LENGTH "${POOM_FIRMWARE_VERSION}" POOM_FIRMWARE_VERSION_LEN)
if(POOM_FIRMWARE_VERSION_LEN GREATER 31)
    string(SUBSTRING "${POOM_FIRMWARE_VERSION}" 0 31 POOM_FIRMWARE_VERSION)
endif()
set(PROJECT_VER "${POOM_FIRMWARE_VERSION}")

set(MODULES
    # Core
    main
    memory
    task

    # Drivers/board-level components
    board
    button_driver
    i2c
    sd_card
    ws2812

    # Local modules
    nfcal
    poom_ble_scan
    poom_buz_theme
    poom_button_sound
    poom_web
    poom_clipper
    poom_lua
    poom_fw_update
    poom_arduboy_display
    poom_ui_keyboard
    poom_sbus
    poom_host_sniffer_transport
    poom_ieee802154_sniffer
    poom_scanner_core
    poom_sniffer_device
    poom_uart_sniffer
    poom_wifi_attacks
    poom_wifi_ctrl
    poom_wifi_mac
# zigbee jonathan
#    zb_cli
#    open_thread
#-----------------
    # Third-party components
    ble_midi
# zigbee jonathan
#    esp-zigbee-console
#-----------------
    lua
    lsm6ds3
    mdns
    pcap
    
    # Applications
    poom_cli
    poom_app_pack
    poom_breakout
    poom_menu
    poom_nfc
    poom_picopass
    poom_motion_midi
    poom_ble_hid
    poom_ble_keyboard
    poom_ble_plot
    poom_ble_spam
    poom_ble_tracker
    poom_drone
    poom_edge_impulse
    poom_http_load_test
    poom_imu_stream
    poom_led_rainbow
    poom_sd_browser
    poom_wifi_captive
    poom_wifi_deauth
    poom_wifi_deauth_detector
    poom_wifi_karma
    poom_wifi_spam
    poom_wii
    poom_wifi_arp
    poom_pcap
)

set(COMPONENTS ${MODULES})

set(POOM_MODULE_DIRS
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_scan
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_gatt_client
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_gatt_dynamic
    ${CMAKE_SOURCE_DIR}/modules/poom_ble_gatt_server
    ${CMAKE_SOURCE_DIR}/modules/poom_buz_theme
    ${CMAKE_SOURCE_DIR}/modules/poom_button_sound
    ${CMAKE_SOURCE_DIR}/modules/poom_espnow
    ${CMAKE_SOURCE_DIR}/modules/poom_midi
    ${CMAKE_SOURCE_DIR}/modules/poom_web
    ${CMAKE_SOURCE_DIR}/modules/poom_clipper
    ${CMAKE_SOURCE_DIR}/modules/poom_lua
    ${CMAKE_SOURCE_DIR}/modules/poom_fw_update
    ${CMAKE_SOURCE_DIR}/modules/poom_arduboy_display
    ${CMAKE_SOURCE_DIR}/modules/poom_ui_keyboard
    ${CMAKE_SOURCE_DIR}/modules/poom_sbus
    ${CMAKE_SOURCE_DIR}/modules/poom_host_sniffer_transport
    ${CMAKE_SOURCE_DIR}/modules/poom_ieee802154_sniffer
    ${CMAKE_SOURCE_DIR}/modules/poom_scanner_core
    ${CMAKE_SOURCE_DIR}/modules/poom_secrets_store
    ${CMAKE_SOURCE_DIR}/modules/poom_sniffer_device
    ${CMAKE_SOURCE_DIR}/modules/poom_uart_sniffer
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_attacks
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_ctrl
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_mac
    ${CMAKE_SOURCE_DIR}/modules/poom_wifi_scanner
    ${CMAKE_SOURCE_DIR}/modules/poom_pcap
# zigbee jonathan
#    ${CMAKE_SOURCE_DIR}/modules/open_thread
# -----------------------------------
)

set(POOM_THIRD_PARTY_DIRS
    ${CMAKE_SOURCE_DIR}/third-party/nfcal
    ${CMAKE_SOURCE_DIR}/third-party/ble_midi
    ${CMAKE_SOURCE_DIR}/third-party/dns_server
    ${CMAKE_SOURCE_DIR}/third-party/drone_id
    ${CMAKE_SOURCE_DIR}/third-party/esp-zigbee-console
    ${CMAKE_SOURCE_DIR}/third-party/lua
    ${CMAKE_SOURCE_DIR}/third-party/lsm6ds3
    ${CMAKE_SOURCE_DIR}/third-party/mdns
    ${CMAKE_SOURCE_DIR}/third-party/mdns_manager
    ${CMAKE_SOURCE_DIR}/third-party/zb_cli
    ${CMAKE_SOURCE_DIR}/third-party/oled
    ${CMAKE_SOURCE_DIR}/third-party/pcap
)

set(POOM_APPLICATION_DIRS
    ${CMAKE_SOURCE_DIR}/applications/poom_cli
    ${CMAKE_SOURCE_DIR}/applications/poom_app_pack
    ${CMAKE_SOURCE_DIR}/applications/poom_breakout
    ${CMAKE_SOURCE_DIR}/applications/poom_menu
    ${CMAKE_SOURCE_DIR}/applications/poom_nfc
    ${CMAKE_SOURCE_DIR}/applications/poom_picopass
    ${CMAKE_SOURCE_DIR}/applications/poom_motion_midi
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_hid
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_keyboard
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_plot
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_spam
    ${CMAKE_SOURCE_DIR}/applications/poom_ble_tracker
    ${CMAKE_SOURCE_DIR}/applications/poom_drone
    ${CMAKE_SOURCE_DIR}/applications/poom_drone_emul
    ${CMAKE_SOURCE_DIR}/applications/poom_edge_impulse
    ${CMAKE_SOURCE_DIR}/applications/poom_http_load_test
    ${CMAKE_SOURCE_DIR}/applications/poom_imu_stream
    ${CMAKE_SOURCE_DIR}/applications/poom_led_rainbow
    ${CMAKE_SOURCE_DIR}/applications/poom_sd_browser
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_captive
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_deauth
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_deauth_detector
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_karma
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_spam
    ${CMAKE_SOURCE_DIR}/applications/poom_wii
    ${CMAKE_SOURCE_DIR}/applications/poom_wifi_arp
)

set(POOM_DRIVER_DIRS
    ${CMAKE_SOURCE_DIR}/board
    ${CMAKE_SOURCE_DIR}/drivers/button_driver
    ${CMAKE_SOURCE_DIR}/drivers/buzzer
    ${CMAKE_SOURCE_DIR}/drivers/i2c
    ${CMAKE_SOURCE_DIR}/drivers/ir
    ${CMAKE_SOURCE_DIR}/drivers/sd_card
    ${CMAKE_SOURCE_DIR}/drivers/ws2812
)

set(POOM_KERNEL_DIRS
    ${CMAKE_SOURCE_DIR}/kernel/memory
    ${CMAKE_SOURCE_DIR}/kernel/task
)

list(APPEND EXTRA_COMPONENT_DIRS
    ${POOM_MODULE_DIRS}
    ${POOM_THIRD_PARTY_DIRS}
    ${POOM_APPLICATION_DIRS}
    ${POOM_DRIVER_DIRS}
    ${POOM_KERNEL_DIRS}
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(poom)
