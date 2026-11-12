#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#define ESP_FAIL -1
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN -1
#define HTTP_GET 0
#define HTTP_POST 1
typedef void *httpd_handle_t;
typedef struct { int content_len; const char *cookie,*query,*body; size_t body_read; int status; char response[8192]; size_t sent; } httpd_req_t;
typedef struct { int max_uri_handlers,max_open_sockets,stack_size,recv_wait_timeout,send_wait_timeout; bool lru_purge_enable; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
esp_err_t httpd_start(httpd_handle_t *,const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *,const char *,char *,size_t);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *,char *,size_t);
esp_err_t httpd_query_key_value(const char *,const char *,char *,size_t);
int httpd_req_recv(httpd_req_t *,char *,size_t);
esp_err_t httpd_resp_send_err(httpd_req_t *,int,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_send(httpd_req_t *,const char *,int);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *,const char *);
esp_err_t httpd_resp_send_chunk(httpd_req_t *,const char *,size_t);
