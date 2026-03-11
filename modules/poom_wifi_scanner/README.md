# poom_wifi_scanner

`poom_wifi_scanner` performs synchronous Wi-Fi scans and exposes cached AP records.

## Purpose

- Initialize STA mode via `poom_wifi_ctrl`.
- Run a blocking scan with `esp_wifi_scan_start`.
- Store and expose AP records for other modules (`wifi_deauth`, menu flows).

## Structure

```text
components/poom_wifi_scanner
├── CMakeLists.txt
├── README.md
├── include/
│   └── poom_wifi_scanner.h
└── poom_wifi_scanner.c
```

## Public API

Header: `components/poom_wifi_scanner/include/poom_wifi_scanner.h`

```c
esp_err_t poom_wifi_scanner_scan(void);
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void);
wifi_ap_record_t *poom_wifi_scanner_get_ap_record(unsigned index);
esp_err_t poom_wifi_scanner_clear_ap_records(void);
```

## Dependencies

- `esp_wifi`
- `poom_wifi_ctrl`

## Notes

- Scan results are cached in a static module buffer.
- Maximum AP records follows `CONFIG_SCAN_MAX_AP` (`POOM_WIFI_SCANNER_MAX_AP`).
