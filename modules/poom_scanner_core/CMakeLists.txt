set(srcs
    "poom_scanner_core.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES freertos poom_wifi_ctrl esp_wifi ieee802154
)
