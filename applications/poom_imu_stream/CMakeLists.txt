set(srcs "poom_imu_stream.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES driver i2c lsm6ds3) 
