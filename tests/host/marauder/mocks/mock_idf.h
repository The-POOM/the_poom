#ifndef MOCK_IDF_H
#define MOCK_IDF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
typedef enum {
    WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA2_WPA3_ENTERPRISE, WIFI_AUTH_WPA3_ENT_192, WIFI_AUTH_WAPI_PSK
} wifi_auth_mode_t;
typedef struct {uint8_t bssid[6], ssid[33], primary; wifi_auth_mode_t authmode;} wifi_ap_record_t;
#define WIFI_IF_STA 0
#define WIFI_PS_NONE 0
esp_err_t esp_wifi_80211_tx(int interface_index, const void *frame, int length, bool system_sequence);
esp_err_t esp_wifi_set_ps(int mode);
esp_err_t poom_wifi_ctrl_init_sta(void);
esp_err_t poom_wifi_ctrl_deinit(void);
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel);
esp_err_t poom_wifi_ctrl_set_promiscuous(bool enabled);
bool poom_wifi_ctrl_channel_is_valid(uint8_t channel);
void esp_fill_random(void *buffer, size_t length);
int64_t esp_timer_get_time(void);
typedef SRWLOCK portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define taskENTER_CRITICAL(lock) AcquireSRWLockExclusive(lock)
#define taskEXIT_CRITICAL(lock) ReleaseSRWLockExclusive(lock)
typedef HANDLE SemaphoreHandle_t;
#define portMAX_DELAY INFINITE
#define pdMS_TO_TICKS(value) (value)
#define pdPASS 1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, DWORD timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle);
void vTaskDelay(unsigned milliseconds);
void vTaskDelete(void *task);
typedef int psa_status_t;
typedef unsigned psa_key_id_t;
typedef struct {unsigned type,bits,usage;} psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ERROR_NOT_SUPPORTED -134
#define PSA_ERROR_INSUFFICIENT_MEMORY -141
#define PSA_ECC_FAMILY_SECP_R1 1
#define PSA_KEY_TYPE_ECC_KEY_PAIR(family) (family)
#define PSA_KEY_USAGE_EXPORT 1
void psa_set_key_type(psa_key_attributes_t *, unsigned);
void psa_set_key_bits(psa_key_attributes_t *, unsigned);
void psa_set_key_usage_flags(psa_key_attributes_t *, unsigned);
void psa_reset_key_attributes(psa_key_attributes_t *);
psa_status_t psa_crypto_init(void);
psa_status_t psa_generate_key(const psa_key_attributes_t *, psa_key_id_t *);
psa_status_t psa_export_key(psa_key_id_t, uint8_t *, size_t, size_t *);
psa_status_t psa_export_public_key(psa_key_id_t, uint8_t *, size_t, size_t *);
psa_status_t psa_destroy_key(psa_key_id_t);
#endif
