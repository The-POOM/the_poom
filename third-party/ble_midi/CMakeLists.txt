set(srcs "ble_midi.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES bt nvs_flash poom_sbus) 
