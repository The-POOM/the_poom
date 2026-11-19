# Native passive BLE profiles

POOM's existing **BEAST → BLE DETECT** owns these profiles. Choose a profile and press A to begin passive discovery. The selector pages through all ten choices. B from a discovery list stops scanning and returns to the profiles; B again leaves BLE DETECT.

| Marauder reference feature | Native POOM behavior |
| --- | --- |
| Generic Bluetooth sniff | BLE DEVICES retains the existing address/name/RSSI list. |
| FindMy Sniff / FindMy Monitor | FINDMY MONITOR continuously updates matching addresses, RSSI, report counts and last-seen age. It accepts structurally valid Apple FindMy manufacturer records, FD44/FMNA, FCB2, and the complete DULT service UUID. |
| iBeacon Sniff / iBeacon Info | IBEACONS filters complete Apple iBeacon manufacturer records. Detail pages show all 128 UUID bits, big-endian major/minor values, signed measured power, RSSI, count and age. |
| Flipper Sniff | FLIPPER CANDIDATES matches advertised 0x3081, 0x3082 and 0x3083 service UUIDs. It does not search arbitrary payload bytes for these pairs. |
| Flock Sniff | FLOCK CANDIDATES combines manufacturer 0x09C8 with Penguin plus ten digits, FS Ext Battery, a ten-digit name, or no advertised name. A bounded TN serial is extracted from that manufacturer field. These patterns can also describe unrelated hardware. |
| Meta Detect | META CANDIDATES checks the six pinned upstream identifiers and five exclusions across manufacturer fields and 16/32/128-bit Bluetooth-base service UUID/data fields. A blocked identifier anywhere in either AD part suppresses the match. |
| HC-03/05/06 scan | HC NAME CANDIDATES finds those advertised BLE name strings. A name match does not establish a skimmer or any other device purpose. |
| Bluetooth Analyzer | BLE RATE shows received advertising-report counts per measured interval, session count/time and observed RSSI range. It offers the same bounded device list with A. No synthesized graph, RF bandwidth estimate or claimed over-the-air packet count is displayed. |
| Bluetooth Fox Hunt / GATT recon | Existing target handoff and GATT browsing are reused from the device detail/actions screen. They are separate operations selected by the user. This change does not alter the GATT transport. |

UP/DOWN cycles device detail pages. A still opens the existing GATT RECON / RENAME actions, LEFT toggles the address display, and RIGHT hands the exact selected address to Fox Hunt. FindMy monitoring and iBeacon information are integrated into their discovery profiles rather than requiring a preliminary scan in a separate menu.

## Parsing and resource bounds

Advertising and scan-response fields are decoded separately, each at most 31 bytes. A zero terminator or malformed trailing AD field ends only that part. Fields cannot borrow bytes from the other part. UUID service data is decoded at its declared width; custom 128-bit UUIDs are not treated as 16-bit IDs merely because they contain a matching pair. The full DULT UUID is compared in BLE wire byte order. Apple FindMy requires company 0x004C, subtype 0x12, length 0x19 and all 25 subtype bytes. iBeacon requires company 0x004C, subtype 0x02, length 0x15 and its complete payload.

The detector retains the existing fixed twelve-record session list. All reports count toward analyzer statistics even when the list is full. Repeated addresses update their record in place; neither advertisement input nor serial/name extraction allocates memory. UI work is batched by the existing task refresh interval. Duplicate filtering is disabled by the existing BLE scanner configuration. Rates describe controller-delivered reports during the measured interval, not every packet transmitted nearby. Scan duty cycle, coexistence, controller filtering and reception loss still affect these numbers.

Profiles identify advertising patterns. They cannot authenticate a vendor, prove a physical product model, establish ownership or purpose, or follow a device reliably across private-address rotation. Names and advertising identifiers can be changed or copied. The new profile detail pages therefore use candidate wording. HC-03/05/06 hardware that exposes Bluetooth Classic only is not discoverable on the ESP32-C5 BLE radio. Passive scanning does not solicit missing scan responses.

The pinned Marauder Flock OUI-only shortcut is not used by this BLE profile: its broad hardware-vendor prefixes do not establish a Flock advertisement. Manufacturer/name candidates are supported; the separate Wi-Fi Flock detector is outside this change. This component does not add Bluetooth Classic, GATT sound commands, BLE spam, SD packet export or upstream target-selection state.

## Provenance and validation

Feature and identifier reference: JustCallMeKoko's [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder/tree/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da), commit `ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da`. The relevant primary sources are `esp32_marauder/MenuFunctions.cpp` (Bluetooth sniffer menu), `WiFiScan.cpp` (BLE callbacks and Flock/Meta classifiers), `WiFiScan.h` (service identifiers), and `IBeacon.h` (payload fields). The upstream Meta callback credits NullPxl for its identifier filters. POOM uses its existing GPL-3.0-or-later detector and UI; the new bounded C parsing implementation remains under GPL-3.0-or-later. No Arduino/NimBLE stack or upstream menu implementation is introduced.

Run `tests/host/run_ble_recon_tests.ps1 -Compiler <host-gcc>` to execute production parser/filter/runtime code against mocked scan input and time. Tests cover every new profile, manufacturer-vs-name false matches, truncated and malformed AD fields, independent scan responses, complete-name preference, iBeacon byte order, Meta exclusions, Flock serial bounds, passive scanner setup, failed starts, report counters and zero-rate silence. These tests do not run a controller or transmit/receive RF.

Hardware acceptance remains separate: compare known owned beacon advertisements to displayed fields; verify each profile against a matching and nonmatching fixture; watch FindMy counts/age while its source enters and leaves range; compare BLE RATE to delivered controller reports; navigate every selector page; stop/back and reopen profiles; then exercise the existing Fox Hunt and GATT handoffs on owned test devices. No physical discovery, GATT interoperability or detection-efficacy claim follows from host tests or a successful firmware build.
