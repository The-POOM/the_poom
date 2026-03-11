set(srcs "src/mdns_manager.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES esp_wifi nvs_flash mdns) 