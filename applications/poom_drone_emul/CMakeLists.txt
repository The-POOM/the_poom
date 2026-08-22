set(srcs "poom_drone_emul.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_wifi esp_timer drone_id poom_wifi_ctrl poom_secrets_store
)
