# Wardrive SD wire formats v1

Root: `/sdcard/wardrive`. Sessions: `sessions/<id>/`. New dated IDs are
`YYYYMMDDTHHMMSSZ-<8-hex-random>`; without a usable clock they are
`undated-<8-hex-boot-ms>-<8-hex-random>`. Existing IDs remain valid and are not
renamed. The manifest's `start_utc` determines dated browsing order/labels;
an absent, malformed or invalid-calendar timestamp remains unknown, not zero.
Provisioning:
`config/wardrive-setup.json`, never downloadable. Exports live beside their
source session rather than introducing a second ID/path convention.

Local-time support adds optional flat metadata fields `display_timezone`
(supported IANA ID) and `display_timezone_source` (GPS, MANUAL, FALLBACK or
LAST GPS). All start/end fields, UTC folder IDs and authoritative journal/CSV
timestamps keep their original UTC meaning. Recorded zones control new-session
labels; legacy metadata uses the current display-zone fallback. Unknown zones
display UTC. No WD01 fields or upload-queue epochs change.

## Authoritative observation journal

`observations.wd` is a concatenation of fixed 192-byte records. No native C
structure is written. Integer byte order is little endian; signed coordinates,
altitude and RSSI use two's complement. Each record starts `WD01` and has an
IEEE CRC32 (reflected polynomial 0xEDB88320, initial/final XOR 0xFFFFFFFF) of its
first 188 bytes. There is no file-level header. Incomplete or invalid records
terminate the recoverable prefix; original bytes remain untouched.

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII WD01 |
| 4 | 1 | kind: 0 Wi-Fi, 1 BLE, 2 GPS history sample |
| 5 | 1 | BLE address type; zero for Wi-Fi/GPS |
| 6 | 6 | radio address; zero for GPS history |
| 12 | 2 | signed RSSI dBm |
| 14 | 2 | channel; zero if unavailable |
| 16 | 2 | Wi-Fi centre frequency MHz; zero if unavailable |
| 18 | 2 | Wi-Fi ESP-IDF 6.1 authentication enum, or BLE advertisement event type |
| 20 | 8 | monotonic receive/sample time in milliseconds since boot |
| 28 | 4 | GPS field presence flags |
| 32 | 4 | signed latitude × 10^7 |
| 36 | 4 | signed longitude × 10^7 |
| 40 | 4 | signed altitude centimetres |
| 44 | 4 | HDOP × 100 |
| 48 | 4 | speed km/h × 100 |
| 52 | 4 | course degrees × 100 |
| 56 | 2 | UTC year |
| 58–62 | 1 each | UTC month, day, hour, minute, second |
| 63–65 | 1 each | satellites, GGA quality, GSA fix type |
| 66 | 1 | name length 0–32 |
| 67 | 32 | raw SSID/name bytes; unused tail zero |
| 99 | 1 | advertisement length 0–31 |
| 100 | 31 | raw legacy BLE advertisement bytes |
| 131 | 1 | GPS connection bit 0, fix bit 1 |
| 132 | 8 | monotonic time of last position sentence |
| 140 | 8 | monotonic time of last supported valid NMEA sentence |
| 148 | 40 | reserved, currently zero |
| 188 | 4 | CRC32 |

GPS flag bits: position 1, date 2, time 4, altitude 8, speed 16, course 32,
HDOP 64, satellite count 128, quality 256, fix type 512. **Ignore values whose
flag is absent.** A numeric zero alone is not presence, and an all-zero GPS
sample does not describe latitude/longitude 0,0. Kind-2 samples are written
approximately once a second, including no-fix/disconnect states. Geographic
CSV omits them and all untimed/unlocated radio records. BLE rotating address
and address-type distinctions are retained in raw logs/index even though CSV
has no equivalent address-type column.

Authenticated browser downloads can create a missing `wigle.csv` on explicit
request. Generation reads this same immutable journal and preserves normal
CRC/UTC/location exclusions. It is cancellable on shutdown/expiry/30-second
budget; partial `.tmp` files are not published or downloadable as a CSV.
Browser paging uses UTC metadata, unknown dates last, and identifies empty
journals. A `.json` manifest contains counts/status, not the observations
needed to reconstruct a geographic CSV on its own.

Every journal write is flushed and fsynced. SD/FAT/controller behaviour can
still lose the last record(s) during power failure. Long write stalls can cause
radio-queue drops, shown on the live screen. A write error stops logging/scanning
with retained data rather than silently continuing or deleting a session.

## Derived device index

`devices.idx` is disposable. First 1,024 bytes: 256 little-endian 32-bit bucket
head offsets. Hash: FNV-1a over an eight-byte key (kind, address type, six-byte
address), modulo 256. A zero head means empty. Nodes are 64 bytes, appended from
offset 1,024, and linked by byte offset:

| Node offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | next node offset, zero for end |
| 4 | 8 | key |
| 12 | 8 | first seen monotonic ms |
| 20 | 8 | last seen monotonic ms |
| 28 | 8 | last logged monotonic ms |
| 36 | 4 | processed report count |
| 40 | 4 | last processed report geographically exportable: 0/1 (older builds reserved/zero) |
| 44 | 4 | signed strongest RSSI |
| 48 | 16 | reserved |

The runtime index includes reports suppressed by the logging interval. Rebuilding
from the journal necessarily reconstructs statistics for retained observations,
not suppressed reports; `recovery.json` reports the recovered counts. Rebuilds
never rewrite original observations or session settings. The index does not
store a claimed transmitter location. It uses bounded stack buffers, with no
RAM allocation proportional to address count.

A report that becomes geographically exportable after an unlocated, untimed,
or disconnected report bypasses the repeat interval. This retains the first
usable observation after acquisition/reacquisition without altering the location
or time of any earlier record. Eligibility uses a connected, valid fix and
present UTC date/time. Repeated eligible reports retain the normal interval/RSSI
policy. Index recovery reconstructs this flag from retained journal reports;
the authoritative WD01 record format is unchanged.

## Metadata and exports

`session.json`: schema version, state (Active/Paused/Completed/NeedsRecovery),
receiver identifier, mode (0 Wi-Fi/1 BLE/2 combined), processed unique counts,
logged radio/GPS counts, unlocated count, write errors, scan/log settings,
monotonic start/end and UTC start/end when known. An empty UTC string means
unknown. The manifest is checkpointed at start, pause/resume, every 30 seconds
and stop using a temporary file and rename. A power cut may leave an older
manifest; recover from the journal. `upload_status_file` and `export_status_file`
point to the canonical independent status files.

`wigle.csv`: UTF-8 CSV v1.6 with the documented pre-header and fourteen columns:
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,
CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type.
Coordinates are observation points. Repeat rows preserve time/position history;
FirstSeen is the observation-row UTC timestamp, not a manufactured AP location.
Capabilities include only authentication information actually reported; unknown
modes remain UNKNOWN, without inventing cipher details. Valid UTF-8 is preserved;
non-UTF-8 bytes/unrepresentable controls become visible `\xNN` escapes. Raw
journal names remain lossless within the IDF-reported name capacity.

`export.json`: format, exported row count, unlocated/untimed count, damaged-tail
flag. Export stops at a damaged tail and retains earlier valid rows. CRC errors
never generate geographic rows. Source records are never edited.

`recovery.json`: valid radio observation count, recovered distinct Wi-Fi/BLE
counts and damaged-tail flag. This does not assert the original session finished
normally. A clean record boundary after power loss can have no damaged tail
and still lack a Completed manifest.

`upload.json`: integer state (0 Pending, 1 Uploading, 2 Submitted, 3 Confirmed,
4 Failed, 5 Needs Review), attempts, transaction ID and UTC retry-after epoch.
Missing file means Pending. A malformed file fails closed. A persisted Uploading
is interpreted as Needs Review after restart, never Pending. See operation guide
for retries, account verification and explicit queue reset. Queue writes use a
temporary file, fsync and rename; source files are never deleted.

`session.tar`: standard USTAR, 512-byte records, containing available
session.json, observations.wd, wigle.csv, export.json, recovery.json and
upload.json. No absolute paths, credentials, configuration or rebuildable index
are included. Archive generation needs one source file plus one destination,
not RAM proportional to session size. Regenerate after new exports/status
changes if an up-to-date archive is needed.

The UI calls this **Prepare download** and the browser calls it **Session
bundle (.tar)**. Preparation regenerates CSV before bundling; zero located rows
does not prevent raw-log download. A missing journal, unexpected file-stat
error or short read prevents publication of a successful bundle. Existing
source files and a previously published bundle are preserved on failure.

## FAT replacement recovery

ESP-IDF FatFs rename cannot overwrite an existing file. Closed/flushed temporary
files are published by moving the current destination to `<filename>.bak`,
renaming the temporary into place and removing the backup after success. An
install failure attempts to restore the old destination. A power cut can leave
only the backup; this is recoverable but is not an atomic rename guarantee.
The next publication restores that backup before replacing it. Upload-state
reads also use a sole committed backup immediately, so an interrupted Uploading
state remains Needs Review rather than becoming Pending. Never delete a sole
backup merely because its canonical filename is absent. These files contain
session metadata/exports, never credential provisioning. Raw journals are not
subject to this replacement procedure and are never removed by it.

## Raw GPS diagnostic capture

`/wardrive/diagnostics/gps-xxxxxxxx-xxxxxxxx.nmea` is a separate text capture,
never an automatic WiGLE submission. Exclusive creation protects older captures.
Comment lines beginning `#` identify format and each frame's monotonic ms,
checksum result and parser-support result. Following `$...*HH` lines retain
complete framed NMEA received by the service, including unparsed GSV/VTG and
checksum failures. No one-Hz sampling is applied to this file. The on-screen GSV
counter counts checksum-valid GSV sentences, not satellites. Missing GSV does
not prove zero satellites visible: output settings may omit it.

Capture uses bounded buffers and one file slot, syncs at one-second intervals,
and stops at 20 minutes or 4 MiB. RIGHT/B closes it; filesystem faults preserve
the partial file and show an error. Unexpected power loss can lose buffered
output. The authenticated Wireless Transfer `/gps` endpoint accepts only the
exact generated filename pattern under the diagnostics directory and streams
512-byte chunks; it cannot access provisioning or arbitrary paths. Raw logs
may include coordinates and are subject to the same HTTP privacy limitations
as other downloads.

### Upload problem classification (2026-11-09)

`upload.json` adds optional integer `problem`: 0 none, 1 TLS/connect failure,
2 authentication rejection, 3 rate limit, 4 CSV/SD read failure, 5 uncertain
response and 6 interrupted submission. Old markers without this field remain
readable as problem 0. Unknown/out-of-range values fail validation. Reading an
interrupted Uploading marker returns Needs Review/problem 6. The field carries
no API credentials, arbitrary server response or observation data. State,
attempts, transaction and UTC retry_after keep their existing meanings.

Upload diagnostics extend `problem` with 7 DNS lookup, 8 certificate validity/UTC,
9 other certificate verification, 10 allocation failure, 11 connection timeout
and 12 request header too long. Optional signed 32-bit fields `net_error`,
`tls_error`, `tls_code`, `tls_flags` and `socket_error` preserve the respective
HTTP open, ESP-TLS, mbedTLS, verification and socket errors captured before close.
Missing fields default to zero; invalid types/ranges are rejected. They contain
no credentials or server-response text. Existing markers remain readable.
Older firmware may refuse newer problem values rather than retrying them.
