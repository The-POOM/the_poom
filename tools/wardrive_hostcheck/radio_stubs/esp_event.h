#pragma once
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define WIFI_EVENT "wifi"
#define WIFI_EVENT_SCAN_DONE 1
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int,void (*)(void *,esp_event_base_t,int,void *),void *,esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int,esp_event_handler_instance_t);
