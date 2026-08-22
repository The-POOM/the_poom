set(srcs
    "poom_pcap_manager.c"
    "poom_pcap_capture.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES pcap sd_card poom_wifi_ctrl poom_ble_scan ieee802154 esp_wifi poom_scanner_core
)
