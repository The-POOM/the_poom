# SPDX-License-Identifier: MIT
# POOM security audit engine (headless): subnet math, ICMP/ARP discovery and
# the TCP Connect port scanner. The pure-logic translation unit
# (poom_security_audit_logic.c) is also host-compiled by test/.

idf_component_register(
    SRCS
        "poom_security_audit.c"
        "poom_security_audit_logic.c"
        "poom_security_audit_ident.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_netif
        lwip
        esp_wifi
        poom_wifi_ctrl
)
