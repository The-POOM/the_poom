# open_thread

`open_thread` is a POOM wrapper around ESP-IDF OpenThread bring-up plus a small set of UDP and TCP helper utilities.

It centralizes common Thread-network setup so higher-level code can initialize a dataset, inspect addresses, and open simple socket-style endpoints without duplicating OpenThread platform glue.

## Structure

```text
modules/open_thread
├── CMakeLists.txt
├── README.md
├── open_thread.c
└── include/
    ├── open_thread.h
    └── open_thread_config.h
```

## Dependencies

Declared in `modules/open_thread/CMakeLists.txt`:

* `openthread`

Private requirements:

* `esp_event`
* `esp_netif`
* `nvs_flash`

## Public API

Header:
`modules/open_thread/include/open_thread.h`

Key helpers include:

* `poom_ot_init()`
* `poom_ot_deinit()`
* `poom_ot_set_dataset()`
* `poom_ot_set_channel()`
* `poom_ot_factory_reset()`
* `poom_ot_get_my_ipv6address()`
* `poom_ot_ipmaddr_subscribe()`
* `poom_ot_ipmaddr_unsubscribe()`
* `poom_ot_enable_promiscuous_mode()`
* `poom_ot_disable_promiscuous_mode()`
* `poom_ot_udp_open()`
* `poom_ot_udp_bind()`
* `poom_ot_udp_close()`
* `poom_ot_udp_send()`
* `poom_ot_tcp_send()`
* `poom_ot_tcp_client_open()`
* `poom_ot_tcp_client_close()`
* `poom_ot_tcp_client_send()`
* `poom_ot_tcp_server_start()`
* `poom_ot_tcp_server_stop()`

## Runtime Behavior

When initialized, the module:

1. sets up the ESP-IDF OpenThread platform,
2. creates the OpenThread network interface glue,
3. starts the OpenThread main loop task,
4. lets callers apply an operational dataset and enable IPv6 + Thread.

The networking helpers then build on that initialized Thread instance for:

* UDP sockets,
* one-shot TCP send,
* persistent TCP client/server helpers,
* optional promiscuous capture callback registration.

## Integration

* This module uses the `poom_ot_*` prefix even though the folder is still named `open_thread`.
* It is currently an infrastructure/bring-up helper rather than a polished end-user POOM app module.
