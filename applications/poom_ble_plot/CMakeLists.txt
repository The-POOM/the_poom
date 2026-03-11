set(srcs "poom_ble_plot.c"
         "poom_imu_plot.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES poom_imu_stream ws2812 poom_sbus button_driver poom_ble_hid board) 
