set(srcs "poom_ieee802154_sniffer.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES poom_scanner_core poom_uart_sniffer
)
