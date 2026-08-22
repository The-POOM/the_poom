set(srcs "poom_host_sniffer_transport.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt
)
