set(srcs "poom_drone.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES drone_id esp_wifi poom_ble_scan poom_pcap poom_wifi_ctrl sd_card
)
