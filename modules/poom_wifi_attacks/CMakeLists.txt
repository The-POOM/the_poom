set(srcs "poom_wifi_attacks.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES esp_wifi poom_wifi_ctrl) 
target_link_libraries(${COMPONENT_LIB}  -Wl,-zmuldefs)
