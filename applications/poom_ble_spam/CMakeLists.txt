set(component_srcs "poom_ble_spam.c" "poom_ble_payload.c")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES bt esp_timer)
