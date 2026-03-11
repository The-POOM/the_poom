set(srcs "poom_edge_impulse.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES poom_wifi_ctrl poom_imu_stream poom_secrets_store esp_http_client cjson esp_timer
)
