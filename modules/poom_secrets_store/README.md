# poom_secrets_store

`poom_secrets_store` is a lightweight NVS-based storage component for sensitive runtime configuration such as:

- Wi-Fi SSID
- Wi-Fi password
- API tokens
- Additional app-specific keys (`mqtt_host`, `user_id`, etc.)

It stores values in the NVS namespace `poom_sec`.

## Features

- Generic key-value API for:
  - `string`
  - `uint32_t`
  - `blob`
- Dynamic record API by `id` string:
  - store/read any structure as blob
  - check if record exists by ID
  - erase record by ID
- Convenience helpers for common keys:
  - `wifi_ssid`
  - `wifi_pass`
  - `api_token`
- Key erase and full namespace clear.

## Public API

- `esp_err_t poom_secrets_init(void);`
- `esp_err_t poom_secrets_set_str(const char* key, const char* value);`
- `esp_err_t poom_secrets_get_str(const char* key, char* out_value, size_t* inout_len);`
- `esp_err_t poom_secrets_set_u32(const char* key, uint32_t value);`
- `esp_err_t poom_secrets_get_u32(const char* key, uint32_t* out_value);`
- `esp_err_t poom_secrets_set_blob(const char* key, const void* data, size_t data_len);`
- `esp_err_t poom_secrets_get_blob(const char* key, void* out_data, size_t* inout_len);`
- `esp_err_t poom_secrets_erase_key(const char* key);`
- `esp_err_t poom_secrets_clear(void);`
- `esp_err_t poom_secrets_key_exists(const char* key, bool* out_exists);`
- `esp_err_t poom_secrets_set_record_blob(const char* id, const void* data, size_t data_len);`
- `esp_err_t poom_secrets_get_record_blob(const char* id, void* out_data, size_t* inout_len);`
- `esp_err_t poom_secrets_get_record_size(const char* id, size_t* out_len);`
- `esp_err_t poom_secrets_get_record_blob_alloc(const char* id, void** out_data, size_t* out_len);`
- `esp_err_t poom_secrets_record_exists(const char* id, bool* out_exists);`
- `esp_err_t poom_secrets_erase_record(const char* id);`

## Usage

```c
#include "poom_secrets_store.h"

void app_store_credentials(void) {
    (void)poom_secrets_init();

    (void)poom_secrets_set_wifi_ssid("MyAP");
    (void)poom_secrets_set_wifi_pass("MyPassword");
    (void)poom_secrets_set_api_token("token_123");

    (void)poom_secrets_set_str("mqtt_host", "broker.local");
    (void)poom_secrets_set_u32("retry_count", 3);
}
```

Read values:

```c
#include <stdlib.h>
#include "poom_secrets_store.h"

void app_read_credentials(void) {
    size_t ssid_len = 0;
    if(poom_secrets_get_wifi_ssid(NULL, &ssid_len) == ESP_OK) {
        char* ssid = (char*)malloc(ssid_len);
        if((ssid != NULL) && (poom_secrets_get_wifi_ssid(ssid, &ssid_len) == ESP_OK)) {
            // use ssid
        }
        free(ssid);
    }
}
```

## Security Notes

- This component uses NVS as backend.
- For encrypted-at-rest secrets, enable NVS encryption in project configuration and use a partition table that includes NVS keys.
- If NVS encryption is not enabled, values are stored in plain form in flash.

## Dynamic Record Example

```c
#include <stdbool.h>
#include <stdlib.h>
#include "poom_secrets_store.h"

typedef struct {
    uint32_t version;
    uint32_t flags;
    char token[48];
} app_secret_t;

void app_dynamic_record_example(void) {
    app_secret_t in_secret = {
        .version = 1U,
        .flags = 0x11U,
        .token = "my_runtime_token",
    };
    bool exists = false;
    size_t out_len = sizeof(app_secret_t);
    app_secret_t out_secret;

    (void)poom_secrets_init();
    (void)poom_secrets_set_record_blob("profile_main", &in_secret, sizeof(in_secret));

    (void)poom_secrets_record_exists("profile_main", &exists);
    if(exists) {
        if(poom_secrets_get_record_blob("profile_main", &out_secret, &out_len) == ESP_OK) {
            // use out_secret
        }
    }
}
```

Load unknown-size record dynamically:

```c
#include <stdlib.h>
#include "poom_secrets_store.h"

void app_load_dynamic_record(void) {
    const char* id = "example_profile_01";
    bool exists = false;
    void* blob = NULL;
    size_t blob_len = 0U;

    (void)poom_secrets_record_exists(id, &exists);
    if(!exists) {
        return;
    }

    if(poom_secrets_get_record_blob_alloc(id, &blob, &blob_len) == ESP_OK) {
        // Parse blob according to your own struct/version format.
        free(blob);
    }
}
```
