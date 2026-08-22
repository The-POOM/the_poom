set(srcs "poom_wii.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES poom_imu_stream poom_ble_hid)
