set(srcs
    "poom_boot_policy.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES
        app_update
        board
        button_driver
        esp_app_format
        esp_driver_gpio
        poom_secrets_store
        sd_card
)
