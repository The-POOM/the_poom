#!/bin/sh
# Host-side checks for the Network Scanner / Security Auditor (no ESP-IDF):
#   1) every pure unit-test suite (ASan/UBSan),
#   2) compile-only of the ESP-side files against minimal stub headers, with
#      gcc and clang in C23 mode and -Werror (the device uses GCC 15,
#      -std=gnu23, -Werror). Catches syntax, format-truncation and C23 issues
#      before the Windows build; it does NOT replace the real build.
# Run from the repo root:  sh tools/net_scanner_hostcheck/check.sh
set -e
R=$(pwd)
H=$R/tools/net_scanner_hostcheck/stubs
T=${TMPDIR:-/tmp}
A=modules/poom_net_audit
N=modules/poom_net_scanner
SAN="-fsanitize=address,undefined"
run() { echo "== $1"; shift; "$@"; }

run "audit logic"  cc -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_logic.c  $A/poom_net_audit_logic.c -o $T/t_al && $T/t_al | tail -1
run "audit rules"  cc -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_rules.c  $A/poom_net_audit_rules.c -o $T/t_ar && $T/t_ar | tail -1
run "audit checks" cc -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_checks.c $A/poom_net_audit_checks.c -o $T/t_ac && $T/t_ac | tail -1
run "audit index"  cc -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_index.c  $A/poom_net_audit_index.c $A/poom_net_audit_logic.c -o $T/t_ax && TMPDIR=$T $T/t_ax | tail -1
run "scan logic"   cc -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$N/include -I$N/test $N/test/test_logic.c  $N/poom_net_scanner_logic.c -o $T/t_sl && $T/t_sl | tail -1
run "scan ident"   cc -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$N/include -I$N/test $N/test/test_ident.c  $N/poom_net_scanner_ident.c $N/poom_net_scanner_logic.c -o $T/t_si && $T/t_si | tail -1

INC="-I$H -Iapplications/poom_app_pack/include -Imodules/poom_arduboy_display/include \
 -Idrivers/button_driver/include -Imodules/poom_sbus/include -Imodules/poom_ui_keyboard/include \
 -Imodules/poom_wifi_ctrl/include -Imodules/poom_wifi_scanner/include -I$A/include -I$N/include \
 -Idrivers/sd_card/include"
for CC in "gcc -std=gnu2x" "clang -std=gnu2x"; do
  for f in $A/poom_net_audit_logic.c $A/poom_net_audit_index.c $A/poom_net_audit_rules.c \
           $A/poom_net_audit_checks.c $N/poom_net_scanner_logic.c $N/poom_net_scanner_ident.c; do
    $CC -O2 -c -o /dev/null -Wall -Wextra -Werror -I$A/include -I$N/include -I$A/test $f
  done
  $CC -O2 -c -o /dev/null -Wall -Wextra -Werror -Wno-unused-parameter $INC $A/poom_net_audit.c
  $CC -O2 -c -o /dev/null -Wall -Wextra -Werror -Wno-unused-parameter $INC \
      applications/poom_app_pack/src/menu_net_scanner.c
  echo "== compile-only OK: $CC"
done
echo "(poom_net_scanner.c, poom_wifi_ctrl.c and sd_card.c need real lwIP/IDF headers: review-only)"
