set(srcs "src/zb_cli.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES esp-zigbee-console espressif__esp-zigbee-lib
                PRIV_REQUIRES nvs_flash console)
