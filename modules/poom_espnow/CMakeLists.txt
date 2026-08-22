set(srcs "poom_espnow.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES poom_wifi_ctrl esp_wifi
)
