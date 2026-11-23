# Find My Beep

`BEAST > FIND MY BEEP` discovers tracker candidates and lets the user select one
candidate or `BEEP ALL`, then explicitly press **A: BEEP**. The selection is a
frozen list of at most 24 current, connectable identities. Each identity includes
both Bluetooth address and address type; names and RSSI never select a different
device. **B** cancels the remaining batch. Discovery never sends a sound request.

The native Bluedroid backend sends one request per selected identity. It releases
and reinitializes its owned Bluetooth stack between devices, including failed
connections. It does not retry indefinitely, pair, bond, or escalate security.
An existing unrelated controller is refused. The OLED entry first stops the
existing POOM spam/tracker/detector/scanner/GATT consumers through their own APIs
and resets the generic helper's selection and callbacks. The CLI radio handoff
waits for the Find My UI and worker to stop.

## Protocol provenance

The service UUIDs, commands and short response grammar are adapted from
**Dragon Detector**, commit `99a6f70d58cc448ff435d15a220da136aee89a68`,
`firmware/include/tracker_sound.h` and the direct-request workflow in
`firmware/src/ble_lab.cpp`. The reviewed `tracker_sound.h` SHA-256 is
`490040d76d802bceb32343bb7786a057106ebc840c811679754fcdec861bf39e`.
Dragon's source credits **Just Call Me Koko / ESP32 Marauder** for the
AirTag/FMNA/DULT service and command references and the Find My Sound workflow.
Those references were also checked against Marauder develop at
[`ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da`](https://github.com/justcallmekoko/ESP32Marauder/tree/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da),
`esp32_marauder/WiFiScan.h` and `WiFiScan.cpp`.
Both MIT copyright notices and permission text are retained in [LICENSE](LICENSE).
The POOM transport, OLED integration and one-pass batch behavior are native C
adaptations; Dragon's repeating scheduler/backoff is not used.

| Profile confirmed by service discovery | Service | Characteristic | ATT command |
| --- | --- | --- | --- |
| AirTag sound | `7dfc9000-7d1c-4951-86aa-8d9728f8d66c` | `7dfc9001-7d1c-4951-86aa-8d9728f8d66c` | `AF` |
| Find My accessory / FMNA | `FD44` | `4f860003-943b-49ef-bed4-2f730304427a` | `01 00 03` |
| DULT reference profile | `15190001-12f4-c226-88ed-2ac5579f2a85` | `8e0c0001-1d68-fb92-bf61-48377421680e` | `00 03` |

Service discovery must find the exact characteristic with **write with response**
support. FMNA and DULT also require notifications/indications and a successful
CCCD write before the command. Indications take precedence when both are offered.
AirTag has no additional response subscription in the reference workflow.

## Candidate and result meaning

The bounded AD parser treats advertisement and scan-response data separately.
Apple manufacturer `004C`, subtype `12`, subtype length `19` and at least 29
manufacturer bytes identify a **Find My candidate**, not a proven AirTag model.
Service UUID `FD44`, the full reference DULT UUID, and `FCB2` are also candidates.
Apple proximity/AirPods payloads, an `AirTag` name alone and generic Fast Pair
`FE2C` do not qualify. `FCB2` discovery does not imply that the older DULT sound
service exists; such a device may correctly return **Unsupported**.

Candidates require a connectable advertising report. A scan response alone does
not prove connectability; recent matching ADV metadata is kept separately. No
extended advertising, Bluetooth Classic, Tile-specific sound, or SmartTag-specific
sound protocol is implemented here. Unsupported services and ATT/security
rejections are visible results rather than automatic retries.

Both selection and connection require sightings younger than 30 seconds.
The frozen batch is not expanded by later discoveries. There is no scanning
between requests, so late batch entries can become **Stale** and are skipped.
This is intentional and visible; rescan to choose a fresh batch.

Connections have a 7-second deadline. Each initialization/discovery/ATT stage has
a 5-second deadline; FMNA/DULT replies have a separate 3-second window. Cancellation
is checked while waiting. A timeout is not evidence that a tag is absent or broken.
The C5 Bluedroid configuration uses legacy 31-byte advertising plus 31-byte scan
response reports. Manufacturer firmware and current tag state control which
services are exposed and whether a request is accepted.

**Write ACK** means that the peripheral acknowledged the ATT write. It does not
mean a speaker was heard. A DULT status response can explicitly accept/reject the
request or report completion; other FMNA replies are recorded without inventing
their meaning. A missing additional response remains **Write ACK / No reply within
3s** when ATT acknowledged the write. Submitted commands may still cause sound
after cancellation; stopping the POOM cannot retract a command already sent.

`poom_findmy_beep_stop()` waits for worker exit and returns `ESP_OK` only after the
owned host/controller have been released. A driver teardown error remains visible,
sets `radio_released=false`, and prevents subsequent starts; the OLED asks for a
device restart. Every driver result is checked in the request path. Normal
teardown uses synchronous host/controller lifecycle calls, not a fixed delay.

## Validation

Run the native host suite without any radio hardware:

```powershell
./tests/host/findmy/run.ps1 -Compiler gcc
```

The protocol tests cover bounded AD parsing, split-payload handling, strict Apple
matching, false-positive exclusions, UUID byte order, commands, exact DULT response
lengths, address-type identity, freshness/wraparound and frozen-list deduplication.
The actual backend source runs against deterministic mock GAP/GATT callbacks for
single-pass requests, ACK/rejection/unsupported/timeout handling, cancellation,
stack release between devices, scan-response connectability and busy ownership.
These mocks verify state transitions and API use, not radio timing or stack races.

The component and OLED integration compile with the official ESP-IDF 6.1 C5 SDK.
Host tests and the firmware build do not establish physical tag compatibility,
audible output, sustained Bluetooth heap behavior, or repeated BLE Detect/GATT/UI
handoffs on the device. Those require separate physical acceptance checks.
