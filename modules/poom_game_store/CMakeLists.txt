idf_component_register(
    SRCS "poom_game_store.c"
    INCLUDE_DIRS "include"
    REQUIRES
        cjson
        esp_http_client
        esp_netif
        esp_rom
        fatfs
        mbedtls
        sd_card
)
