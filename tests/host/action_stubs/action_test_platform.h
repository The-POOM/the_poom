#ifndef ACTION_TEST_PLATFORM_H
#define ACTION_TEST_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_IDF_TARGET_ESP32C5 1
#define ARDUBOY_WIDTH 128
#define INVERT 2
#define BUTTON_SINGLE_CLICK 4
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
const char *esp_err_to_name(esp_err_t error);
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef struct test_queue *QueueHandle_t;
typedef unsigned TickType_t;
typedef void (*TaskFunction_t)(void *);

typedef enum {
    WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_ENTERPRISE
} wifi_auth_mode_t;
typedef struct {
    uint8_t ssid[33]; uint8_t bssid[6]; uint8_t primary;
    int8_t rssi; wifi_auth_mode_t authmode;
} wifi_ap_record_t;
#define WIFI_SCAN_TYPE_ACTIVE 0
typedef struct {
    bool show_hidden; int scan_type;
    struct { struct { uint32_t min, max; } active; } scan_time;
} wifi_scan_config_t;
esp_err_t esp_wifi_scan_stop(void);
esp_err_t poom_wifi_ctrl_deinit(void);
bool poom_wifi_ctrl_channel_is_valid(uint8_t channel);

typedef struct { uint8_t button, event; } button_event_msg_t;
typedef struct { const void *data; size_t len; } poom_sbus_msg_t;
typedef void (*test_sbus_callback_t)(const poom_sbus_msg_t *, void *);
bool poom_sbus_subscribe_cb(const char *, test_sbus_callback_t, void *);
bool poom_sbus_unsubscribe_cb(const char *, test_sbus_callback_t, void *);
bool poom_sbus_publish(const char *, const void *, size_t, uint32_t);

QueueHandle_t xQueueCreate(unsigned count, unsigned item_size);
int xQueueSend(QueueHandle_t queue, const void *event, unsigned wait);
int xQueueReceive(QueueHandle_t queue, void *event, unsigned wait);
void vQueueDelete(QueueHandle_t queue);
int xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(unsigned ticks);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
int64_t esp_timer_get_time(void);
void poom_arduboy_clear(void);
void poom_arduboy_set_text_size(unsigned size);
void poom_arduboy_set_cursor(int x, int y);
void poom_arduboy_print(const char *text);
void poom_arduboy_fill_rect(int x, int y, int width, int height, int color);
void poom_arduboy_display(void);
#endif
