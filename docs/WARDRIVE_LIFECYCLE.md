# Wardrive lifecycle and stock integration

This integration is based on upstream 1.0.16, commit
`3533ee027efcf06a610265f3c4b4be8c73548fb5`.

Wardrive is started by the menu action only. It has no boot hook, background
service, global allocator policy, partition change or sdkconfig override.
Stock Bluetooth, Wi-Fi manager, scanner and Device Inspector source remain
unchanged. The shared changes are menu/build registration and an additive I2C
stream transaction helper for the GPS receiver.

## Ownership

- The UI, button queue, session, GPS capture and file workers exist only while
  the app is open. Completed radio/file workers are suspended and joined by
  their owner before their state is reused or released.
- Wi-Fi uses a Wardrive-private adapter, handler instances and callback slot.
  It refuses an already-initialised driver or active Bluetooth controller;
  it does not disconnect somebody else's connection to obtain ownership.
- Private Wi-Fi initialisation uses RAM configuration storage. Its scan/transfer
  buffers apply only to that driver instance. Cleanup stops scans, clears the
  driver's AP list, disconnects, stops and deinitialises the owned driver,
  unregisters handlers, destroys only netifs/event loops created by Wardrive,
  and restores the previous default netif. If it borrowed an existing inactive
  default station netif, it restores its IP, DNS and DHCP configuration too.
- The network chooser owns and frees its own bounded scan cache; it does not
  clear the stock Wi-Fi scanner's cached results.
- BLE uses a private passive GAP scanner. It does not use the stock scanner or
  GATT client's settings, connection targets, callback slots or persistent
  state. Its interval/window are `0x0012` (11.25 ms), the pinned IDF's actual
  fallback timing. Stock requests invalid `0x0003` values, ignores their
  asynchronous rejection and starts with IDF's fallback. Scan start is
  successful only after the asynchronous completion event succeeds.
- BLE cleanup disables scan delivery, stops scanning, disables/deinitialises
  the owned Bluedroid host and controller, and frees the completion semaphore.
  No controller memory-release API is called: those releases are irreversible.
- Start failure, stop, pause, cancellation and exit follow the same owned
  resource cleanup paths. On teardown failure ownership is retained for retry;
  Wardrive does not return the menu while its radio resources remain owned.
- GPS stream transfers use temporary device handles if the address was not
  already registered. They do not leave a GPS slot in the shared I2C table.
- The app closes its files and unmounts the SD card only if it mounted it.
  Credentials, edit buffers and transfer tokens are cleared when released.

Opening and immediately closing Wardrive does not start or deinitialise Wi-Fi
or Bluetooth. Existing active radios are left alone; a Wardrive radio operation
fails with a busy/invalid-state result instead of taking them over. GPS polling
and SD operations can still occur while browsing their related app screens.

## Clock and persistent user data

Validated GPS UTC anchors an app-owned monotonic clock for the display and
session IDs. The anchor survives temporary GPS loss for at most 24 hours and
is discarded when Wardrive closes. A valid existing system clock is a fallback.
Timezone selection only formats UTC; it does not establish the current time.

IDF's supported TLS verifier reads the platform wall clock. For HTTPS upload,
verification or bulk jobs only, if that clock is unset, Wardrive can temporarily
lend it validated GPS UTC. The job snapshots the original wall clock and
monotonic time, then returns the original clock **plus elapsed time** on every
success/failure/cancellation path, before the job completes. Failed restoration
retains the loan for retry. A valid system clock is never replaced.

A loan is refused while an existing SNTP service is active. Wardrive does not
start or reconfigure SNTP, install a global mbedTLS time hook, or alter TLS
verification. If another owner starts SNTP or visibly changes the clock during
a loan, its update is preserved instead of overwritten. The platform clock is
shared during that bounded HTTPS operation, so background consumers can see
the temporary UTC; exact isolation from concurrent unknown clock writers is
not possible with this IDF API. Offline recording, CSV generation and local
HTTP download never borrow the platform clock. Without valid system or GPS
time HTTPS fails closed with `WAIT GPS DATE/TIME`.

Session manifests receive start/end UTC from the same private clock as the UI,
not `time(NULL)`. If GPS time first arrives during a recording, the start is
backfilled using monotonic elapsed time. Raw observation GPS timestamps remain
unchanged. Existing dated folder IDs can supply a missing display/sort timestamp
read-only; sessions without any trustworthy date remain marked `NO UTC`.
No existing journal, manifest, session ID or upload status is rewritten by this
browsing fallback. Optional verbose start/save diagnostics report paths and observation counts
without logging discovered network names, addresses or GPS coordinates.

User-requested recordings, exports, settings and saved credentials/profiles
remain persistent. Restoring transient runtime ownership does not undo those
explicit user operations.

## Saved Sessions browser

Entering Saved Sessions creates an app-owned sorted metadata snapshot, using
at most 256 KiB through the configured allocator (PSRAM under the verified
release profile). Arrow scrolling and detail returns reuse the snapshot;
only an arriving row's upload marker is read. Re-entering from the main menu
refreshes the snapshot, and backing out or closing Wardrive releases it.
Allocation failure or a collection beyond the cache budget falls back to the
existing constant-memory pager without hiding recordings. External SD edits
while browsing become visible on the next browser entry. Raw recordings,
manifest dates, exports and upload queue contents are not changed by browsing.

## Limits and hardware acceptance

Adding code has a permanent flash cost and some static bookkeeping. Exact heap
addresses, fragmentation and first-use SDK infrastructure caches cannot be
restored byte-for-byte. FreeRTOS reclaims the exiting UI task through its idle
cleanup. Compare settled free internal/DMA heap, not just total heap including
PSRAM, and allow the idle task to run before measuring.

The stock-source BLE failure was reproduced without Wardrive and resolved for
the tested sequence using the verified release-compatible external-BSS/PSRAM
profile. See [build evidence](WARDRIVE_BUILD.md) and
[hardware acceptance](WARDRIVE_VALIDATION.md). The app does not change the
global allocation policy at runtime or patch Device Inspector.

For hardware acceptance use identical IDF/configuration for pure stock and this
integration. First establish the stock baseline, then test:

1. Open/close Wardrive without starting a session, then stock BLE/Wi-Fi apps.
2. Record Wi-Fi only, BLE only and combined sessions; pause/resume, stop and
   exit each, then reopen stock BLE/Wi-Fi apps.
3. Cancel network selection/connection and local file-transfer operations;
   exit while a cancellable job is completing and check subsequent stock apps.
4. Repeat entry/exit at least five times and check settled internal/DMA heap
   for growth, callback activity after exit, crashes or stale scan settings.
5. Run Device Inspector in the same sequence as the stock baseline. A baseline
   failure is not a passing compatibility result.

Real WiGLE uploads require owner approval. Host tests use synthetic offline
HTTP/TLS mocks only.

## Validation

`ASAN_OPTIONS=detect_leaks=0 CJSON_DIR=/path/to/cJSON sh tools/wardrive_hostcheck/check.sh`
includes the production UI and private Wi-Fi/BLE adapters. Lifecycle tests
exercise pre-existing owners, partial initialisation failures, failed teardown
and retry, asynchronous scan failure/timeout, late callbacks and restart.
`test_isolation_contract.py` guards stock boot/config/partition/radio files.

ESP32-C5 compilation is checked against IDF commit
`95b6f32350e1118181d1a1c375972cbcd85a6540`. The owner reported successful Wi-Fi/BLE/GPS collection, pause/resume, repeated
entry/exit, CSV export and local Wi-Fi download, with stock BLE Detect and Fox
Hunt still working afterwards. The owner also reported a working clock, dated session saves and successful WiGLE submission. No real WiGLE upload was performed by the agent.
