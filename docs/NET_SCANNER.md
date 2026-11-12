# POOM Network Scanner & Security Auditor

A LAN scanner and lightweight security auditor for POOM firmware (ESP32-C5,
the release ESP-IDF snapshot). It joins a Wi-Fi network, discovers devices, finds open ports,
identifies services, products and operating systems, looks up **published
exploits** for what it found in an offline Exploit-DB copy on the SD card, and
flags **risky configurations** - all from the handheld, no PC needed.

> **Authorised use only.** Only scan networks and devices you own or are
> explicitly authorised to assess. Every check is non-destructive: the POOM
> connects, reads what services announce about themselves and compares that
> with offline data. It never runs exploits, never tries passwords and never
> changes anything on the devices it scans. Exploit results are *leads* for you
> to verify, never proof of a vulnerability.

Network Scanner lives under **THE BEAST -> NET SCANNER**. It is independent of
**WIFI DETECT** and the ARP **SCAN NET** app.

---

## Quick start: Full NetAudit

1. Open **NET SCANNER**, connect to a network (*Saved Profiles* or *Scan
   Networks*).
2. On the **Devices** list choose **Full NetAudit**.
3. Pick a port range (**Quick** is a good start), check the estimate, press **A**.
4. The POOM runs every step on its own:
   `Discover -> Ports + services -> Identify devices -> Web checks -> Exploit-DB lookup`.
   Press **B** at any time to stop; whatever was gathered so far is kept.
5. Browse the **Network Vuln Summary** (see below); its last row, **Save
   report**, writes a text report to the SD card when you want one.

Results stay available until the next scan: **Devices -> View Last Results**.

---

## SD card setup (for the exploit lookup)

The exploit lookup needs the Exploit-DB metadata CSV on the SD card (FAT):

1. Get `files_exploits.csv` from the official Exploit-DB repository
   (`exploit-database/exploitdb` on GitLab; it is the same file SearchSploit
   uses).
2. Copy it to the card as `/searchsploit/files_exploits.csv`
   (on the device: `/sdcard/searchsploit/files_exploits.csv`).

That is all. The first lookup reads the whole CSV and **builds an index**
next to it (`files_exploits.idx`, about 0.7 MB) - the progress screen says
*"Building index (once)"*. Later lookups use the index and only read the parts
of the CSV that can match, which is much faster. When you replace the CSV with
a newer one, the index is rebuilt automatically on the next lookup. A refresh
every month or so is plenty.

Without the CSV everything else (discovery, ports, identification, exposure
findings) still works; the lookup reports *"No EDB on SD card"*.

---

## Screens

**Wi-Fi** - current network, *Scan Hosts* / *Change Net* / *Disconnect*, or
*Saved Profiles* / *Scan Networks* when offline. Credentials use the shared
saved-Wi-Fi profiles (same store as Settings -> WIFI PROFILES).

**Devices** - actions, then the discovered devices (IP + hostname, or the
manufacturer when no name is known):

| Row | What it does |
|---|---|
| Full NetAudit | The one-tap audit above |
| Refresh Devices | Re-discover devices (runs automatically the first time) |
| Scan All Ports+Svc | Port scan + service identification on every device -> **Net Summary** |
| Manual Target | Type an IP (the local subnet is pre-filled) |
| View Last Results | Re-open the last Scan All / NetAudit (shown when there is one) |
| *a device* | Its host page: name, MAC, manufacturer, OS + how sure, ping TTL -> *Port Scan* |

**Port scan results** (one device) - open ports with the identified service;
closed/filtered are counted. **A** on a port opens its detail page (service,
product, version, banner, evidence); **L** runs **Assess** for that device.

**Net Summary** (after Scan All) - devices with open ports. **L** runs
**Assess All**: one pass over the Exploit-DB for every product found on the
network.

**Network Vuln Summary** - four categories, each opens a list of devices
(shown as last IP octet + what the device is, e.g. `.96 SRS-ZR5`):

| Category | Picking a device shows |
|---|---|
| Potential | *By exploit* / *By service* / *Device info* for that device |
| No match | Its identified products with no Exploit-DB entry |
| Unassessed | Its open ports where nothing could be identified |
| Exposures | Its findings: *Confirmed* (seen by a web check), *Weaknesses found*, *Good to know* |

**Exploit list** - one row per CVE (with a count), strongest first. **Left /
Right** cycle the filter **ALL / STRONG / MEDIUM / WEAK**. **A** opens the
entry: title, service, version, type, platform, CVE, how strong the match is,
and whether the exploit's platform fits the device's OS. Every entry is marked
**POTENTIAL - verify**.

Detail pages scroll with **Up / Down**; `^` / `v` at the right edge show there
is more.

### Buttons

| Button | Action |
|---|---|
| A | Select / confirm |
| B | Back one screen; cancels a running step |
| Hold B | Leave to the POOM main menu (Wi-Fi is switched off on the way out) |
| Up / Down | Move in lists, scroll pages |
| Left | Context action: *Svc ON/OFF* on the port-range screen, *Assess* on results, *Assess All* on the Net Summary |
| Left / Right | Tier filter on the exploit list |

---

## How it works

**Discovery.** The subnet comes from the assigned IP and netmask (not assumed
to be a /24) and is bounded. Devices are found by ARP, which every IP device on
the LAN must answer; client-isolated networks simply show few peers.

**Port scanning.** TCP Connect over lwIP, a few connections in parallel,
classifying *open / closed / filtered*. Port sets: *Quick* (common ports),
*Extended* (1-1023 + common high ports), *Full* (1-65535) - all fed through a
small window so memory stays fixed - and *Custom* (e.g. `22 80 443` or
`8000-8010`, up to 256 ports). TCP SYN scanning is not available: lwIP cannot
suppress its own RST to a half-open reply, so results would be unreliable; it
is never silently replaced by TCP Connect.

**Service and product identification.** For each open port the POOM reads what
the service announces: SSH / FTP / SMTP / POP3 / IMAP greetings (e.g. OpenSSH,
vsftpd, ProFTPD, Exim, Postfix, Dovecot), the MySQL/MariaDB handshake, Telnet
and VNC; for web servers the `Server` header, the login realm or page title
(where routers and cameras often name their model, e.g. `R7000`) and
`X-Powered-By`. A port number alone is only ever shown as a guess.

**Device and OS identification.** After the port scan a short *Identify* step
sends one ping per device (reply TTL: ~64 Unix-like, ~128 Windows, ~255
embedded) and the standard discovery queries devices answer for themselves:
SSDP/UPnP (including the device description: manufacturer, model, firmware),
mDNS (hostname, model) and NetBIOS (Windows/Samba names). These are combined
with service banners, the open-port pattern and the MAC manufacturer into one
identity with an honest confidence level:

| Level | Based on |
|---|---|
| confirmed | what the device reports itself (banner, UPnP/mDNS model, SSDP) |
| inferred | ping TTL family |
| guess | open-port pattern, MAC manufacturer, being the gateway |

A UPnP model (e.g. a router's model number) is looked up in Exploit-DB like one
more service. Full Nmap-style OS fingerprinting is not possible on lwIP (it
needs hand-crafted packets).

**Exploit lookup (offline SearchSploit).** Each identified product is matched
as a whole word against Exploit-DB titles in one pass (e.g. "apache" does not
match "ApacheBench"), then ranked:

| Tier | Meaning |
|---|---|
| STRONG | the title names the detected version |
| MEDIUM | the title names some other version |
| WEAK | product only |

Duplicates of the same CVE are grouped. When a device's OS is *inferred* or
*confirmed*, exploits for a different platform (e.g. Windows-only entries on a
Linux device) are left out; a mere *guess* never hides a lead. A match is
always only **Potential**: titles are free text and versions are inconsistent.

**Exposure findings.** A small rule table checks what the scan already
observed - no extra traffic:

| Finding | Severity |
|---|---|
| Telnet enabled, FTP enabled, database port reachable, OpenSSH older than 7.4 | weakness |
| Remote desktop open (RDP/VNC), SMB file sharing, MQTT without TLS, web UI without HTTPS, UPnP enabled, raw print port | good to know |

Rules are written in a tiny condition language (`port:`, `service:`,
`product:` with optional version comparison, `upnp`, combined with `& | ! ( )`)
in `modules/poom_net_audit/poom_net_audit_rules.c`; edit the table there and
rebuild to change them.

**Web checks (Confirmed findings).** On each device's plain-HTTP ports the
POOM makes a few read-only `GET` requests for well-known paths and looks for
tell-tale text in the reply: a directory listing, an exposed `.git` folder,
a public Apache `/server-status` or nginx status page, a `phpinfo` page, a
readable `.env` secrets file. GET only, fixed paths, no payloads, no logins -
it can only notice that something is exposed. A request for a random nonsense
path first tells it whether the device answers every URL with the same page, so
such devices cannot produce false hits. Hits are shown as **CONFIRMED** at the
top of the device's Exposures page. HTTPS ports are not checked (no TLS). The
checks live in `modules/poom_net_audit/poom_net_audit_checks.c`.

**Audit report.** On request - **Save report**, the last row of the Network
Vuln Summary - the POOM writes a plain-text report to
`/poom_netaudit/audit_001.txt`, `002`, ... on the SD card: the network, and
for each device its identity, open ports with services and products,
confirmed / weakness / good-to-know findings and up to ten exploit leads
(marked potential), followed by the devices with nothing to report. The screen
shows the file name when it is saved; nothing is written unless you ask. The date is included only when the
POOM's clock has been set.

**Radio and memory behaviour.** The Exploit-DB lookup needs no network, so Wi-Fi
is switched off for it (this frees the RAM the SD card needs) and stays off;
the next action that needs the network reconnects on its own and carries on.
Leaving the app switches Wi-Fi off and unmounts the SD card (if the scanner
mounted it), so other radio apps start cleanly.

---

## Architecture

- **`modules/poom_net_scanner`** - headless engine:
  - `poom_net_scanner_logic.c` - pure: subnet math, port sets and windows, port
    parser, service table, banner / HTTP parsers, word wrap.
  - `poom_net_scanner_ident.c` - pure: OS / device identification and the
    discovery packet builders/parsers (ICMP, SSDP, NetBIOS, mDNS, UPnP XML).
  - `poom_net_scanner.c` - networked: subnet, ARP discovery, TCP Connect scan,
    service detection, the identify pass.
- **`modules/poom_net_audit`** - offline audit:
  - `poom_net_audit_logic.c` - pure: CSV parsing, whole-word matching, version
    tiers, multi-term collector, network-wide Assess helpers, platform fit.
  - `poom_net_audit_index.c` - the automatic block index (plain stdio).
  - `poom_net_audit_rules.c` - exposure rule engine + built-in rules.
  - `poom_net_audit_checks.c` - read-only web checks + matcher.
  - `poom_net_audit.c` - SD mount / search entry points.
- **`applications/poom_app_pack/src/menu_net_scanner.c`** - the screens.

### Build

See [the pinned release build and memory profile](NET_SCANNER_BUILD.md).

### Tests

The pure parts have host unit tests (no ESP32 needed):

```
cd modules/poom_net_scanner
cc -std=c11 -Wall -Wextra -Iinclude -Itest test/test_logic.c poom_net_scanner_logic.c -o /tmp/t && /tmp/t
cc -std=c11 -Wall -Wextra -Iinclude -Itest test/test_ident.c poom_net_scanner_ident.c poom_net_scanner_logic.c -o /tmp/t && /tmp/t

cd ../poom_net_audit
cc -std=c11 -Wall -Wextra -Iinclude -Itest test/test_logic.c poom_net_audit_logic.c -o /tmp/t && /tmp/t
cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_rules.c poom_net_audit_rules.c -o /tmp/t && /tmp/t
cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_checks.c poom_net_audit_checks.c -o /tmp/t && /tmp/t
cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_index.c poom_net_audit_index.c poom_net_audit_logic.c -o /tmp/t && /tmp/t
```

### Diagnostics

The default build compiles `ESP_LOGx` output out, so the scanner prints a few
`[net_scanner] ...` lines with `printf` to the USB console: heap on enter /
exit, per-host open-port counts, UPnP models found, web-check hits, the saved
report's path, how the Exploit-DB pass ran
(*indexed* / *full scan + index built*) and a one-line summary at the end of a
Full NetAudit.

---

## Known limitations

- Exploit matches are leads only; nothing is verified against the device. Web
  checks confirm exposures, not exploitability.
- No TCP SYN scan and no TLS (HTTPS) service identification - a TLS handshake
  needs more free RAM than is available while Wi-Fi is up.
- OS identification is heuristic (TTL, banners, self-reported models), not
  stack fingerprinting; it shows how sure it is.
- Up to 64 devices; Scan All keeps up to 12 open ports per device, a
  single-device scan up to 32.
- A Full (1-65535) scan of many devices is slow by nature; use Quick or
  Extended for Scan All and cancel with B at any time.
