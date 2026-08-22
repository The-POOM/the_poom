# poom_wifi_deauth_detector

## Purpose

`poom_wifi_deauth_detector` provides passive Wi-Fi management-frame monitoring focused on deauthentication/disassociation detection.

## Responsibilities

- Enable promiscuous capture through `poom_wifi_ctrl`.
- Detect deauth/disassoc management frames.
- Accumulate totals and per-channel counters.
- Expose start/stop/status APIs for menu/UI modules.

## Runtime Behavior

- Passive detector (no frame injection, no active attacks).
- Channel-hopping monitor across 2.4 GHz channels.
- Runtime counters for deauth and disassoc frames.
- Console stats dump helper.

## Public API

Header: `applications/poom_wifi_deauth_detector/include/poom_wifi_deauth_detector.h`

- `esp_err_t poom_wifi_deauth_detector_start(void)`
- `esp_err_t poom_wifi_deauth_detector_stop(void)`
- `bool poom_wifi_deauth_detector_is_running(void)`
- `esp_err_t poom_wifi_deauth_detector_get_stats(poom_wifi_deauth_detector_stats_t *out_stats)`
- `esp_err_t poom_wifi_deauth_detector_reset_stats(void)`
- `esp_err_t poom_wifi_deauth_detector_print_stats(void)`

## Structure

```text
applications/poom_wifi_deauth_detector
├── CMakeLists.txt
├── component.mk
├── README.md
├── include/
│   └── poom_wifi_deauth_detector.h
└── poom_wifi_deauth_detector.c
```

## Integration

- Add `poom_wifi_deauth_detector` in `REQUIRES` where this API is used.
- Depends on `poom_wifi_ctrl` and `esp_wifi`.
- Intended for monitoring and diagnostics.

## Usage

- `CONFIG_POOM_WIFI_DEAUTH_DETECTOR_ENABLE_LOG`
  Enables POOM log macros in this module.

## Runtime Behavior

- Uses POOM log format with tag `poom_wifi_deauth_detector`.
- Logs detector start/stop and runtime warnings.
