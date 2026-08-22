# poom_ieee802154_sniffer

`poom_ieee802154_sniffer` provides fixed-channel IEEE 802.15.4 sniffing with optional UART forwarding through the shared host sniffer transport.

It is intended for live Zigbee / 802.15.4 capture workflows on supported ESP32 targets.

## Structure

```text
modules/poom_ieee802154_sniffer
├── CMakeLists.txt
├── README.md
├── poom_ieee802154_sniffer.c
└── include/
    └── poom_ieee802154_sniffer.h
```

## Dependencies

Declared in `modules/poom_ieee802154_sniffer/CMakeLists.txt`:

* `poom_scanner_core`
* `poom_uart_sniffer`

## Public API

Header:
`modules/poom_ieee802154_sniffer/include/poom_ieee802154_sniffer.h`

```c
void poom_ieee802154_sniffer_set_uart_forward_enabled(bool enabled);
esp_err_t poom_ieee802154_sniffer_start(uint8_t channel);
esp_err_t poom_ieee802154_sniffer_stop(void);
bool poom_ieee802154_sniffer_is_active(void);
uint8_t poom_ieee802154_sniffer_get_channel(void);
int8_t poom_ieee802154_sniffer_get_recent_rssi(void);
uint32_t poom_ieee802154_sniffer_get_packet_count(void);
```

## Runtime Behavior

When started, the module:

1. validates the fixed channel,
2. enables IEEE 802.15.4 radio receive mode,
3. registers an ISR consumer through `poom_scanner_core`,
4. copies captured frames into a queue,
5. processes them in a FreeRTOS task,
6. optionally forwards them over UART with channel and RSSI metadata.

The forwarded payload strips the hardware-replaced FCS bytes so the host receives NOFCS-style frame payloads.

## Integration

* Intended for ESP32-C5 / ESP32-C6 class targets with 802.15.4 support.
* Useful as a bridge between low-level radio capture and host-side sniffing tools.
