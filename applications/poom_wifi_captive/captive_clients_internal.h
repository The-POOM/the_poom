// SPDX-License-Identifier: MIT
#ifndef CAPTIVE_CLIENTS_INTERNAL_H
#define CAPTIVE_CLIENTS_INTERNAL_H

#include "esp_http_server.h"

esp_err_t captive_clients_start(void);
void captive_clients_stop(void);
void captive_clients_observe_http(httpd_req_t *req);

#endif
