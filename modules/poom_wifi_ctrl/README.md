# poom_wifi_ctrl

`poom_wifi_ctrl` is a lightweight Wi-Fi management component built on top of ESP-IDF.

It provides a clean and consistent interface to control Wi-Fi modes inside the POOM firmware while keeping the implementation:

* Modular
* Idempotent
* MISRA-conscious
* Explicit in error handling

---

## Overview

This component allows POOM firmware to:

* Start Wi-Fi in **Access Point (AP)** mode
* Start Wi-Fi in **Station (STA)** mode
* Start Wi-Fi in **AP + STA (dual mode)**
* Disable Wi-Fi (NULL mode)
* Manage AP configuration through Kconfig
* Set and restore MAC addresses
* Set Wi-Fi channel safely

---

## Structure

```
components/poom_wifi_ctrl/
├── poom_wifi_ctrl.c
├── poom_wifi_ctrl.h
├── Kconfig
└── README.md
```

---

## Configuration (menuconfig)

Navigate to:

```
POOM Wi-Fi Controller
```

### Scan Settings

| Option                     | Description                                           |
| -------------------------- | ----------------------------------------------------- |
| POOM_WIFI_CTRL_SCAN_MAX_AP | Maximum number of AP entries stored during Wi-Fi scan |

Increasing this value increases RAM usage.

---

### Manager Access Point Settings

| Option                                    | Description                           |
| ----------------------------------------- | ------------------------------------- |
| POOM_WIFI_CTRL_MANAGER_AP_SSID            | SSID of the management AP             |
| POOM_WIFI_CTRL_MANAGER_AP_PASSWORD        | WPA2 password                         |
| POOM_WIFI_CTRL_MANAGER_AP_CHANNEL         | Channel (1–13)                        |
| POOM_WIFI_CTRL_MANAGER_AP_MAX_CONNECTIONS | Maximum connected clients             |
| POOM_WIFI_CTRL_MANAGER_AP_AUTH_ENABLE     | Enable or disable WPA2 authentication |

If authentication is disabled, the AP will be open (no password required).

---

### Optional Features

| Option                        | Description |
| ----------------------------- | ----------- |
| POOM_WIFI_CTRL_ENABLE_MDNS   | Enable mDNS integration via `mdns_manager` |
| POOM_WIFI_CTRL_ENABLE_LOG    | Enable internal printf debug logs |

If mDNS is enabled, the `mdns_manager` component must be present in the project.

Disabling logs reduces binary size and runtime overhead.

---

### Notes

- Higher `SCAN_MAX_AP` values increase RAM usage.
- Increasing `MAX_CONNECTIONS` increases Wi-Fi resource usage.
- mDNS adds additional memory and background processing.
---

## Public API

### Start Manager AP (Using Kconfig Defaults)

```c
poom_wifi_ctrl_manager_ap_start(NULL);
```

---

### Start Custom AP

```c
wifi_config_t cfg;

poom_wifi_ctrl_manager_ap_config_default(&cfg);
poom_wifi_ctrl_ap_start(&cfg);
```

---

### Initialize Station Mode

```c
poom_wifi_ctrl_init_sta();
```

---

### Initialize AP + STA Mode

```c
poom_wifi_ctrl_init_apsta();
```

---

### Disable Wi-Fi

```c
poom_wifi_ctrl_init_null();
```

---

### Stop Access Point

```c
poom_wifi_ctrl_ap_stop();
```

---

### Deinitialize Wi-Fi

```c
poom_wifi_ctrl_deinit();
```

---

### MAC Address Management

```c
uint8_t mac[6];

poom_wifi_ctrl_get_ap_mac(mac);
poom_wifi_ctrl_set_ap_mac(custom_mac);
poom_wifi_ctrl_restore_ap_mac();

poom_wifi_ctrl_get_sta_mac(mac);
```

---

### Set Wi-Fi Channel

```c
poom_wifi_ctrl_set_channel(6);
```

Valid range: **1–13**

---

## Design Principles

* No internal `ESP_ERROR_CHECK()` calls
* All public APIs return `esp_err_t`
* Safe repeated initialization (idempotent behavior)
* No magic numbers
* Static internal state encapsulation
* Bounded string operations

---

## MISRA-Oriented Practices

This component follows structured practices aligned with MISRA recommendations:

* Explicit return codes
* Defensive argument checking
* No uncontrolled global exposure
* Compile-time constants for limits
* Controlled initialization flow

---

## Security Notes

* Avoid hard-coded credentials in production builds
* Prefer secure provisioning mechanisms
* Limit maximum AP connections when possible
* Disable open AP mode unless explicitly required

---

## Integration Notes

* Requires ESP-IDF Wi-Fi (`esp_wifi`)
* Requires `esp_netif` and event loop
* Requires `nvs_flash`
* Optional mDNS integration (compile-time controlled)

---

## Usage

This section shows minimal examples for:

- Access Point (AP) mode
- Station (STA) mode with event callback

---

### Example 1 — Start Manager AP

This example starts the default POOM Manager AP using Kconfig parameters.

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "poom_wifi_ctrl.h"

void app_main(void)
{
    esp_err_t err;

    printf("Starting POOM Manager AP...\n");

    err = poom_wifi_ctrl_manager_ap_start(NULL);
    if (err != ESP_OK)
    {
        printf("AP start failed: %s\n", esp_err_to_name(err));
        return;
    }

    printf("AP running.\n");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("AP alive...\n");
    }
}
```
### Example 2 — Connect in STA Mode (with Callback)
This example connects to an external Wi-Fi network and prints connection events.
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "poom_wifi_ctrl.h"
#include "esp_netif_ip_addr.h"

static void wifi_cb(const poom_wifi_ctrl_evt_info_t *info, void *ctx)
{
    (void)ctx;

    if (info == NULL) { return; }

    if (info->evt == POOM_WIFI_CTRL_EVT_STA_CONNECTED)
    {
        printf("STA connected (link)\n");
    }
    else if (info->evt == POOM_WIFI_CTRL_EVT_STA_DISCONNECTED)
    {
        printf("STA disconnected, reason=%ld\n", (long)info->reason);
    }
    else if (info->evt == POOM_WIFI_CTRL_EVT_STA_GOT_IP)
    {
        printf("Got IP: " IPSTR "\n", IP2STR(&info->ip));
    }
}

void app_main(void)
{
    esp_err_t err;

    printf("Initializing WiFi STA...\n");

    err = poom_wifi_ctrl_init_sta();
    if (err != ESP_OK)
    {
        printf("init_sta failed: %s\n", esp_err_to_name(err));
        return;
    }

    err = poom_wifi_ctrl_register_cb(wifi_cb, NULL);
    if (err != ESP_OK)
    {
        printf("register_cb failed: %s\n", esp_err_to_name(err));
        return;
    }

    printf("Connecting to WiFi...\n");

    err = poom_wifi_ctrl_sta_connect("SDDI_ROUTER", "12345678");
    if (err != ESP_OK)
    {
        printf("sta_connect failed: %s\n", esp_err_to_name(err));
        return;
    }

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```