# poom_wifi_captive

`poom_wifi_captive` runs a captive portal stack with AP+STA Wi-Fi, HTTP server and DNS redirection.

## Purpose

- Start a cloned AP from configured STA credentials.
- Serve captive portal pages (from SD if available, fallback to embedded HTML).
- Capture query parameters (`user1..user4`) and store them on SD.
- Redirect DNS A queries to the AP interface for captive portal behavior.

## Structure

```text
applications/poom_wifi_captive
├── CMakeLists.txt
├── component.mk
├── poom_wifi_captive.c
├── root.html
├── redirect.html
├── include/
│   └── poom_wifi_captive.h
└── README.md
```

## Dependencies

Defined in `applications/poom_wifi_captive/CMakeLists.txt`:

- `dns_server`
- `poom_wifi_scanner`
- `esp_http_server`
- `board`
- `esp_http_client`
- `sbus`
- `sd_card`
- `ws2812`
- `poom_wifi_ctrl`

## Public API

Header: `applications/poom_wifi_captive/include/poom_wifi_captive.h`

```c
esp_err_t poom_wifi_captive_start(void);
void poom_wifi_captive_stop(void);
void poom_wifi_captive_set_portal_file(const char *filename);
```

## Runtime Behavior

- `poom_wifi_captive_start()`:
  - initializes LED strip,
  - mounts/creates required SD layout,
  - loads STA credentials from `ssid.txt` (or keeps defaults),
  - keeps the portal file/path selected before start,
  - initializes AP+STA Wi-Fi via `poom_wifi_ctrl`,
  - starts HTTP server and DNS server.
- HTTP handlers:
  - `/` serves selected portal file or embedded fallback,
  - `/validate` captures URL params and appends to SD,
  - `/redirect` serves redirect page,
  - unknown routes return redirect to `/`.
- `poom_wifi_captive_stop()`:
  - stops DNS and HTTP,
  - unregisters `poom_wifi_ctrl` callback and deinitializes Wi-Fi,
  - frees user context and LED resources.

## SD Layout

- Portal folder: `CAPTIVE_PORTALS_FOLDER_PATH` (`/portals` under SD root)
- Captive menu preferences: `AP Name` and `Portal` are stored in NVS via `poom_secrets_store`
- Captured data: `CAPTIVE_DATA_PATH`
- STA credentials file: `SSID_DATA_PATH` (`ssid,password` format)

## Logging

Configurable in `poom_wifi_captive.h`:

- `CAPTIVE_MODULE_LOG_ENABLED`
- `CAPTIVE_MODULE_DEBUG_LOG_ENABLED`

## OLED client monitor

The Captive status screen offers `Clients` and `Stop`. In the client list,
Up/Down selects a row, A opens its details, and B returns without stopping
the portal. B on the status screen exits and stops the portal. Four clients
can associate at once. The monitor refreshes once per second without scanning
other networks.

Labels prefer the DHCP hostname, then a User-Agent platform hint (iPhone,
iPad, iPod, Android, ChromeOS, Windows, Mac, Linux), then the assigned IP.
A client without a known IP is shown as `Pending IP`. DHCP names require
`CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME=y` (enabled in the current build).
Names are limited to 63 display characters; long labels scroll using the
Wi-Fi Scan timing. Device types are hints, not verified hardware identities.
Devices using a static IP may have no known address/name until information
is available; DHCP leases are not used as proof of an online connection.

Details show the name, IP, complete MAC, RSSI, platform and online status.
If the selected client leaves, its open detail remains marked OFFLINE and
RSSI becomes unavailable. A refresh failure shows UNKNOWN rather than a
false disconnect. No hostname history, request log or User-Agent is retained.

`poom_wifi_captive_start()` now returns an error on failed startup. The menu
only shows RUNNING after successful startup. `poom_wifi_captive_get_clients()`
returns a bounded copy ordered by MAC, preserving selection during updates.

Inside Beast, `CAPTIVE PORTAL` opens the captive app menu. `Start` uses the
saved `AP Name` and `Portal`; `Settings` edits the AP name or opens the SD
browser at `/sdcard/portals` with an `.html/.htm` filter; `Scan SSID` copies
a scanned SSID into `AP Name` and returns to settings. Client monitoring is
shown only in the running status screen, where `Clients` opens the list and
`Stop` stops the portal.

`Scan SSID` runs in a separate worker, with four visible rows, scrolling
labels and continuation arrows. B cancels the scan; navigation resumes once
the scanner has released its resources. The configured AP result limit
remains unchanged (20 in the current build).

Hardware checks: connect a DHCP client with a hostname and one without; open
the portal with different User-Agents; connect four clients; unplug one while
its detail is open; reconnect and check selection and IP changes; verify long
SSID/name scrolling and repeated scan cancellation, entry, stop and exit.

The OLED state and client table are allocated only while the menu/portal is
active, preferring PSRAM and falling back to the normal heap. Closing the menu
frees its context and event queue; stopping the portal unregisters its event
handlers and frees the client table. Partial startup failures use the same
cleanup path. Refresh reuses the existing display snapshot instead of placing
a second client table on the task stack.
