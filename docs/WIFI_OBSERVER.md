# BEAST Wi-Fi Guard and Recon

`WIFI GUARD` contains Pwnagotchi, Pine-like, Multi SSID, Flock candidate and all-alert
views. `WIFI RECON` contains session totals, access points, stations and probe
requests. These use one native receive owner and stop it when leaving the app.

- Left/right change views; up/down select an observed record; A opens details.
- In details, A locks to the record's observed channel or resumes hopping, right
  saves the session, and B returns to the list. Rejected channel changes are shown.
- AP details: up/down show the first three distinct advertised SSIDs; left ends
  the observation session and hands that AP to existing FOX HUNT. Save before
  this handoff if the current session is needed. B from this direct hunt returns
  to BEAST. FOX HUNT retains its existing scan/feedback behavior.
- The save view writes JSON Lines under `/sdcard/wifi_guard` or `/sdcard/wifi_recon`.
  Each line is a JSON object; quotes and backslashes in displayed SSIDs are escaped.
  Saving is explicit. Failed SD writes are reported. A new session starts empty.
  Files are created exclusively with a collision sequence, so a repeated uptime
  after reset cannot overwrite an earlier report.

The bounded session retains 32 APs, 48 stations and 32 distinct MAC/SSID probe
pairs. Entries are not silently replaced. Table overflow counts represent
observations that could not be retained, not a count of distinct omitted devices.
The RX queue holds ten frames, drops oversized management frames, and stores only
the headers needed from data frames. Drop and tune-skip counts remain visible and
are included in the report. Model and receive storage are allocated in PSRAM
only while this app is open; receive code never parses JSON, draws, or writes SD.

An AP or station becomes stale after 30 seconds without an observation. Joins and
departures describe observation-window transitions, not verified association or
physical presence. Relationships come from valid ToDS/FromDS or association frames;
they are not inferred from probe requests. Randomized client addresses can appear
as separate records. Retry flags and repeated MAC/SSID probe pairs are counted.
Hopping, channel locking, congestion and packet loss affect all these observations.

Pwnagotchi means a valid size/depth-bounded JSON advertisement with the expected
fields, not authenticated identity. Pine candidates match upstream OUI or minimal
beacon patterns; three observations within ten seconds set the confirmed-pattern
flag. Multi SSID means at least three distinct raw SSIDs were seen for one BSSID.
Display sanitization does not merge different raw SSIDs. Flock candidates preserve
separate name-pattern and shared-vendor-OUI reasons, including probe senders.
None of these patterns proves intent, product identity or a security incident.
Activity alerts preserve authentication, probe, CSA, EAPOL Logoff, SAE and deauth
bursts, counted across APs over five-second windows.

Report schema 1 includes session counters and AP, station, probe and activity rows.
AP/probe flag bits: 1 multi SSID, 2 security conflict, 4 Pine OUI, 8 Pine pattern,
16 repeated Pine pattern, 32 Pwnagotchi JSON, 64 Flock name candidate, 128 Flock
shared-OUI candidate. The last two bits may also appear on probe rows. Names are
printable display forms; reports do not pretend to preserve binary SSID bytes.

Run portable parser and actual receive-lifecycle tests after IDF resolves cJSON:

```powershell
.\tests\host\run_wifi_observer.ps1 -Compiler gcc
```

Tests cover truncation, malformed/deep JSON, split JSON IEs, security ordering,
raw-name distinction, Flock reason separation, Pine confirmation, relationship
changes, wraparound, table saturation, JSON report parsing and 30,000 deterministic
random frames. Mock radio tests exercise initialization failures, existing-owner
refusal, queue overflow, deferred parsing, failed tuning, stop and late callbacks.
They do not validate reception, RF identity accuracy, SD hardware or live Fox Hunt.

Radio teardown errors retain the owner and remain visible while cleanup is
retried. CLI handoff can call `menu_wifi_observer_stop()` and check its boolean
acknowledgement (five-second bound), or inspect `menu_wifi_observer_is_active()`.
An active failed teardown must not be treated as a free radio.
