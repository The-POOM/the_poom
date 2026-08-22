# poom_espnow

`poom_espnow` is a lightweight ESP-NOW helper module built on top of `poom_wifi_ctrl`.

It wraps Wi-Fi startup, ESP-NOW init, peer management, RX queueing, and callback delivery so callers can focus on payload handling.

## Structure

```text
modules/poom_espnow
├── CMakeLists.txt
├── README.md
├── component.mk
├── poom_espnow.c
└── include/
    └── poom_espnow.h
```

## Dependencies

Declared in `modules/poom_espnow/CMakeLists.txt`:

* `poom_wifi_ctrl`
* `esp_wifi`

## Public API

Header:
`modules/poom_espnow/include/poom_espnow.h`

Key helpers include:

* `poom_espnow_config_default()`
* `poom_espnow_is_running()`
* `poom_espnow_start()`
* `poom_espnow_stop()`
* `poom_espnow_register_rx_cb()`
* `poom_espnow_set_channel()`
* `poom_espnow_peer_add()`
* `poom_espnow_peer_del()`
* `poom_espnow_send()`

## Runtime Behavior

When started, the module:

1. initializes Wi-Fi in STA mode through `poom_wifi_ctrl`,
2. disables power save for more predictable timing,
3. starts ESP-NOW,
4. optionally adds the broadcast peer,
5. receives frames into an internal queue,
6. dispatches RX callbacks from a task context.

## Integration

* RX callbacks are not invoked directly from the low-level ISR/callback path; they are delivered from the module task.
* The module is intended for POOM features that need quick ESPNOW bring-up without repeating Wi-Fi boilerplate.
