# Security Audit PR validation

Prepared against upstream tag `1.0.16` (`3533ee027efcf06a610265f3c4b4be8c73548fb5`).
The original two scanner commits were cherry-picked onto this base in an isolated
worktree. The original source branch is preserved at
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

## Checks completed before the maintainer revision

- Six existing host suites: audit logic, rules, checks, index, scan logic and
  identification; ASan/UBSan enabled, zero failures.
- GCC 14.2 and Clang 19.1 compile-only checks: C23, warnings as errors.
- Harness regression: injected compiler and executable failures both stop the
  suite, preserve diagnostics and clean temporary files.
- Full ESP32-C5 firmware build and size check with the pinned SDK and the former
  standalone audit profile, including the real lwIP backend, shared Wi-Fi
  controller, SD driver and menu integration.
- All 16 explicit release-profile settings verified in generated sdkconfig.
- Upstream defaults, partitions, dependency versions, Device Inspector and
  Wi-Fi Guard sources unchanged. No Wardrive feature code included.
- `git diff --check` clean.

The build has warnings in upstream/third-party code and SDK component dependency
validation; it is not a warning-free build. Host compiler checks for the scanner
are strict. The build's static memory/partition checks do not establish runtime
heap headroom or app-transition behavior.

## Maintainer revision

- Reuse the upstream RAM optimization profile and remove the app-specific
  profile. Record the fresh RX16 / BA16 versus release RX10 / BA6 difference
  in the build notes.
- Preserve the optional host infrastructure at source commit `320f0e8`.
  Remove the test suites and stubs from the firmware PR diff.
- Restore the stock Wi-Fi controller. Verify on hardware that a failed audit
  Wi-Fi startup and subsequent exit leave the next Wi-Fi app usable.
- Keep shared changes to build/menu registration and the SD driver. The
  two-file mount reduces contiguous allocation pressure for the audit's CSV
  and index. Raw mount status lets the audit distinguish memory pressure
  from a missing or unreadable volume. The SD handle is cleared after FAT
  unmount succeeds so a subsequent SPI cleanup failure cannot leave a stale
  mounted state. Existing mount and format entry points keep four file
  slots.

### Standalone ESP32-C5 smoke test

On 19 November 2026 the pinned POOM release SDK built and linked revision
`9d39dbffb4d70e3293a68c1c3b2f3d0b6b0b4330` as Windows build
`build-pr51-review-20261119-203521`. The image was `0x37b9e0` bytes, leaving
4% of the smallest app partition free. The generated config checks passed
for RX10, BA6 and external mbedTLS allocation.

The owner reported that the standalone smoke checks passed, including SD
report saving, returning to existing apps and reopening Security Audit
without rebooting. The serial excerpt confirms a successful SD mount,
indexed lookup and normal audit exit. Wi-Fi startup fault injection was
outside the reported smoke test.

## Owner hardware checks

On 12 November 2026 the owner verified discovery, full audit, indexed offline
lookup, SD report saving, cancellation/exit and app transitions on ESP32-C5
using the pinned release SDK and memory profile in a separate combined QA
build. After the rename, the owner also confirmed the SECURITY AUDIT menu and
screen titles, successful audit/report saving, renamed report heading and a
responsive main menu on exit.

These are owner-reported hardware results; the cloud environment has no device.
The hardware build also contained the separate handshake and Wardrive features,
and enabled Wardrive diagnostics. The standalone smoke test above was performed separately. ESP32-C6 validation
remains pending.

## Integration note

The combined QA exercise exposed a DNS-restoration issue in the separate
Wardrive implementation after borrowing a station interface. Its fix and the
Wardrive navigation improvements remain on the QA branch, outside this PR.
The tested combined Wi-Fi/BLE transition used that fix. When combining with
Wardrive PR #50, apply its focused DNS cleanup fix as well; Security Audit
itself does not depend on the Wardrive feature.
