# SPDX-License-Identifier: MIT
# POOM network scanner engine (headless): subnet math, ICMP/ARP discovery and
# the TCP Connect port scanner. The pure-logic translation unit
# (poom_net_scanner_logic.c) is also host-compiled by test/.

idf_component_register(
    SRCS
        "poom_net_scanner.c"
        "poom_net_scanner_logic.c"
        "poom_net_scanner_ident.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_netif
        lwip
        esp_wifi
        poom_wifi_ctrl
)
