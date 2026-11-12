# Net Scanner PR validation

Prepared against upstream tag `1.0.16` (`3533ee027efcf06a610265f3c4b4be8c73548fb5`).
The original two scanner commits were cherry-picked onto this base in an isolated
worktree. The original `feature/net-scanner-auditor-clean` branch remains at
`c703cea793c4e7edf3e3add565735014bdf10316`.

## Review changes

- Preserve upstream Device Inspector, Wi-Fi Guard and release version 16.
- Wait for the ICMP end callback after timeout/stop before freeing callback
  storage. The pinned SDK's stop and delete calls do not join the ping task.
- Release Wi-Fi if creating the scanner UI task fails.
- Make host compiler/test/sanitizer failures fail the harness, show the failure
  output and remove isolated temporary files. Add injected-failure regression
  coverage and support invocation outside the repository root.
- Document and supply the opt-in Wardrive-verified release memory profile.
- Correct SD mount comments to describe two file slots (CSV and index).

## Completed checks

- Six existing host suites: audit logic, rules, checks, index, scan logic and
  identification; ASan/UBSan enabled, zero failures.
- GCC 14.2 and Clang 19.1 compile-only checks: C23, warnings as errors.
- Harness regression: injected compiler and executable failures both stop the
  suite, preserve diagnostics and clean temporary files.
- Full ESP32-C5 firmware build and size check with the exact SDK and profile in
  [build instructions](NET_SCANNER_BUILD.md), including the real lwIP backend,
  shared Wi-Fi controller, SD driver and menu integration.
- All 16 explicit release-profile settings verified in generated sdkconfig.
- Upstream defaults, partitions, dependency versions, Device Inspector and
  Wi-Fi Guard sources unchanged. No Wardrive feature code included.
- `git diff --check` clean.

The build has warnings in upstream/third-party code and SDK component dependency
validation; it is not a warning-free build. Host compiler checks for the scanner
are strict. The build's static memory/partition checks do not establish runtime
heap headroom or app-transition behavior.

## Pending

No attached ESP32-C5 was available. The hardware acceptance scenarios in the
build guide remain untested for Net Scanner, including the cleanup fixes.
No scans were run against real network targets. Only the ESP32-C5 release
configuration was built; ESP32-C6 is not validated here. No upstream PR was
published by this preparation task.
