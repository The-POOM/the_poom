set(srcs
    "src/poom_menu.c"
    "src/poom_menu_icons.cpp"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES
        button_driver
        buzzer
        esp_driver_gpio
        esp_hw_support
        nvs_flash
        poom_app_pack
        poom_breakout
        poom_arduboy_display
        poom_button_sound
        poom_ble_keyboard
        poom_led_rainbow
        poom_sbus
        sd_card
)
