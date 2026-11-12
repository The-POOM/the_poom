#!/bin/sh
# Regression: compiler/test failures must make check.sh fail immediately.
set -eu
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/security-audit-harness-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
trap 'exit 1' HUP INT TERM
mkdir "$T/bin" "$T/tmp"
cat > "$T/bin/cc" <<'STUB'
#!/bin/sh
if [ "$FAIL_COMPILE" = 1 ]; then exit 23; fi
while [ "$#" -gt 0 ]; do
  if [ "$1" = -o ]; then
    shift
    printf '#!/bin/sh\necho injected-test-failure\nexit 42\n' > "$1"
    chmod +x "$1"
    exit 0
  fi
  shift
done
exit 1
STUB
chmod +x "$T/bin/cc"
for fail_compile in 0 1; do
  if PATH="$T/bin:$PATH" TMPDIR="$T/tmp" FAIL_COMPILE="$fail_compile" \
      sh "$R/tools/security_audit_hostcheck/check.sh" > "$T/result" 2>&1; then
    echo 'FAIL: harness swallowed an injected failure'
    exit 1
  fi
  if [ "$fail_compile" = 0 ]; then
    case "$(cat "$T/result")" in
      *injected-test-failure*) ;;
      *) echo 'FAIL: test failure output was lost'; exit 1 ;;
    esac
  fi
  case "$(cat "$T/result")" in
    *'== audit rules'*) echo 'FAIL: harness continued after a failure'; exit 1 ;;
  esac
  if [ -n "$(ls -A "$T/tmp")" ]; then
    echo 'FAIL: harness left temporary files behind'
    exit 1
  fi
done
echo 'PASSED: compiler/test failures propagate and temporary files are removed'
