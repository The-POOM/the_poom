set(srcs "src/dns_server.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES esp_netif)                        