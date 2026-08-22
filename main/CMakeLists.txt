set(COMPONENT_REQUIRES
    button_driver
    i2c
    nvs_flash
    poom_menu
    poom_nfc
    poom_sbus
    poom_cli
)
set(COMPONENT_PRIV_REQUIRES )
set(COMPONENT_SRCS "main.cpp")
set(COMPONENT_ADD_INCLUDEDIRS "")

if(CONFIG_ZB_ENABLED)
    list(APPEND COMPONENT_REQUIRES zb_cli)
endif()

register_component()
