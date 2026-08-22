set(srcs "poom_wifi_mac.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi poom_wifi_ctrl
                       PRIV_REQUIRES freertos)
