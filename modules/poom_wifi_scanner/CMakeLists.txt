set(srcs "poom_wifi_scanner.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi poom_wifi_ctrl)
