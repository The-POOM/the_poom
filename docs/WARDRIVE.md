# Wardrive

Wardrive records passive Wi-Fi and BLE observations with optional GPS positions
on the SD card. It supports WiGLE CSV export, local wireless download and
user-initiated WiGLE uploads. Recording works offline.

## Recording

Open **THE BEAST → WARDRIVE**. The main menu contains Start Wardrive,
Check GPS Status, Saved Sessions, Bulk WiGLE Upload, WiGLE API Settings,
WiFi Upload Settings and Additional Settings. UP/DOWN selects, A opens and
B returns. Additional Settings selects Wi-Fi, BLE or combined collection,
scan timing, minimum RSSI, repeat interval and local time zone.

Start Wardrive creates a new session. Hold A on the live screen to pause or
resume; LEFT/RIGHT cycles the live, GPS and statistics pages. GPS UP/DOWN
cycles its detail pages. B returns to the Wardrive menu while recording
continues. Stop Session, or hold B on a live page, to stop. B from the main
Wardrive menu exits and stops any remaining session.

Combined mode alternates a passive Wi-Fi sweep and a passive BLE window,
tearing down each owned radio between phases. It does not scan both radios
at the same instant. The collector does not associate with access points,
connect to BLE devices, inject packets or request pairing. Address counts are
processed observations; BLE address rotation is not a stable device identity.

## GPS and time

The implemented receiver adapter is XA1110 over I²C. See
[GPS wiring, protocol and diagnostics](WARDRIVE_GPS.md). GPS Status separates
I²C communication, valid NMEA and a satellite fix. A enables user-selected raw
serial output; RIGHT starts/stops a bounded NMEA capture on SD when idle.

GPS UTC anchors an app-owned clock; time zone selection only changes display
formatting. See [time zone coverage](WARDRIVE_TIMEZONES.md). A session started
before valid time uses an `undated-...` directory. If GPS time arrives during
recording, its start time is backfilled in metadata; the directory is not
renamed. Earlier observations do not receive invented positions or timestamps.

## Saved Sessions

Runs live under `/sdcard/wardrive/sessions/`. Saved Sessions shows dated runs
newest first and unknown dates afterwards. Stop recording before exporting,
transferring or uploading. A session offers CSV creation, transfer/upload and
additional recovery/status operations. Raw logs remain available even if
there are no geographically usable CSV rows.

The journal is incremental and RAM use does not grow with recording length.
Recovery rebuilds derived data from the valid CRC-checked journal prefix;
export reports a damaged tail. Source journals are retained. SD write failures
stop collection visibly. There is no automatic formatting or deletion after
upload, and SD power-loss durability still depends on the card/filesystem.
See [file formats](WARDRIVE_FORMATS.md).

## Wireless download and WiGLE

Choose a trusted Wi-Fi network under WiFi Upload Settings using saved profiles,
a scan or manual entry. Joining a new network offers an explicit Save/Skip
choice through the existing POOM profile store. Wardrive's collector remains
passive; association is only for user-requested transfer/upload work.

Transfer over WiFi serves prepared files through a temporary local HTTP
server with the displayed access code. This local connection is unencrypted;
use a trusted network. Setup files and credentials are not served. A whole-run
TAR bundle contains the journal, metadata, CSV and available status reports.

WiGLE uploads require API credentials and explicit on-device confirmation.
Bulk upload has a separate confirmation/hold. There is no automatic upload
on recording completion. See [WiGLE setup and queue behavior](WARDRIVE_WIGLE.md).

## Build and validation

Use the [release-compatible build instructions](WARDRIVE_BUILD.md). The
verified memory profile includes external BSS in PSRAM; simply enabling PSRAM
is insufficient to reproduce the stock release's radio memory behavior.
The profile is opt-in and upstream defaults, boot code and partitions are
unchanged. [Lifecycle and ownership](WARDRIVE_LIFECYCLE.md) describes teardown,
clock handling and limits. [Validation](WARDRIVE_VALIDATION.md) distinguishes
host checks, firmware builds and owner-reported hardware results.
