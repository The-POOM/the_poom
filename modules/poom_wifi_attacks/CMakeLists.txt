set(srcs "poom_wifi_attacks.c" "poom_wifi_marauder.c" "poom_marauder_frames.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES esp_wifi esp_timer mbedtls poom_wifi_ctrl poom_wifi_captive)
target_link_libraries(${COMPONENT_LIB}  -Wl,-zmuldefs)
