set(srcs "poom_wifi_arp.c" "poom_wifi_net_scan.c")
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES poom_wifi_ctrl esp_netif lwip esp_timer)
