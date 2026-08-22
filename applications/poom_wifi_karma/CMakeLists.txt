set(srcs "poom_wifi_karma.c")
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES poom_wifi_ctrl poom_wifi_captive esp_timer)
