# POOM WPA Handshake Cracker

On-device WPA/WPA2 handshake auditing for POOM firmware (ESP32-C5).

> **Authorized use only.** This is an offensive Wi-Fi security tool. Use it
> **only** on networks and equipment you own or have explicit, documented
> permission to test. Capturing or cracking handshakes for networks you are not
> authorised to assess is illegal in most jurisdictions. You are solely
> responsible for how you use it.

## Overview

The cracker runs a dictionary/rules attack **on the device** against a captured
WPA/WPA2 handshake, using the `.22000` (hashcat-22000) files that POOM's
existing handshake-capture path already writes to `/sdcard/pcaps/handshakes/`.
It recovers only weak or default pre-shared keys; it is not a brute-forcer (see
*Expectations* below).

## Features

### Cracking
- Parses hashcat-22000 captures: `WPA*01` (PMKID) and `WPA*02` (EAPOL).
- Self-contained crypto (SHA-1, MD5, PBKDF2-HMAC-SHA1) tuned for the C5 — no
  external crypto dependency.
- **Built-in list:** 230 common default / weak-pattern keys (numeric patterns
  from real cracked-WPA data, keyboard walks, and generic factory/default
  words). Deliberately small and default-focused, not a general passphrase list.
- **SD-card wordlists:** stream any `.txt`/`.lst`/`.dic`/`.dict` from
  `/sdcard/wordlists`, tried as-is. A pre-pass counts valid (8–63 char) lines,
  then cracking shows tried/total, %, and live PSK/s. A "may take a long time"
  warning is shown before a large SD run.

### Modes (confirm screen, cycle with LEFT/RIGHT, A to start)
- **QUICK** — the 230 keys with as-is / Capitalize / `+!` / Capitalize+`!`
  rules (~606 candidates, roughly 70 s). After a QUICK miss, **A:MORE** runs the
  remaining rules without repeating anything already tried.
- **SMART** — all rules plus ESSID-derived candidates (~1140, roughly 2 min).
- **SD LIST** — pick a wordlist file and stream it.

### Attempt journal
- Per-AP record (keyed by BSSID + ESSID) of which lists were tried and the
  outcome, stored as a human-readable CSV on SD
  (`/sdcard/pcaps/handshakes/crack_journal.csv`) and mirrored for cracked keys
  in NVS. Already-cracked APs show their key instantly; a list already tried as
  a miss prompts before re-running; cracked handshakes are marked in the
  browser. Both stores degrade gracefully if SD or NVS is unavailable.

## How to reach it
1. **BEAST → CRACK HS** — browse captured handshakes and crack one.
2. **Capture screen → A:CRACK** — crack the handshake just captured.

## Controls
- Browser: UP/DOWN select, A opens, B back. Newest captures first; 0-byte files
  are skipped; already-cracked entries are marked.
- Confirm: LEFT/RIGHT choose QUICK / SMART / SD LIST, A starts, B back.
- Running: live progress; **B:CANCEL** stops (also during the SD count pass).
- Result: shows the key if cracked, or tried / elapsed / PSK/s; **A:MORE** after
  a QUICK miss.

## Expectations and limitations
- **Throughput is ~9 PSK/s** on the C5 (4096×2 PBKDF2 iterations per guess is
  fixed by the standard). QUICK is ~70 s; keep SD runs bounded (a "long" run of
  ~7k words is ~15 min). Million-line GPU lists (rockyou/weakpass) are not
  feasible on-device — extract a top-N from them instead.
- **Only weak/default keys are recoverable.** Modern APs ship unique
  per-device or algorithmic ISP keys that are not flat-dictionary-crackable;
  the built-in list only covers the small weak-static/pattern space.
- **Built-in list provenance:** the numeric keys and keyboard walks are
  empirical; a subset of generic default words (e.g. `router123`,
  `wifipassword`, `default123`) is curated, not scraped. The list favours
  objective default/weak patterns over human-chosen passphrases by design.

## Build and testing
- ESP-IDF v6.1, target `esp32c5` (also builds `esp32c6`). Run `idf.py fullclean`
  once after adding the new source files (the app-pack `CMakeLists.txt` globs
  sources at configure time), then `idf.py build flash monitor`.
- The crypto hot loop is pinned to `-O2` in `applications/poom_app_pack/
  CMakeLists.txt` so it stays optimised regardless of the global build type.
