# SPDX-License-Identifier: MIT
# POOM offline SearchSploit (Exploit-DB) lookup over the SD card. Pure CSV/match
# logic (poom_net_audit_logic.c) is also host-compiled by test/.

idf_component_register(
    SRCS
        "poom_net_audit.c"
        "poom_net_audit_logic.c"
        "poom_net_audit_index.c"
        "poom_net_audit_rules.c"
        "poom_net_audit_checks.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sd_card
)
