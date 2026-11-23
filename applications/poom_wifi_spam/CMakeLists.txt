set(srcs "poom_wifi_spam.c" "poom_wifi_spam_presets.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES poom_wifi_ctrl esp_wifi poom_secrets_store poom_wifi_attacks)
