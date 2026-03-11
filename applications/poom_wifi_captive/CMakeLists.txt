set(srcs "poom_wifi_captive.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES dns_server poom_wifi_scanner esp_http_server board esp_http_client poom_sbus sd_card ws2812 poom_wifi_ctrl
                EMBED_FILES redirect.html root.html)
