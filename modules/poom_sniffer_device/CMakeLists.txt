set(srcs "poom_sniffer_device.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES poom_wifi_ctrl poom_secrets_store sd_card esp_wifi esp_netif freertos)
