# poom_host_sniffer_transport

`poom_host_sniffer_transport` provides the shared serial framing used by POOM host-side live sniffers.

This module is transport-only: it does not capture radio traffic itself. Protocol-specific capture stays in modules such as BLE or IEEE 802.15.4 sniffers.

## Structure

```text
modules/poom_host_sniffer_transport
├── CMakeLists.txt
├── README.md
├── poom_host_sniffer_transport.c
└── include/
    └── poom_host_sniffer_transport.h
```

## Public API

Header:
`modules/poom_host_sniffer_transport/include/poom_host_sniffer_transport.h`

The module currently provides helpers for:

* generic framed packet sending,
* BLE GAP scan result export as HCI H4 event payloads.

## Transport Format

Frames use:

* `COBS`
* `CRC16-CCITT`
* trailing `0x00` delimiter
* body layout: `VER | TYPE | CH | FLAGS | DLC | PAYLOAD | RSSI | STATUS`

## Runtime Behavior

Caller modules build protocol payloads, then hand them to this transport layer for:

1. framing,
2. checksum generation,
3. UART emission to a host parser/tool.

## Integration

* `poom_uart_sniffer` uses this transport as a compatibility layer.
* The framing is intended for POOM host sniffing tools and live capture bridges.
