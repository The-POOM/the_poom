set(srcs "poom_motion_midi.c" "motion_engine.c")

idf_component_register(
    SRCS "motion_engine.c" "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES ble_midi poom_imu_stream poom_sbus
)
