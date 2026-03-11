# poom_http_load_test

`poom_http_load_test` generates HTTP GET load from a POOM device to a network target.

## Purpose

This flow is used to validate connectivity, stability, and server behavior under repeated HTTP requests.

- On a computer, you start a simple HTTP server.
- On POOM, you start `poom_http_load_test`, which connects to Wi-Fi and sends GET requests to that server.
- This lets you observe how the server behaves under continuous traffic from POOM.

## Structure

1. Computer (target server):
- Run a local HTTP server with Python.

2. POOM (load client):
- Connect to Wi-Fi (`ssid` and `password`).
- Send parallel GET requests to the configured host/port/path.
- STA Wi-Fi management uses `poom_wifi_ctrl`.

## Local GET Server

```bash
# Dynamic function
# Run : 
# python3 -m http.server
# fuser -k 8000/tcp
python3 -m http.server 8000
```

To stop the process using port 8000:

```bash
fuser -k 8000/tcp
```

## Public API

Header:
- `include/poom_http_load_test.h`

Functions:
- `poom_http_load_test_start(const poom_http_load_test_config_t *config)`
- `poom_http_load_test_switch_target(const char *host, const char *port, const char *path)`
- `poom_http_load_test_stop()`

## Configuration

`poom_http_load_test_config_t`:
- `ssid`
- `password`
- `host`
- `port`
- `path`
- `worker_count`

## Usage

```c
#include "poom_http_load_test.h"

void app_example(void)
{
    poom_http_load_test_config_t cfg = {
        .ssid = "MyWifi",
        .password = "MyPassword",
        .host = "192.168.1.10",
        .port = "8000",
        .path = "/",
        .worker_count = 8,
    };

    (void)poom_http_load_test_start(&cfg);
}
```
