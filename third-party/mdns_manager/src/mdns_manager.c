// mdns_manager.c
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_manager";

esp_err_t setup_mdns(void) {
    esp_err_t err;

    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set("poom");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("POOM Portal");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS initialized: http://poom.local");
    return ESP_OK;
}

void teardown_mdns(void) {
    mdns_free();
}