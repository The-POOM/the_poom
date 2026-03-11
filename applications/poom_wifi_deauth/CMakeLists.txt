set(srcs "poom_wifi_deauth.c")
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES poom_wifi_attacks poom_wifi_scanner esp_wifi)
