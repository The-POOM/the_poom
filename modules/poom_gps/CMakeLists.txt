idf_component_register(SRCS "poom_gps_capture.c" "poom_nmea.c" "poom_gps_service.c" "poom_gps_xa1110.c"
    INCLUDE_DIRS "include" REQUIRES i2c)
