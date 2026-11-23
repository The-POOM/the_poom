idf_component_register(SRCS "poom_wifi_observer.c" "poom_wifi_observer_model.c"
    INCLUDE_DIRS "include" REQUIRES esp_wifi esp_timer poom_wifi_ctrl cjson)
