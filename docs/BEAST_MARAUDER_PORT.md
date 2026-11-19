# Native Beast Wi-Fi actions and Marauder feature mapping

The Beast menu adds Bad Msg, Association Sleep, Quiet Time, Channel Switch and
SAE Commit actions using POOM's ESP-IDF radio services and OLED controls. Each
action starts its own discovery flow: choose the action, choose a network,
choose clients when needed, review the run settings, and press **A:START**.
There is no prerequisite scan elsewhere in the menu.

The implementation follows POOM's memory, input and application lifecycle
conventions. It reuses the existing AP scanner, passive client detector and
capture manager instead of importing Marauder's Arduino application and menu
runtime. This is a scoped adaptation, not a claim that all Marauder features or
its exact screen flow are present.

## Actions and navigation

| Beast entry | Target selection | Current behavior |
| --- | --- | --- |
| `BAD MSG` | One AP, then all observed clients or a chosen client set | Sends the adapted Bad Msg frame format in a round-robin across the copied client set. |
| `ASSOC SLEEP` | One AP, then all observed clients or a chosen client set | Sends the adapted association/power-management frame format across the copied client set. |
| `QUIET TIME` | One AP | Sends a beacon-format Quiet element for the selected BSSID. |
| `CHANNEL SWITCH` | One AP and a different channel in the same band | Sends a beacon-format Channel Switch Announcement. The run remains on the selected AP's current channel. |
| `SAE COMMIT FLOOD` | One WPA3-Personal or WPA2/WPA3-Personal transition AP | Sends bounded initial group-19 SAE Commit frames. It does not complete SAE authentication or implement anti-clogging-token retries. |

APs with equal SSIDs remain separate targets when their BSSIDs differ. The list
shows channel, RSSI and a BSSID suffix. Hidden SSIDs remain selectable. The
selected AP is copied into the session, so a later scan-cache update cannot
silently change an already chosen target.

Bad Msg and Association Sleep each have one Beast entry. After choosing an AP,
choose **ALL CLIENTS** or **CHOOSE CLIENTS** within that action. This is a choice
of stations on one network, not a multi-network operation.

Client discovery listens on the selected AP's channel and derives station MACs
from observed traffic involving that BSSID. It does not prove that every
associated client is visible. CHOOSE CLIENTS uses UP/DOWN to browse and A to
toggle each MAC; RIGHT continues with the marked set, up to 16 clients. Client
selection remains attached to MAC addresses across refreshed snapshots.
ALL CLIENTS advances to the ready screen after three seconds once at least one
client is present; **A:USE CLIENTS** can accept the current set earlier. With no
observed clients it keeps listening. Discovery stops before the ready screen,
and a separate **A:START** is still required to begin transmission.

On the ready screen, UP/DOWN chooses 15, 30 or 60 seconds; the initial selection
is 30 seconds. Channel Switch also uses LEFT/RIGHT to choose its destination
channel. **B** returns to the preceding selection stage, and **B:STOP** stops an
active run. A finished run shows its counters and allows a return to settings
before another start.

The native runtime sends at most one frame per configured interval; the menu
uses 100 ms. Processing can make the actual rate lower. It stops at the selected
duration, on a stop request, or on a construction/driver error. The backend
rejects durations above 60 seconds and intervals below 100 ms. Unsupported
driver behavior is reported instead of being retried indefinitely.

## Capture and scan coverage

The comparison below uses Marauder `develop` commit
[`ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da`](https://github.com/justcallmekoko/ESP32Marauder/tree/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da).
An existing POOM function with related behavior does not imply exact feature or
output equivalence.

| Marauder area | POOM implementation | Scope or remaining gap |
| --- | --- | --- |
| AP discovery and target AP selection | Wi-Fi scanner and Wi-Fi detector; new action-owned scan flow | Uses ESP-IDF scan results and explicit BSSID selection. It does not reproduce every Marauder AP-detail field. |
| Station/AP-client discovery | `WIFI DETECT` AP/client monitor; reused by the new client actions | Clients are tied to one selected AP. A standalone all-stations survey/wardrive workflow is not included. |
| Probe requests | `PROBE REQ` and SNIFFER `PROBE` | Existing observation UI and capture paths. |
| Beacon, deauthentication and raw Wi-Fi capture | SNIFFER `BEACON`, `DEAUTH`, `RAW` | The DEAUTH capture filter selects deauthentication subtype 12; the separate deauth detector also handles disassociation. |
| EAPOL and handshakes | SNIFFER `EAPOL`/`HANDSHAKE`, target capture and POOMGOTCHI | Existing EAPOL mode also admits protected frames. Handshake parsing and output have their own validation requirements. |
| PMKID | Existing handshake parser and `.22000` sidecar export | The presence of a PMKID/export is distinct from recovering a password. |
| WPS | Existing WFA EAP-Expanded capture | This is not a WPS-capable-AP discovery display or a WPS enrollment workflow. |
| SAE capture | New SNIFFER `SAE` mode | Passive capture of SAE Authentication commit, confirm and status/token responses. No group/token detail UI or token-retry state machine is added. |
| Pwnagotchi discovery | No dedicated advertisement decoder | POOMGOTCHI is POOM's handshake-capture app; its name does not mean it detects Pwnagotchi advertisements. |
| Espressif discovery | Existing OUI/vendor labels | No dedicated Espressif-only scan/filter is added. |
| Channel activity, packet rates and signal views | `SCAN CHANNELS`, `SPECTRUM`, frame-mix statistics and Fox Hunt | Related native views, not identical Marauder data presentation. |
| BLE discovery/capture | Existing BLE scan, tracker, detector and PCAP services | Includes tracker/wearable classifications such as AirTag, SmartTag, Tile and smart-glasses heuristics. It does not establish exact parity with Marauder's skimmer, Flipper, iBeacon or BLE Flock workflows. |
| Remote ID | Existing drone scan features | Existing POOM implementation; this change does not revalidate every transport. |
| PineScan, multi-SSID detection, CSI motion and follow detection | No dedicated counterparts identified in this port | Not implemented by this change. |
| ARP, ping and port/service scans | Existing `SCAN NET` ARP discovery, TCP/UDP port probes, SSH checks and common-service host probes | ICMP ping and a complete 1..65535 TCP port sweep remain separate gaps; existing native probes should be reused. |
| GPS tracking and wardriving | Existing `WARDRIVING NODE` forwards records to an external core | The external core owns GPS/logging. Standalone GPS tracking and station/BLE wardriving orchestration are not added. |

Existing deauthentication, Wi-Fi Clone/Combine, Karma, Wi-Fi/BLE spam and captive
portal features retain their own menus and behavior. They have not all been
rewritten into the new action flow.

## Implementation and integration

- [Native action UI](../applications/poom_app_pack/src/menu_wifi_action.c):
  session allocation in PSRAM with internal-memory fallback, copied targets,
  input queue, scan worker, passive client snapshots and status rendering.
- [Runtime](../modules/poom_wifi_attacks/poom_wifi_marauder.c): configuration
  validation, bounded worker lifecycle, checked radio calls, driver counters
  and cleanup before reporting completion.
- [Frame builders](../modules/poom_wifi_attacks/poom_marauder_frames.c): bounded
  output buffers and independent construction of each frame.
- [SAE capture filter](../modules/poom_pcap/poom_pcap_sae_filter.h): a pure
  bounds-checked classifier for Authentication frames with algorithm 3. The
  ESP-IDF capture path removes the FCS from the length used for parsing.
- [AP scanner](../modules/poom_wifi_scanner/poom_wifi_scanner.c): failed scans
  invalidate the prior target count; new results become selectable only after
  successful retrieval.
- [CLI integration](../applications/poom_cli/src/cli_apps.c): `wifi-status`
  includes the action runtime, and `stop-all`/Wi-Fi conflict handling stop it.
  Optional acknowledged hooks also stop an OLED action's AP scan or client
  discovery before CLI takeover, without a cyclic component dependency.
  This change adds no CLI transmission command for the new formats.

ESP32-C5 supports the native dual-band paths; other targets retain their own
channel limits. A channel present in a menu is not proof that the driver can
operate on it under the active radio configuration. Errors remain visible to
the caller. POOM's existing raw-frame sanity-check override is a build/runtime
mechanism, not evidence that a particular frame type transmits successfully on
every SDK or board.

The radio and promiscuous callback are shared resources. A single radio session
must own discovery, monitoring or transmission at a time. CLI and OLED behavior
must be checked together when validating transitions. This feature does not
introduce a system-wide radio scheduler for every legacy application.

## Provenance

The adapted Bad Msg, Association Sleep, Quiet, Channel Switch and initial SAE
frame formats come from **ESP32 Marauder by Just Call Me Koko**. The reference
files are
[`WiFiScan.h`](https://github.com/justcallmekoko/ESP32Marauder/blob/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da/esp32_marauder/WiFiScan.h)
and
[`WiFiScan.cpp`](https://github.com/justcallmekoko/ESP32Marauder/blob/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da/esp32_marauder/WiFiScan.cpp).
The upstream MIT notice and source revision are preserved in
[MARAUDER_LICENSE.txt](../modules/poom_wifi_attacks/MARAUDER_LICENSE.txt).

The action-first OLED flow, ESP-IDF integration, resource handling, immutable
frame builders and PSA-based ECC integration are POOM adaptations. Existing
POOM components retain their own license notices and attribution. This document
does not imply upstream endorsement or that the resulting firmware is Marauder.

## Verification and lab acceptance

Host checks exercise frame construction, parsing and cache behavior without
using a radio. The passive checks are documented in
[README_PASSIVE_WIFI.md](../tests/host/README_PASSIVE_WIFI.md). A successful host
check or firmware build does not verify RF reception or an effect on a peer.
The UI's **Sent** field counts frames accepted by the driver, not peer receipts,
disconnections, channel changes or authentication outcomes.

The production action UI can also be exercised with a native GCC compiler:

```powershell
./tests/host/run_wifi_action_tests.ps1 -Compiler gcc
```

That harness includes the production `menu_wifi_action.c` and models the task,
queue, scanner, client-monitor and backend interfaces. It covers navigation,
copied target identity, WPA3 filtering, selected/all-client flows, automatic
client-set acceptance, scan/error recovery, early cancellation and acknowledged
CLI stop. Its deterministic scheduler does not prove FreeRTOS timing or RF
behavior.

The following is a proposed acceptance sequence for owned lab equipment. It is
not a record of completed hardware testing.

1. Record the firmware revision/hash, board, SDK/configuration, AP model,
   security mode and channel. Keep a known working image available before
   installing the candidate.
2. Open each new action directly from Beast without a preceding scan. Verify
   hidden SSIDs and same-name APs remain distinguishable by BSSID/channel.
   Exercise scan cancellation, empty scans, rescan, back navigation and reopen.
3. Use one owned AP and controlled clients. Confirm client discovery only lists
   stations observed with that BSSID; check the chosen station MAC in the
   chosen-client scope and the copied count in the all-client scope.
4. Verify the ready screen reflects the intended AP, client set, duration and
   destination channel. Observe that discovery alone does not start an action.
5. Begin with the shortest run and a separate receiver on the AP's channel.
   Compare captured frame type, BSSID, destination, sequence and information
   elements with the intended action. Record driver acceptance and peer behavior
   as separate results, including cases where the peer ignores the frames.
6. During a run, press **B:STOP** and verify transmission stops and the radio is
   released. Separately verify timed completion, reported driver errors,
   subsequent scanning and repeated entry/exit without stale counters or heap
   growth.
7. Repeat appropriate cases on owned 2.4 GHz and 5 GHz networks supported by the
   board and active driver configuration. Verify SAE selection excludes
   WPA3-Enterprise and non-SAE networks. Confirm that lack of token retries or a
   complete SAE exchange is represented as a limitation, not as successful
   authentication.
8. Capture a controlled WPA3 exchange using SNIFFER `SAE`. Inspect the saved PCAP
   independently for SAE Authentication frames, and check the existing capture
   modes still select and save their expected traffic.
9. Check `wifi-status`, `stop-all`, and transitions between the OLED action and
   CLI radio services. Verify another service cannot silently replace a scan or
   client monitor and that all stop paths return to a usable menu.

Keep build results, capture evidence and observed peer behavior separate in the
acceptance record. No hardware efficacy claim is made by this document.
