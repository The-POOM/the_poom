set(component_requires)
set(component_priv_requires
    esp_event
    esp_netif
    nvs_flash
)

list(APPEND component_requires esp_wifi)

if(CONFIG_POOM_WIFI_CTRL_ENABLE_MDNS)
    list(APPEND component_requires mdns_manager)
endif()

idf_component_register(
    SRCS
        "poom_wifi_ctrl.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${component_requires}
    PRIV_REQUIRES
        ${component_priv_requires}
)

if(CONFIG_POOM_WIFI_CTRL_ENABLE_MDNS)
    message(STATUS "poom_wifi_ctrl: mDNS support enabled")
endif()
