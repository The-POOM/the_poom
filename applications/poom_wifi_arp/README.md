# poom_wifi_arp

`poom_wifi_arp` is a small Wi-Fi STA LAN discovery helper that:
- Discovers active hosts on the current `/24` subnet using ARP requests (STA must have an IP).
- Probes a selected host for common TCP ports, UDP responding ports, and (best-effort) SSH banner detection.

It is designed to be called from UI/menu code (for example the Beast scan menu).

## Purpose

- ARP-based host discovery (IPv4 + MAC).
- TCP connect probe for a fixed set of common ports.
- UDP response probe for a fixed set of common ports.
- Optional SSH banner probe on common SSH ports.
- No persistent background tasks: callers can run scans in their own task.

## Structure

```text
applications/poom_wifi_arp
├── CMakeLists.txt
├── component.mk
├── README.md
├── include/
│   └── poom_wifi_arp.h
└── poom_wifi_arp.c
```

## Public API

Header: `applications/poom_wifi_arp/include/poom_wifi_arp.h`

- `poom_wifi_arp_scan_subnet`
- `poom_wifi_arp_probe_tcp_port`
- `poom_wifi_arp_probe_udp_port`
- `poom_wifi_arp_probe_ssh_port`
- `poom_wifi_arp_probe_host_services`

## Dependencies

Defined in `applications/poom_wifi_arp/CMakeLists.txt`:

- `poom_wifi_ctrl`
- `esp_netif`
- `lwip`

## Integration

- `CONFIG_POOM_WIFI_ARP_ENABLE_LOG`: enables debug prints via `POOM_PRINTF_*` macros.

## Usage

### Subnet discovery (ARP)

```c
#include "poom_wifi_arp.h"

static poom_wifi_arp_scan_host_t s_hosts[64];

void run_scan(void)
{
    poom_wifi_arp_scan_ctx_t ctx = {
        .hosts = s_hosts,
        .num_active_hosts = 0,
        .max_hosts = 64,
        .subnet_prefix = {0},
    };

    (void)poom_wifi_arp_scan_subnet(&ctx);
}
```

### Host probing (TCP/UDP/SSH)

```c
#include "poom_wifi_arp.h"

void probe_one_host(const char *ip)
{
    poom_wifi_arp_service_probe_result_t r;
    (void)poom_wifi_arp_probe_host_services(ip, &r);
    /* r.tcp.open_ports / r.udp.open_ports / r.ssh_* */
}
```

## Runtime Behavior

```mermaid
flowchart TD
    A[Menu/UI task] --> B{WiFi STA has IPv4?}
    B -- No --> C[Connect STA + DHCP]
    C --> B
    B -- Yes --> D[poom_wifi_arp_scan_subnet]
    D --> E[ARP requests over STA netif]
    E --> F[Read lwIP ARP table]
    F --> G[Build host list: IP + MAC]
    G --> H[User selects one host]
    H --> I[poom_wifi_arp_probe_host_services]
    I --> J[TCP connect probe common ports]
    I --> K[UDP response probe common ports]
    I --> L[Optional SSH banner probe]
    J --> M[Return combined result]
    K --> M
    L --> M

    G --> N[User enters TCP port]
    N --> O[Probe by mode (TCP/UDP/SSH)]
    O --> Q[poom_wifi_arp_probe_tcp_port / poom_wifi_arp_probe_udp_port / poom_wifi_arp_probe_ssh_port]
    O --> P[Return hit list]

```

## Integration

- ARP discovery requires the STA interface to have an IPv4 address.
- UDP probing is response-based and may miss open-but-silent UDP ports.
