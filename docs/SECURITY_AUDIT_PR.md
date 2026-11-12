# Add Security Audit for LAN discovery and offline security assessment

Adds **THE BEAST → SECURITY AUDIT**, an on-device LAN discovery and security-assessment workflow. It discovers hosts, identifies services and devices, checks observed configurations and read-only HTTP exposures, and matches detected products against offline Exploit-DB metadata on SD.

### Features

- Wi-Fi connection through shared saved profiles or network selection.
- Bounded host discovery using the assigned IPv4 subnet and netmask.
- TCP Connect scanning with Quick, Extended, Full and Custom port sets; single-host and network-wide workflows.
- Service/banner parsing and device/OS identification with confidence levels.
- Configuration findings and read-only HTTP exposure checks.
- Offline Exploit-DB CSV lookup with an automatically rebuilt block index, version-match tiers and grouped CVE leads.
- Full audit workflow, cancellation with partial results, result browsing and explicit SD text-report saving.
- **Security Audit** naming throughout the menu, reports, serial diagnostics, component/API names, documentation and host tools.

Exploit matches are labelled **potential** and require verification. The app does not execute exploits or attempt passwords. HTTP exposure checks use fixed read-only requests; HTTPS identification is not supported.

### Build and integration

Based on upstream **1.0.16** (`3533ee027efcf06a610265f3c4b4be8c73548fb5`). Includes scanner components, menu integration, a two-file-slot SD mount, and shared Wi-Fi initialization failure cleanup.

Use ESP-IDF **`95b6f32350e1118181d1a1c375972cbcd85a6540`** (`v6.1-dev-1391-g95b6f32350e`). The optional `sdkconfig.security_audit_release` reproduces the release memory settings verified during Wardrive work, including PSRAM malloc and external BSS. Build instructions are in `docs/SECURITY_AUDIT_BUILD.md`; usage is in `docs/SECURITY_AUDIT.md`.

Upstream defaults, partitions, dependency versions, Device Inspector and Wi-Fi Guard remain unchanged. Wardrive and the handshake cracker are separate features and are not included in this diff.

### Validation

- Six ASan/UBSan host suites covering audit logic, rules, HTTP matchers, index behavior, scan logic and identification.
- GCC 14.2 and Clang 19.1 C23 compile checks with warnings as errors; injected compiler/test failures verify harness failure propagation and temporary-file cleanup.
- Standalone ESP32-C5 firmware build and partition-size check using the pinned SDK; all 16 release-profile settings verified in generated configuration.
- Owner hardware checks on ESP32-C5 in a separate combined QA build: discovery, full audit, indexed lookup, SD report saving, exit/menu responsiveness, renamed menu/report headings and app transitions. The standalone branch was built separately but was not separately flashed.
- `git diff --check` passes. Existing SDK/upstream warnings remain; ESP32-C6 and diagnostics-disabled hardware acceptance are not claimed.

The tested combined QA image contained a focused DNS-restoration fix for the separate Wardrive implementation. When combining this feature with Wardrive PR #50, that Wardrive cleanup fix is also needed for the tested transition behavior; it is outside this PR.

Full validation details: `docs/SECURITY_AUDIT_VALIDATION.md`.

