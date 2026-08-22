# poom_wifi_ctrl

`poom_wifi_ctrl` is the shared Wi-Fi control module used across POOM firmware.

It provides a consistent API for Access Point, Station, and mixed runtime flows while centralizing Wi-Fi setup details in one place.

## Structure

```text
modules/poom_wifi_ctrl
├── CMakeLists.txt
├── README.md
├── Kconfig.projbuild
├── poom_wifi_ctrl.c
└── include/
    └── poom_wifi_ctrl.h
```

## Dependencies

Declared in `modules/poom_wifi_ctrl/CMakeLists.txt`:

* `esp_wifi`
* `esp_event`
* `esp_netif`

Optional integration is enabled through Kconfig, such as mDNS support.

## Public API

Header:
`modules/poom_wifi_ctrl/include/poom_wifi_ctrl.h`

The module exposes helpers to:

* initialize Wi-Fi in AP mode,
* initialize Wi-Fi in STA mode,
* initialize AP + STA mode,
* disable Wi-Fi / return to NULL mode,
* set channel,
* set and restore MAC addresses,
* manage Wi-Fi event callbacks used by higher-level apps.

## Runtime Behavior

This module is the common Wi-Fi foundation for features such as:

* captive portals,
* OTA / DFU,
* Wi-Fi scanners,
* sniffers,
* Karma-like and attack-oriented apps.

By centralizing mode changes here, apps avoid duplicating Wi-Fi bring-up and teardown logic.

## Configuration

Menuconfig section:

* `POOM Wi-Fi Controller`

Notable options include:

* manager AP SSID/password/channel,
* AP max connections,
* AP auth enable/disable,
* scan result limits,
* mDNS enable,
* logging enable.

## Integration

* Add `poom_wifi_ctrl` to `REQUIRES` in any module that changes Wi-Fi mode.
* Prefer using this module instead of calling scattered `esp_wifi_*` init flows directly from apps.
