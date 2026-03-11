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
void poom_wifi_captive_start(void);
void poom_wifi_captive_stop(void);
void poom_wifi_captive_set_portal_file(const char *filename);
```

## Runtime Behavior

- `poom_wifi_captive_start()`:
  - initializes LED strip,
  - mounts/creates required SD layout,
  - loads STA credentials from `ssid.txt` (or keeps defaults),
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
- Captured data: `CAPTIVE_DATA_PATH`
- STA credentials file: `SSID_DATA_PATH` (`ssid,password` format)

## Logging

Configurable in `poom_wifi_captive.h`:

- `CAPTIVE_MODULE_LOG_ENABLED`
- `CAPTIVE_MODULE_DEBUG_LOG_ENABLED`
