# Offline local time for Wardrive

GPS supplies UTC date/time; a position does not itself contain a time zone.
Wardrive combines these inputs locally using a small regional lookup and rule
table. No location lookup service, network or independent POOM RTC is required.
The running clock advances between updates; after a complete power-off, GPS
or network sync must supply UTC again.

## Controls and persistence

Open **Wardrive > Settings > Time zone**. UP/DOWN selects a row; LEFT/RIGHT
changes it and A behaves like RIGHT. B saves and returns to Settings.

- MODE AUTO GPS chooses a supported regional interior from a fresh valid fix.
  FB selects the fallback for unsupported/border areas or initial no-fix.
- MODE MANUAL always uses the selected TZ, overriding GPS detection.

The four-byte versioned non-secret configuration is saved through the existing
NVS service under `wd_timezone`, separately from scan settings and credentials.
Invalid/missing versions revert to Auto GPS with UTC fallback. TZ SAVE FAILED
means persistence was not established, although the current app visit can use
the changed setting. No credential persistence, partition, encryption,
bootloader or eFuse change is involved.

The page shows the selected zone, local clock and source. GPS Status also
shows that clock when capture status does not occupy the last data line:

- GPS: supported region chosen from a fresh position.
- LAST: last detected zone retained during a fix loss, for this app visit only.
  This is last-known context, not proof of current location.
- MAN: manual choice.
- FB: fallback; detection is unavailable or outside the supported cores.

Before UTC sync it reads TIME WAIT GPS/NET. A zone setting cannot supply a
missing clock date/time. Neither last position nor last detected zone is saved
by this feature. Set your preferred fallback for predictable behaviour before
a new fix, including after restart.

## Initial compact region pack

There are 16 hand-authored conservative interior windows, not a worldwide
boundary database. Representative locations include Sydney, Canberra,
Melbourne, Hobart, Brisbane, Cairns, Adelaide, Darwin, Perth, Broken Hill,
Eucla, Lord Howe, Norfolk, Auckland, Wellington, Christchurch and Chatham.

Exact bounds live in `wardrive_timezone.c`. Country/state borders, islands
and Eucla highway communities are not approximated by filling every gap.
For example, the NSW/Queensland border near Tweed, the SA/NT/Queensland
junction and much of the Eucla highway corridor deliberately fall back.
Windows also cover some nearby water and are not official maritime boundaries.
This is a convenience map, not navigation data. Use manual override if a core
is unsuitable. New cores do not require engine, exporter or storage changes.

There are 26 manual choices: UTC; Sydney, Melbourne, Brisbane, Adelaide,
Darwin, Perth, Hobart, Eucla, Lord Howe, Norfolk, Broken Hill; Auckland/Chatham;
London/Central Europe; US Eastern/Central/Mountain/Pacific, Arizona, Hawaii;
Japan, Singapore, India and Nepal. This is not the full IANA catalogue.
Automatic detection remains restricted to the AU/NZ cores.

## Local display and UTC records

New sessions record optional `display_timezone` (a supported IANA identifier)
and `display_timezone_source`, alongside existing UTC start/end fields. The
zone selected at session start is retained for later display. Starting before
UTC is known does not retroactively invent a start time.

Saved Sessions displays, for example, `26-11-08 17:56AEDT`. The selected detail
includes seconds: `08-11 17:56:43 AEDT`. Compact local list labels use minutes
to fit the abbreviation; UTC-only labels retain their seconds/Z format.
Browser headings include full local time and numeric UTC offset, with a
separate UTC start line. Repeated local times at DST end are distinguished by
their abbreviation; sorting always uses UTC.

Older sessions without zone metadata use the current selected zone for display.
A recorded supported zone takes precedence; an unrecognised recorded zone
displays UTC. No old folders or timestamps are rewritten. UTC folder IDs,
raw observations, GPS history, CSV timestamps, WiGLE uploads, queue epochs
and TLS certificate time remain UTC. No process-wide TZ variable, tzset or
localtime policy changes, so other apps keep their clock behaviour.

## Rules, sources and tests

The table implements current post-2024 AU/NZ, EU and US schedules and fixed
offset choices, including Lord Howe's half-hour DST and Chatham's shifted
transition clock. It is not historical tzdb. Legal changes require a firmware
rule update; future-year tests compare current tzdb predictions and do not
guarantee future legislation.

Primary references checked 2026-11-08:

- [NSW time definitions and Lord Howe exception](https://dcj.nsw.gov.au/about-us/daylight-saving/definitions-of-time-and-how-time-is-regulated.html)
- [Australian schedules](https://www.rba.gov.au/schedules-events/daylight-saving.html)
- [NZ daylight-saving legislation and Chatham time](https://www.legislation.govt.nz/regulation/public/2007/0185/latest/DLM1348400.html)
- [US daylight-saving rules](https://www.nist.gov/pml/time-and-frequency-division/popular-links/daylight-saving-time-dst)
- [UK clock changes](https://www.gov.uk/when-do-the-clocks-change)
- [IANA time-zone database](https://www.iana.org/time-zones)

The implementation is original MIT-licensed code with no imported boundary
dataset. Host tests compare 5,698 conversions with independent Python IANA
zoneinfo across 26 zones in 2024, 2026, 2027, 2028, 2032, 2037 and 2099,
including exact-second DST boundaries, repeated hours and date rollover.
Other checks cover cores/guard gaps, invalid coordinates, manual/fallback,
fresh/stale GPS, simulated NVS save/reload and invalid config, OLED layout,
local browser headings, UTC sorting through repeated hours and unchanged
UTC CSV timestamps despite a Sydney display zone.

Physical validation remains: local clock at your position; manual override;
B save and re-entry/reboot persistence; GPS loss with LAST; saved-session dates;
and browser local/UTC headings. These device tests are not claimed completed.
