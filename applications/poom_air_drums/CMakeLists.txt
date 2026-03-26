set(srcs "poom_air_drums.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES ble_midi poom_imu_stream poom_sbus
)
