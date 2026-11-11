# Wardrive validation and review scope

## Hardware acceptance reported by the owner, 11 November 2026

Using the pinned release SDK and the memory profile in
[build instructions](WARDRIVE_BUILD.md), the owner reported:

- Pure stock Device Inspector Wi-Fi/BLE scanning, Spectrum, BLE Detect,
  Fox Hunt BLE and repeated Device Inspector entry all passed.
- Wardrive Wi-Fi/BLE collection, GPS reception/clock, pause/resume,
  session save, CSV creation and local Wi-Fi download passed.
- Wardrive → stock apps → Wardrive passed without reset between apps;
  repeat BLE start/stop and dated session saves completed with zero errors.
- The owner independently reported successful WiGLE submission. Agent checks
  used offline mocks only.

Example dated sessions saved 82 observations (10 Wi-Fi / 25 BLE distinct keys)
and subsequent runs saved 28 and 26 observations with zero write errors.
Two successive stopped-session total-heap readings were identical. This is
limited repeat-use evidence, not a leak/endurance proof. An undated start before
GPS time is expected; later GPS can backfill metadata without renaming folders.

The owner also passed the final cleaned-build smoke test on 12 November 2026
(Australia/Brisbane), using source commit
`3efedca916639059d7ba21b115bbefc4005ce4e6` and Windows build directory
`build-pr-final-20261112-081928`. Tests passed before GPS lock and again
after taking the device to a window, acquiring a lock and running Wardrive.
The stock-app checks after Wardrive also passed. USB was deliberately
disconnected to move the device; the resulting monitor ClearCommError does
not by itself indicate a firmware crash. These are owner-reported hardware
results; the agent did not directly operate the POOM.

## Resource cost

Before cleanup, the matched stock image was 0x368240 bytes and the integration
image was 0x380d30 in the same Linux environment: approximately 99 KiB added.
The existing 0x3a0000 app slot retained 3% free. The owner's Windows image was
0x381690 and also retained 3%; toolchain paths/build metadata can affect size.
Permanent internal BSS grew by 536 bytes (51568 → 52104). External BSS remained
33816 bytes. Larger Wardrive state, queues and workers are allocated on entry
and released on exit. Exact heap layout and SDK caches are not guaranteed to
return byte-for-byte to an earlier snapshot.

External BSS recovered 33808 bytes internally in the stock comparison. The
previous runtime allocation failures were reproduced without Wardrive and
resolved for the tested sequence by the verified memory profile. No Device
Inspector or shared radio-helper fix was needed.

## Automated checks

The host suite exercises GPS framing/freshness, storage/export/recovery,
metadata dates and sorting, UI navigation/holds, private radio ownership,
asynchronous scan failures, cleanup retries and scoped HTTPS clock restoration.
CSV and TAR output receive independent Python validation. Upload/transfer
checks use synthetic offline servers/mocks and include ambiguous submissions,
queue persistence, cancellation and TLS failure handling. ASan/UBSan are enabled.
An isolation guard checks unchanged upstream boot/config/partitions/radio files.
The ESP32-C5 build uses the exact release SDK and unchanged partition layout.

## Shared-file scope and remaining limits

Shared code changes are build/menu registration and an additive I²C stream
transaction helper used by the GPS adapter. Stock boot, allocator defaults,
Device Inspector and shared BLE/Wi-Fi helpers remain unchanged. The optional
release memory profile is documented separately from app lifecycle behavior.

Saved Sessions scrolling across off-screen entries can be sluggish; the owner
explicitly deferred performance work. Field endurance, power-loss resilience
across SD brands and every unsupported GPS receiver are not claimed. Preserve
raw data when inspecting faults. See [lifecycle](WARDRIVE_LIFECYCLE.md) for
ownership refusal and the shared-clock concurrency limit during HTTPS.

## Packaged revision checks

After removing the temporary allocation-failure hook and disabling verbose
traces by default, the full ASan/UBSan host suite passed, including 1191
production UI frames. The exact-SDK firmware build passed: 0x37f9f0 bytes
in the 0x3a0000 app slot, leaving 0x20610 (132624) bytes, or 3%. Internal
BSS is 52080 bytes and external BSS 33816. Documentation links and protected
stock-file checks passed. The owner subsequently passed the final hardware smoke test of the cleaned
revision as recorded above. The Windows final image was 0x380350 bytes,
leaving 0x1fcb0 (130224) bytes, or 3%, in the same app slot. Internal BSS
52080 and external BSS 33816 matched the Linux build.
