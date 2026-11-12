# GPS compatibility and XA1110 transport

| Receiver | Classification | Transport / detection |
|---|---|---|
| SparkFun GPS-14414 / SEN-14414 XA1110, MT3333 | Hardware-tested basic I²C/NMEA and static fix; updated padding fix awaits re-test | Qwiic I²C address 0x10, stream protocol; ACK plus checksum-valid supported NMEA |
| Other hardware-tested GPS receivers | None | No other models tested |
| u-blox Qwiic/DDC receivers | Planned | Separate adapter required; not the XA1110 protocol |
| Other Qwiic receivers | Planned / unsupported until an adapter exists | Do not assume address or stream compatibility |
| UART / BLE GPS | Planned only | Physical routing, transport and receiver-specific work required |
| Arbitrary USB GPS | Unsupported | No generic USB host GPS support is promised |

The main board target is ESP32-C5. `board/include/bsp_pong.h` supplies SDA GPIO 0,
SCL GPIO 1, I²C port 0 and 100 kHz. The unrelated C6/default driver pins are not
used. Reuse the OLED's existing shared bus and mutex. No bus deletion, bus-speed
change, OLED reinitialisation or interrupt GPIO assignment is introduced.
The XA1110 consumes one of the existing four device slots, retained for reuse
across app visits. Its transactions use the new explicit 30 ms stream timeout;
existing staged I²C callers retain their previous timing and behaviour.

The POOM expansion connector is advertised as Qwiic-style. Its actual voltage,
pin order, regulator capacity and power margin with SD/radios/display have not
been independently verified. Standard Qwiic and the XA1110 are nominally 3.3 V,
and the module's typical current is approximately 25 mA. These are interface/
module reference specifications, not measured POOM properties. **Verify the electrical interface before connection.** The owner subsequently
reported basic receiver operation and supplied a 20-minute capture with fixes;
this does not establish a guaranteed connector current rating.

## Protocol investigation

References inspected:

- SparkFun hookup guide:
  https://learn.sparkfun.com/tutorials/sparkfun-gps-breakout---xa1110-qwiic-hookup-guide/all
  (the guide's web endpoint returned HTTP 403 in this environment).
- Hardware and module reference repository:
  https://github.com/sparkfun/SparkFun_GPS_Breakout_XA1110_Qwiic
- SparkFun's reference stream implementation:
  https://github.com/sparkfun/SparkFun_I2C_GPS_Arduino_Library/blob/master/src/SparkFun_I2C_GPS_Arduino_Library.cpp

The manufacturer's library actually performs a dummy zero write with a STOP to
0x10, followed by stream reads (32-byte requests in its Arduino implementation).
It treats LF (0x0A) as idle filler; this is not a memory-mapped random-register
read and is not u-blox's byte-count/register interface. The independent ESP-IDF
adapter follows that wire sequence, bounds reads to 32 bytes and holds the
shared mutex only for a transaction. The UI drains up to eight chunks between
OLED frames so a single 32-byte read per slow display frame does not become
the stream bottleneck. Real FIFO/throughput/clock-stretching behaviour still
needs measurement with the actual receiver. No optional IRQ is assumed.

The XA1110 adapter removes all LF idle-fill bytes, including those received
between partial sentence reads; CR ends the sentence. Other adapters retain
the generic parser's normal CR/LF handling. `$` resynchronises
a frame. Frames have a 127-character maximum, strict hexadecimal XOR checksum
validation and a bounded field table. The parser supports talker-independent
RMC, GGA and GSA. It rejects invalid coordinates, minutes outside 0–59.999…,
non-finite numbers, malformed UTC, impossible calendar dates and invalid flags.
RMC's two-digit year is interpreted as 2000–2099; geographic export/system-clock
use additionally requires year 2024–2099. This epoch policy is explicit, not a
claim to solve receiver week-rollover faults.

GGA quality 0, 6 (dead reckoning), 7 (manual) and 8 (simulation) do not yield a
GNSS position. GSA no-fix invalidates position. No-fix RMC/GGA data does not reuse
coordinates for a newly received observation. Position expires at 2.5 seconds;
connection expires at five seconds; date/time have separate 2.5-second freshness.
A midnight GGA rollover clears the old RMC date until RMC supplies a new date.

The driver leaves update rate, constellations, SBAS and antenna configuration
unchanged. It sends no PMTK settings, persistent commands or reboot commands.
The default 1 Hz update rate is the starting assumption to validate on hardware.

## Extending GPS support

`poom_gps_fix_t` is the standard snapshot. `fields` presence bits distinguish an
unavailable value from a valid zero. It includes coordinates, UTC, fix state,
quality/type, satellites, HDOP, altitude, speed, course, freshness timestamps,
connection state and adapter identification. HDOP is not reported as metres.

A `poom_gps_adapter_t` supplies `open`, bounded `read`, optional `close`, a name
and context. Return -1 for a communication error and 0 for idle. Names are static
identifiers of at most 23 printable characters (no JSON quotes/control bytes).
The GPS service owns framing, retry and parsing. The UI selects the XA1110
adapter explicitly today; ACK plus valid NMEA validates the selected protocol,
not an independently queried manufacturer/model identity. It never probes
unrelated device addresses or writes receiver configuration during detection.

For a different NMEA transport, add/register an adapter and its UI selection;
the observation engine, journal, exporter and upload queue do not require device
changes. A proprietary non-NMEA receiver should translate into the same presence-
flagged snapshot rather than leaking device dependencies into wardriving.
The service is caller-owned and polled; it creates no hidden task or unbounded
buffers. Diagnostics are a callback, off by default.

## Licensing

The SparkFun Arduino source inspected carries GPL notices. No Arduino library
source or parsing implementation is incorporated or linked. These new MIT files
are independently implemented; the reference is used to understand the external
wire protocol. Retain this provenance note. Existing repository licences remain
unchanged.

## Updated connection-readiness evidence

The 2026-11-08 follow-up found an explicit published 3.3 V Qwiic description in
the POOM manufacturer's Maker guide, with matching bus pins in its hardware
reference. No revision-specific available-current rating or regulator schematic
was located. Independent rail/pin-order verification and power-budget confirmation
remain hardware-specific. Confirm electrical compatibility for the actual board/receiver revision; a software test does not certify a power budget.


## First full hardware capture and stream-framing correction

The submitted 20-minute near-window SD capture contained 1,161 complete GGA
fix reports using 5–10 satellites, with median HDOP 1.02. The first recorded fix
was approximately 36 seconds after capture began. All subsequent complete GGA
reports indicated a fix. This confirms basic receiver operation under that
specific test; it does not certify field endurance or all radio/SD combinations.

However, 863 records were incomplete, and valid RMC messages had gaps up to
8.012 seconds. Review found that the adapter was passing documented LF filler
to the generic parser as a line terminator, potentially splitting an unfinished
sentence. The adapter now strips every LF before framing, matching the reference
wire behaviour. A regression injects LF padding inside sentences/checksums.
BAD now includes malformed-frame errors as well as true checksum mismatches.
Later owner tests acquired GPS time/position and produced dated sessions. This does not quantify every framing error from the earlier capture. Receiver update rate/settings are unchanged.
