set(srcs "poom_motion_midi.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES ble_midi poom_imu_stream
)
