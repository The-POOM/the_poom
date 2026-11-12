# feat: on-device Security Audit

On-device LAN security auditing for POOM: discover devices, identify their services, check for exposed configurations and look up potential vulnerabilities using offline Exploit-DB metadata on the SD card.

### Adds

- **Device discovery** — finds LAN hosts using the assigned subnet and netmask, with IP/MAC details and device/OS identification with confidence levels.
- **TCP port scanning** — Quick, Extended, Full and Custom port sets, for one device or the whole discovered network.
- **Service identification** — reads service banners, HTTP headers and device announcements to identify products and versions.
- **Security findings** — flags risky configurations and checks plain-HTTP services for exposed pages and files using fixed read-only requests.
- **Offline exploit lookup** — matches identified products against Exploit-DB titles, ranks version matches and groups duplicate CVE leads. Results are marked **POTENTIAL — verify**, rather than proof that a device is vulnerable.
- **Full NetAudit** — runs discovery, ports/services, identification, web checks and offline lookup as one workflow. B cancels the current step and retains partial results.
- **SD reports** — save a text report containing device identities, services, findings and potential exploit matches.
- Reachable via **THE BEAST → SECURITY AUDIT**, using shared saved Wi-Fi profiles or network selection. It remains separate from stock **SCAN NET**.

### Using and updating the exploit database

The offline lookup uses the same metadata CSV as SearchSploit; users do not need to copy executable exploits onto the device.

1. Download the latest `files_exploits.csv` from the official [Exploit-DB repository](https://gitlab.com/exploit-database/exploitdb).
2. With Security Audit closed, copy it to `/searchsploit/files_exploits.csv` on the SD card. To update the database later, replace this file with the newer copy.
3. Reinsert the card and run an assessment. The app builds its index automatically on the first lookup and rebuilds it when the CSV's size or timestamp changes.

**No firmware update is required.** To force a fresh index, delete `files_exploits.idx` from the same folder while the app is closed; the next lookup recreates it.

Without the CSV, discovery, port scanning, identification and exposure checks still work; offline exploit matching is unavailable.

### Notes

- Based on upstream **1.0.16**. Stock defaults, partitions and dependency versions are unchanged.
- Release build: ESP-IDF `95b6f32350e1118181d1a1c375972cbcd85a6540` (`v6.1-dev-1391-g95b6f32350e`), with the optional `sdkconfig.security_audit_release` PSRAM/external-BSS profile verified during Wardrive work.
- **Authorized use only** — for networks and devices the user owns or has permission to assess. The app does not run exploits or attempt passwords.
- TCP Connect scanning only; HTTPS service identification and HTTP exposure checks over TLS are not supported.
- **Tested:** standalone ESP32-C5 build and partition-size checks; six ASan/UBSan host suites; GCC/Clang compile checks and harness-failure tests. The owner flashed and tested the feature in a combined ESP32-C5 QA build, including audits, indexed lookup, report saving, renamed screens and app transitions. The standalone branch was not separately flashed; ESP32-C6 hardware is unverified.
- Wardrive and the handshake cracker are outside this PR. Combining with Wardrive #50 requires its separate DNS-restoration fix for the tested transition behavior.
- Usage: `docs/SECURITY_AUDIT.md`. Build requirements: `docs/SECURITY_AUDIT_BUILD.md`. Validation: `docs/SECURITY_AUDIT_VALIDATION.md`.
