# poom_wifi_mac

`poom_wifi_mac` is a helper module built on top of `poom_wifi_ctrl` for repeated STA reconnects with randomized MAC addresses.

It is useful for workflows that need to rotate the station MAC, reconnect, and observe the assigned IP each time.

## Structure

```text
modules/poom_wifi_mac
├── CMakeLists.txt
├── README.md
├── poom_wifi_mac.c
└── include/
    └── poom_wifi_mac.h
```

## Dependencies

Declared in `modules/poom_wifi_mac/CMakeLists.txt`:

* `poom_wifi_ctrl`

## Public API

Header:
`modules/poom_wifi_mac/include/poom_wifi_mac.h`

Main helpers:

* `poom_wifi_mac_start()`
* `poom_wifi_mac_stop()`

## Runtime Behavior

When started, the module:

1. generates a locally administered unicast STA MAC,
2. applies it through `poom_wifi_ctrl`,
3. connects to the target router,
4. waits for `STA_GOT_IP`,
5. reports the assigned IP through an optional callback,
6. repeats as directed by the caller flow.

When stopped, the original STA MAC is restored.

## Integration

* The IP callback is optional.
* This module is best treated as a focused Wi-Fi utility rather than a general scanner or attack component.
