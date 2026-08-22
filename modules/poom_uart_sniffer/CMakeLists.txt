set(srcs "poom_uart_sniffer.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt poom_host_sniffer_transport
)
