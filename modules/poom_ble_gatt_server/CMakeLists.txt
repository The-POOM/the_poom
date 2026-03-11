set(srcs "poom_ble_gatt_server.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES bt )
