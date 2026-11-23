#ifndef HOST_ESP_ERR_H
#define HOST_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_WIFI_NOT_INIT 1
#define ESP_ERR_WIFI_NOT_STARTED 2
const char *esp_err_to_name(esp_err_t error);
#endif
