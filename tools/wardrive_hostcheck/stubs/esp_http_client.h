#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#define ESP_FAIL -1
#define ESP_ERR_HTTP_HEADER_TOO_LONG 0x700a
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define HTTP_AUTH_TYPE_BASIC 1
#define HTTP_METHOD_POST 1
typedef void *esp_http_client_handle_t;
typedef struct {
    const char *url,*username,*password;
    int auth_type,timeout_ms,buffer_size,buffer_size_tx;
    bool disable_auto_redirect;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_write(esp_http_client_handle_t,const char *,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char *,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t,int);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t,int);

int esp_http_client_get_errno(esp_http_client_handle_t);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int *,int *);
