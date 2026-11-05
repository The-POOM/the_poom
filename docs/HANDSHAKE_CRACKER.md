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
- **SD-card wordlists:** run *your own* dictionary files from the SD card (not
  just the built-in list) — see [Bring your own wordlist](#bring-your-own-wordlist-sd-card).

### Modes (confirm screen, cycle with LEFT/RIGHT, A to start)
QUICK and SMART both run the built-in 230-key list — the difference is how many
rule variations each key is tried with.

- **QUICK** — each key four ways: **as-is**, **Capitalized**, **+`!`**, and
  **Capitalized +`!`** (e.g. `password` → `password`, `Password`, `password!`,
  `Password!`). ~606 candidates, roughly 70 s. The most common weak transforms,
  fast.
- **SMART** — everything QUICK does, **plus** three more suffix rules (**+`1`**,
  **+`123`**, **Capitalized +`1`**), **and** it also runs the network's own name
  (**ESSID**) through all of those rules. ~1140 candidates, roughly 2 min —
  about double the coverage.
- **SD LIST** — run one of your own wordlist files from the SD card (see below).

After a QUICK miss, **A:MORE** runs *only* the extra SMART rules (never what
QUICK already tried), so QUICK → A:MORE equals a full SMART run with no repeated
work. Capitalize applies only to keys that contain a real word (a run of ≥2
letters), so it's skipped for all-digit or keyboard-walk keys like `12345678`.

### Bring your own wordlist (SD card)
You are not limited to the built-in 230-key list — put your own dictionary
files on the SD card and run them as a full dictionary attack:

1. Create a `wordlists` folder at the SD-card root: `/sdcard/wordlists/`.
2. Copy your wordlist files into it — plain text, one candidate per line. Files
   ending in `.txt`, `.lst`, `.dic`, or `.dict` are listed.
3. In the cracker's confirm screen, cycle to **SD LIST**, press **A**, and pick
   your file. Every line of WPA-valid length (8–63 chars) is tried against the
   handshake, with a live tried / total, %, and PSK/s readout, and **B:CANCEL**
   at any time. A "may take a long time" warning is shown first.

At ~9 PSK/s a ~7,000-word list takes roughly 15 minutes, so trim large dumps to
likely candidates — million-line lists (rockyou, weakpass) aren't practical to
run in full on-device; extract a top-N from them instead.

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
- The crypto hot loop (`poom_handshake_crack.c`) is compiled at `-O2` —
  overriding the project's default optimization level for just that file — so
  cracking speed stays up regardless of the global build type. Set via
  `set_source_files_properties` in `applications/poom_app_pack/CMakeLists.txt`.
