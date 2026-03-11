// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_SECRETS_STORE_H
#define POOM_SECRETS_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/** @brief Default NVS namespace used by the secrets store. */
#define POOM_SECRETS_NAMESPACE "poom_sec"

/** @brief Common key for Wi-Fi SSID. */
#define POOM_SECRETS_KEY_WIFI_SSID "wifi_ssid"
/** @brief Common key for Wi-Fi password. */
#define POOM_SECRETS_KEY_WIFI_PASS "wifi_pass"
/** @brief Common key for API token. */
#define POOM_SECRETS_KEY_API_TOKEN "api_token"
/** @brief Max ID length accepted by dynamic record API. */
#define POOM_SECRETS_RECORD_ID_MAX_LEN (128U)

/**
 * @brief Initializes NVS flash for secrets storage.
 *
 * @return esp_err_t
 */
esp_err_t poom_secrets_init(void);

/**
 * @brief Stores a string value by key in NVS.
 *
 * @param[in] key NVS key.
 * @param[in] value NUL-terminated value.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_str(const char* key, const char* value);

/**
 * @brief Reads a string value by key from NVS.
 *
 * @param[in] key NVS key.
 * @param[out] out_value Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_str(const char* key, char* out_value, size_t* inout_len);

/**
 * @brief Stores a uint32 value by key in NVS.
 *
 * @param[in] key NVS key.
 * @param[in] value Value to store.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_u32(const char* key, uint32_t value);

/**
 * @brief Reads a uint32 value by key from NVS.
 *
 * @param[in] key NVS key.
 * @param[out] out_value Output value pointer.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_u32(const char* key, uint32_t* out_value);

/**
 * @brief Stores a binary blob by key in NVS.
 *
 * @param[in] key NVS key.
 * @param[in] data Blob data.
 * @param[in] data_len Blob size in bytes.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_blob(const char* key, const void* data, size_t data_len);

/**
 * @brief Reads a binary blob by key from NVS.
 *
 * @param[in] key NVS key.
 * @param[out] out_data Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_blob(const char* key, void* out_data, size_t* inout_len);

/**
 * @brief Erases one key from secrets namespace.
 *
 * @param[in] key NVS key.
 * @return esp_err_t
 */
esp_err_t poom_secrets_erase_key(const char* key);

/**
 * @brief Erases all keys from secrets namespace.
 *
 * @return esp_err_t
 */
esp_err_t poom_secrets_clear(void);

/**
 * @brief Checks whether a key exists in secrets namespace.
 *
 * @param[in] key NVS key.
 * @param[out] out_exists True when key exists.
 * @return esp_err_t
 */
esp_err_t poom_secrets_key_exists(const char* key, bool* out_exists);

/**
 * @brief Stores a dynamic record by string ID as blob.
 *
 * @param[in] id Dynamic record identifier.
 * @param[in] data Blob data.
 * @param[in] data_len Blob size in bytes.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_record_blob(const char* id, const void* data, size_t data_len);

/**
 * @brief Reads a dynamic record by string ID as blob.
 *
 * @param[in] id Dynamic record identifier.
 * @param[out] out_data Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_record_blob(const char* id, void* out_data, size_t* inout_len);

/**
 * @brief Gets dynamic record size in bytes by ID.
 *
 * @param[in] id Dynamic record identifier.
 * @param[out] out_len Record size in bytes.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_record_size(const char* id, size_t* out_len);

/**
 * @brief Reads a dynamic record by ID and allocates output buffer.
 *
 * @param[in] id Dynamic record identifier.
 * @param[out] out_data Allocated buffer. Caller must free.
 * @param[out] out_len Record size in bytes.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_record_blob_alloc(const char* id, void** out_data, size_t* out_len);

/**
 * @brief Checks whether a dynamic record exists by ID.
 *
 * @param[in] id Dynamic record identifier.
 * @param[out] out_exists True when record exists.
 * @return esp_err_t
 */
esp_err_t poom_secrets_record_exists(const char* id, bool* out_exists);

/**
 * @brief Erases a dynamic record by ID.
 *
 * @param[in] id Dynamic record identifier.
 * @return esp_err_t
 */
esp_err_t poom_secrets_erase_record(const char* id);

/**
 * @brief Stores Wi-Fi SSID in default key.
 *
 * @param[in] ssid NUL-terminated SSID.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_wifi_ssid(const char* ssid);

/**
 * @brief Reads Wi-Fi SSID from default key.
 *
 * @param[out] out_ssid Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_wifi_ssid(char* out_ssid, size_t* inout_len);

/**
 * @brief Stores Wi-Fi password in default key.
 *
 * @param[in] password NUL-terminated password.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_wifi_pass(const char* password);

/**
 * @brief Reads Wi-Fi password from default key.
 *
 * @param[out] out_password Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_wifi_pass(char* out_password, size_t* inout_len);

/**
 * @brief Stores API token in default key.
 *
 * @param[in] token NUL-terminated token.
 * @return esp_err_t
 */
esp_err_t poom_secrets_set_api_token(const char* token);

/**
 * @brief Reads API token from default key.
 *
 * @param[out] out_token Output buffer or NULL to query required length.
 * @param[in,out] inout_len Buffer size on input, required size on output.
 * @return esp_err_t
 */
esp_err_t poom_secrets_get_api_token(char* out_token, size_t* inout_len);

#ifdef __cplusplus
}
#endif

#endif /* POOM_SECRETS_STORE_H */
