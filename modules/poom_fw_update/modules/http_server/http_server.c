/*
 * http_server.c
 *
 *  Created on: 17-Jul-2023
 *      Author: xpress_embedo
 */
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "sys/param.h"

#include "http_server.h"

#include "dfu.h"
#include "poom_dfu_log.h"

// Macros
#define HTTP_SERVER_MAX_URI_HANDLERS     (20u)
#define HTTP_SERVER_RECEIVE_WAIT_TIMEOUT (10u)  // in seconds
#define HTTP_SERVER_SEND_WAIT_TIMEOUT    (10u)  // in seconds
#define HTTP_SERVER_MONITOR_QUEUE_LEN    (3u)

// Private Variables
#if CONFIG_POOM_DFU_ENABLE_LOG
static const char TAG[] = "http_server";
#endif
static void http_server_fw_update_reset_cb(void* arg);
// HTTP Server Task Handle
static httpd_handle_t http_server_handle = NULL;
// HTTP Server Monitor Task Handler
static TaskHandle_t task_http_server_monitor = NULL;
// Queue Handle used to manipulate the main queue of events
static QueueHandle_t http_server_monitor_q_handle = NULL;
// Firmware Update Status
static int fw_update_status = OTA_UPDATE_PENDING;
// ESP32 Timer Configuration Passed to esp_timer_create
static const esp_timer_create_args_t fw_update_reset_args = {
    .callback = &http_server_fw_update_reset_cb,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "fw_update_reset"};
esp_timer_handle_t fw_update_reset = NULL;

// Embedded Files: JQuery, index.html, app.css, app.js, and favicon.ico files
extern const uint8_t jquery_3_3_1_min_js_start[] asm(
    "_binary_jquery_3_3_1_min_js_start");
extern const uint8_t jquery_3_3_1_min_js_end[] asm(
    "_binary_jquery_3_3_1_min_js_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

// Private Function Prototypes
static void http_server_monitor(void* pvParameter);
static httpd_handle_t http_server_configure(void);
static esp_err_t http_server_j_query_handler(httpd_req_t* req);
static esp_err_t http_server_index_html_handler(httpd_req_t* req);
static esp_err_t http_server_app_css_handler(httpd_req_t* req);
static esp_err_t http_server_app_js_handler(httpd_req_t* req);
static esp_err_t http_server_favicon_handler(httpd_req_t* req);
static esp_err_t http_server_ota_update_handler(httpd_req_t* req);
static esp_err_t http_server_ota_status_handler(httpd_req_t* req);
static BaseType_t http_server_monitor_send_msg(http_server_msg_e msg_id);
static void http_server_fw_update_reset_cb(void* arg);
static void http_server_fw_update_reset_timer(void);

// Public Function Definition
/*
 * Starts the HTTP Server
 */
esp_err_t http_server_start(void) {
  if (http_server_handle == NULL) {
    http_server_handle = http_server_configure();
  }
  return (http_server_handle != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t http_server_stop(void) {
  esp_err_t err = ESP_OK;

  if (fw_update_reset != NULL) {
    (void)esp_timer_stop(fw_update_reset);
    (void)esp_timer_delete(fw_update_reset);
    fw_update_reset = NULL;
  }

  if (http_server_handle != NULL) {
    err = httpd_stop(http_server_handle);
    http_server_handle = NULL;
  }

  if (task_http_server_monitor != NULL) {
    vTaskDelete(task_http_server_monitor);
    task_http_server_monitor = NULL;
  }

  if (http_server_monitor_q_handle != NULL) {
    vQueueDelete(http_server_monitor_q_handle);
    http_server_monitor_q_handle = NULL;
  }

  fw_update_status = OTA_UPDATE_PENDING;
  return err;
}

/*
 * Timer Callback function which calls esp_restart function upon successful
 * firmware update
 */

/**
 * @brief Handles an internal callback for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void http_server_fw_update_reset_cb(void* arg) {
  POOM_DFU_PRINTF_I(TAG, "http_fw_update_reset_cb: Timer timed-out, restarting the device");
  esp_restart();
}

/*
 * Sends a message to the Queue
 * @param msg_id Message ID from the http_server_msg_e enum
 * @return pdTRUE if an item was successfully sent to the queue, otherwise
 * pdFALSE
 */

/**
 * @brief Internal helper for `http_server_monitor_send_msg`.
 *
 * @param[in] msg_id Parameter passed to the function.
 * @return BaseType_t
 */
static BaseType_t http_server_monitor_send_msg(http_server_msg_e msg_id) {
  http_server_q_msg_t msg;
  if (http_server_monitor_q_handle == NULL) {
    return pdFALSE;
  }
  msg.msg_id = msg_id;
  return xQueueSend(http_server_monitor_q_handle, &msg, portMAX_DELAY);
}

// Private Function Definitions
/*
 * HTTP Server Monitor Task used to track events of the HTTP Server.
 * @param pvParameter parameters which can be passed to the task
 * @return http server instance handle if successful, NULL otherwise
 */

/**
 * @brief Internal helper for `http_server_monitor`.
 *
 * @param[in] pvParameter Parameter passed to the function.
 * @return void
 */
static void http_server_monitor(void* pvParameter) {
  http_server_q_msg_t msg;
  for (;;) {
    if (xQueueReceive(http_server_monitor_q_handle, &msg, portMAX_DELAY)) {
      switch (msg.msg_id) {
        case HTTP_MSG_WIFI_OTA_UPDATE_SUCCESSFUL:
          POOM_DFU_PRINTF_I(TAG, "HTTP_MSG_OTA_UPDATE_SUCCESSFUL");
          fw_update_status = OTA_UPDATE_SUCCESSFUL;
          http_server_fw_update_reset_timer();
          break;
        case HTTP_MSG_WIFI_OTA_UPDATE_FAILED:
          POOM_DFU_PRINTF_I(TAG, "HTTP_MSG_OTA_UPDATE_FAILED");
          fw_update_status = OTA_UPDATE_FAILED;
          break;
        default:
          break;
      }
    }
  }
}

/*
 * Sets up the default httpd server configuration
 * @return http server instance handle if successful, NULL otherwise
 */

/**
 * @brief Internal helper for `http_server_configure`.
 *
 * @return httpd_handle_t
 */
static httpd_handle_t http_server_configure(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  http_server_monitor_q_handle =
      xQueueCreate(HTTP_SERVER_MONITOR_QUEUE_LEN, sizeof(http_server_q_msg_t));
  if (http_server_monitor_q_handle == NULL) {
    POOM_DFU_PRINTF_I(TAG, "http_server_configure: Error creating monitor queue");
    return NULL;
  }

  if (xTaskCreate(&http_server_monitor, "http_server_monitor", 4 * 1024u, NULL, 3u,
                  &task_http_server_monitor) != pdPASS) {
    POOM_DFU_PRINTF_I(TAG, "http_server_configure: Error creating monitor task");
    vQueueDelete(http_server_monitor_q_handle);
    http_server_monitor_q_handle = NULL;
    return NULL;
  }


  config.task_priority = 4u;

  config.stack_size = 4 * 1024u;

  config.max_uri_handlers = HTTP_SERVER_MAX_URI_HANDLERS;

  config.recv_wait_timeout = HTTP_SERVER_RECEIVE_WAIT_TIMEOUT;
  config.send_wait_timeout = HTTP_SERVER_SEND_WAIT_TIMEOUT;

  POOM_DFU_PRINTF_I(TAG, "http_server_configure: Starting Server on port: '%d' with task "
           "priority: '%d'",
           config.server_port, config.task_priority);

  if (httpd_start(&http_server_handle, &config) == ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_configure: Registering URI Handlers");
    httpd_uri_t jquery_js = {.uri = "/jquery-3.3.1.min.js",
                             .method = HTTP_GET,
                             .handler = http_server_j_query_handler,
                             .user_ctx = NULL};
    httpd_uri_t index_html = {.uri = "/",
                              .method = HTTP_GET,
                              .handler = http_server_index_html_handler,
                              .user_ctx = NULL};
    httpd_uri_t app_css = {.uri = "/app.css",
                           .method = HTTP_GET,
                           .handler = http_server_app_css_handler,
                           .user_ctx = NULL};
    httpd_uri_t app_js = {.uri = "/app.js",
                          .method = HTTP_GET,
                          .handler = http_server_app_js_handler,
                          .user_ctx = NULL};
    httpd_uri_t favicon_ico = {.uri = "/favicon.ico",
                               .method = HTTP_GET,
                               .handler = http_server_favicon_handler,
                               .user_ctx = NULL};
    httpd_uri_t ota_update = {.uri = "/dfu_update",
                              .method = HTTP_POST,
                              .handler = http_server_ota_update_handler,
                              .user_ctx = NULL};

    httpd_uri_t ota_status = {.uri = "/ota_status",
                              .method = HTTP_POST,
                              .handler = http_server_ota_status_handler,
                              .user_ctx = NULL};

    httpd_register_uri_handler(http_server_handle, &jquery_js);
    httpd_register_uri_handler(http_server_handle, &index_html);
    httpd_register_uri_handler(http_server_handle, &app_css);
    httpd_register_uri_handler(http_server_handle, &app_js);
    httpd_register_uri_handler(http_server_handle, &favicon_ico);
    httpd_register_uri_handler(http_server_handle, &ota_update);
    httpd_register_uri_handler(http_server_handle, &ota_status);
    return http_server_handle;
  }

  POOM_DFU_PRINTF_I(TAG, "http_server_configure: Error starting server!");
  if (task_http_server_monitor != NULL) {
    vTaskDelete(task_http_server_monitor);
    task_http_server_monitor = NULL;
  }
  if (http_server_monitor_q_handle != NULL) {
    vQueueDelete(http_server_monitor_q_handle);
    http_server_monitor_q_handle = NULL;
  }
  return NULL;
}

/*
 * jQuery get handler requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_j_query_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_j_query_handler(httpd_req_t* req) {
  esp_err_t error;
  POOM_DFU_PRINTF_I(TAG, "JQuery Requested");
  httpd_resp_set_type(req, "application/javascript");
  error = httpd_resp_send(req, (const char*) jquery_3_3_1_min_js_start,
                          jquery_3_3_1_min_js_end - jquery_3_3_1_min_js_start);
  if (error != ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_j_query_handler: Error %d while sending Response",
             error);
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_j_query_handler: Response Sent Successfully");
  }
  return error;
}

/*
 * Send the index HTML page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_index_html_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_index_html_handler(httpd_req_t* req) {
  esp_err_t error;
  POOM_DFU_PRINTF_I(TAG, "Index HTML Requested");
  httpd_resp_set_type(req, "text/html");
  error = httpd_resp_send(req, (const char*) index_html_start,
                          index_html_end - index_html_start);
  if (error != ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_index_html_handler: Error %d while sending Response",
             error);
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_index_html_handler: Response Sent Successfully");
  }
  return error;
}

/*
 * app.css get handler is requested when accessing the web page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_app_css_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_app_css_handler(httpd_req_t* req) {
  esp_err_t error;
  POOM_DFU_PRINTF_I(TAG, "APP CSS Requested");
  httpd_resp_set_type(req, "text/css");
  error = httpd_resp_send(req, (const char*) app_css_start,
                          app_css_end - app_css_start);
  if (error != ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_app_css_handler: Error %d while sending Response",
             error);
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_app_css_handler: Response Sent Successfully");
  }
  return error;
}

/*
 * app.js get handler requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_app_js_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_app_js_handler(httpd_req_t* req) {
  esp_err_t error;
  POOM_DFU_PRINTF_I(TAG, "APP JS Requested");
  httpd_resp_set_type(req, "application/javascript");
  error = httpd_resp_send(req, (const char*) app_js_start,
                          app_js_end - app_js_start);
  if (error != ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_app_js_handler: Error %d while sending Response",
             error);
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_app_js_handler: Response Sent Successfully");
  }
  return error;
}

/*
 * Sends the .ico file when accessing the web page
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_favicon_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_favicon_handler(httpd_req_t* req) {
  esp_err_t error;
  POOM_DFU_PRINTF_I(TAG, "Favicon.ico Requested");
  httpd_resp_set_type(req, "image/x-icon");
  error = httpd_resp_send(req, (const char*) favicon_ico_start,
                          favicon_ico_end - favicon_ico_start);
  if (error != ESP_OK) {
    POOM_DFU_PRINTF_I(TAG, "http_server_favicon_handler: Error %d while sending Response",
             error);
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_favicon_handler: Response Sent Successfully");
  }
  return error;
}

/**
 * @brief Receives the *.bin file via the web page and handles the firmware
 * update
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK, other ESP_FAIL if timeout occurs and the update canot be
 * started
 */
static esp_err_t http_server_ota_update_handler(httpd_req_t* req) {
  esp_err_t error;
  esp_ota_handle_t ota_handle;
  char ota_buffer[1024];
  int content_len = req->content_len;  // total content length
  int content_received = 0;
  int recv_len = 0;
  bool is_req_body_started = false;
  bool flash_successful = false;

  poom_fw_update_emit_event(POOM_FW_UPDATE_SHOW_START_EVENT, NULL);

  const esp_partition_t* update_partition =
      esp_ota_get_next_update_partition(NULL);

  do {
    recv_len =
        httpd_req_recv(req, ota_buffer, MIN(content_len, sizeof(ota_buffer)));
    if (recv_len < 0) {
      if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
        POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: Socket Timeout");
        continue;  // Retry Receiving if Timeout Occurred
      }
      POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: OTA Other Error, %d",
               recv_len);
      poom_fw_update_emit_event(POOM_FW_UPDATE_SHOW_RESULT_EVENT, &flash_successful);
      return ESP_FAIL;
    }
    POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: OTA RX: %d of %d",
             content_received, content_len);
    uint8_t ota_progress = (content_received * 100) / content_len;

    poom_fw_update_emit_event(POOM_FW_UPDATE_SHOW_PROGRESS_EVENT, &ota_progress);

    if (!is_req_body_started) {
      is_req_body_started = true;
      char* body_start_p = strstr(ota_buffer, "\r\n\r\n") + 4u;
      int body_part_len = recv_len - (body_start_p - ota_buffer);
      POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: OTA File Size: %d",
               content_len);
      error = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
      if (error != ESP_OK) {
        POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: Error with OTA Begin, "
                 "Canceling OTA");
        poom_fw_update_emit_event(POOM_FW_UPDATE_SHOW_RESULT_EVENT, &flash_successful);
        return ESP_FAIL;
      } else {
        POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: Writing to partition subtype "
                 "%d at offset 0x%lx",
                 update_partition->subtype, update_partition->address);
      }
      esp_ota_write(ota_handle, body_start_p, body_part_len);
      content_received += body_part_len;
    } else {
      esp_ota_write(ota_handle, ota_buffer, recv_len);
      content_received += recv_len;
    }

  } while ((recv_len > 0) && (content_received < content_len));

  if (esp_ota_end(ota_handle) == ESP_OK) {
    if (esp_ota_set_boot_partition(update_partition) == ESP_OK) {
      const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
      POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: Next boot partition subtype %d "
               "at offset 0x%lx",
               boot_partition->subtype, boot_partition->address);
      flash_successful = true;
    } else {
      POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: Flash Error");
    }
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_ota_update_handler: esp_ota_end Error");
  }

  if (flash_successful) {
    http_server_monitor_send_msg(HTTP_MSG_WIFI_OTA_UPDATE_SUCCESSFUL);
  } else {
    http_server_monitor_send_msg(HTTP_MSG_WIFI_OTA_UPDATE_FAILED);
  }
  poom_fw_update_emit_event(POOM_FW_UPDATE_SHOW_RESULT_EVENT, &flash_successful);
  return ESP_OK;
}

/*
 * OTA status handler responds with the firmware update status after the OTA
 * update is started and responds with the compile time & date when the page is
 * first requested
 * @param req HTTP request for which the URI needs to be handled
 * @return ESP_OK
 */

/**
 * @brief Internal helper for `http_server_ota_status_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t http_server_ota_status_handler(httpd_req_t* req) {
  char ota_JSON[200];
  const esp_app_desc_t* desc = esp_app_get_description();
  const char* ver = (desc != NULL) ? desc->version : "unknown";
  POOM_DFU_PRINTF_I(TAG, "OTA Status Requested");
  sprintf(ota_JSON,
          "{\"ota_update_status\":%d,\"current_fw_version\":"
          "\"%s\"}",
          fw_update_status, ver);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ota_JSON, strlen(ota_JSON));

  return ESP_OK;
}

/*
 * Check the fw_update_status and creates the fw_update_reset time if the
 * fw_update_status is true
 */

/**
 * @brief Internal helper for `http_server_fw_update_reset_timer`.
 *
 * @return void
 */
static void http_server_fw_update_reset_timer(void) {
  if (fw_update_status == OTA_UPDATE_SUCCESSFUL) {
    POOM_DFU_PRINTF_I(TAG, "http_server_fw_update_reset_timer: FW Update successful "
             "starting FW update reset timer");
    if (fw_update_reset != NULL) {
      (void)esp_timer_stop(fw_update_reset);
      (void)esp_timer_delete(fw_update_reset);
      fw_update_reset = NULL;
    }
    ESP_ERROR_CHECK(esp_timer_create(&fw_update_reset_args, &fw_update_reset));
    ESP_ERROR_CHECK(esp_timer_start_once(fw_update_reset, 3 * 1000 * 1000));
  } else {
    POOM_DFU_PRINTF_I(TAG, "http_server_fw_update_reset_timer: FW Update unsuccessful");
  }
}
