#!/bin/sh
# Host-side checks for the Network Scanner / Security Auditor (no ESP-IDF):
#   1) every pure unit-test suite (ASan/UBSan),
#   2) compile-only of the ESP-side files against minimal stub headers, with
#      gcc and clang in C23 mode and -Werror (the device uses GCC 15,
#      -std=gnu23, -Werror). Catches syntax, format-truncation and C23 issues
#      before the Windows build; it does NOT replace the real build.
# Run from the repo root:  sh tools/net_scanner_hostcheck/check.sh
set -e
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$R"
H=$R/tools/net_scanner_hostcheck/stubs
T=$(mktemp -d "${TMPDIR:-/tmp}/net-scanner-hostcheck.XXXXXX")
trap 'rm -rf "$T"' EXIT
trap 'exit 1' HUP INT TERM
A=modules/poom_net_audit
N=modules/poom_net_scanner
SAN="-fsanitize=address,undefined"
# Keep test execution outside pipelines: POSIX sh otherwise reports tail's
# success even when a test or sanitizer fails.
run_test() {
  echo "== $1"
  name=$2
  shift 2
  cc "$@" -o "$T/$name"
  if TMPDIR="$T" "$T/$name" > "$T/$name.log" 2>&1; then
    tail -1 "$T/$name.log"
  else
    cat "$T/$name.log"
    return 1
  fi
}

run_test "audit logic" t_al -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_logic.c  $A/poom_net_audit_logic.c
run_test "audit rules" t_ar -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_rules.c  $A/poom_net_audit_rules.c
run_test "audit checks" t_ac -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_checks.c $A/poom_net_audit_checks.c
run_test "audit index" t_ax -std=gnu11 -O2 -Wall -Wextra -Werror $SAN -I$A/include -I$A/test $A/test/test_index.c  $A/poom_net_audit_index.c $A/poom_net_audit_logic.c
run_test "scan logic" t_sl -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$N/include -I$N/test $N/test/test_logic.c  $N/poom_net_scanner_logic.c
run_test "scan ident" t_si -std=c11   -O2 -Wall -Wextra -Werror $SAN -I$N/include -I$N/test $N/test/test_ident.c  $N/poom_net_scanner_ident.c $N/poom_net_scanner_logic.c

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
