# dns_server

`dns_server` is a lightweight UDP DNS responder for IPv4 A queries.

## Purpose

- Listen on UDP port `53`.
- Parse DNS questions from incoming packets.
- Resolve response IPs from configured rules (`name -> netif` or `name -> static ip`).
- Return DNS answers only when a matching rule is found.

## Structure

```text
components/dns_server
├── CMakeLists.txt
├── include/
│   └── dns_server.h
└── src/
    └── dns_server.c
```

## Dependencies

Defined in `components/dns_server/CMakeLists.txt`:

- `esp_netif`

## Public API

Header: `components/dns_server/include/dns_server.h`

```c
typedef struct dns_entry_pair {
    const char *name;
    const char *if_key;
    esp_ip4_addr_t ip;
} dns_entry_pair_t;

typedef struct dns_server_config {
    int num_of_entries;
    dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS];
} dns_server_config_t;

typedef struct dns_server_handle *dns_server_handle_t;

dns_server_handle_t start_dns_server(dns_server_config_t *config);
void stop_dns_server(dns_server_handle_t handle);
```

Helper macro:

```c
#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key) ...
```

## Runtime Behavior

- `start_dns_server()`:
  - validates config,
  - allocates runtime handle,
  - starts a FreeRTOS task that owns the UDP socket.
- DNS task:
  - receives packet,
  - parses all questions,
  - creates answers for matching A records,
  - sends response packet.
- `stop_dns_server()`:
  - sets stop flag,
  - closes socket,
  - stops task,
  - releases memory.

## Logging

Configurable in `dns_server.h`:

- `DNS_SERVER_LOG_ENABLED`
- `DNS_SERVER_DEBUG_LOG_ENABLED`

## Notes

- Name compression pointers inside `QNAME` are not supported.
- Only IPv4 A records are answered.
- If no rule matches, no DNS answer section is generated.
