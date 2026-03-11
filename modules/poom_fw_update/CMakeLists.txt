idf_component_register(SRCS "poom_fw_update.c" 
                            "modules/http_server/http_server.c" 
                    INCLUDE_DIRS "include" "modules/http_server"
                    PRIV_REQUIRES esp_wifi esp_http_server app_update esp_timer nvs_flash mdns_manager poom_wifi_ctrl
                    EMBED_FILES src/webpage/app.css src/webpage/app.js src/webpage/favicon.ico 
                    src/webpage/index.html src/webpage/jquery-3.3.1.min.js
                    )
