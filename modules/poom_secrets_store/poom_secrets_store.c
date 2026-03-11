// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_secrets_store.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#ifndef NVS_KEY_NAME_MAX_SIZE
#define NVS_KEY_NAME_MAX_SIZE (16U)
#endif

#define POOM_SECRETS_NVS_PARTITION_NAME   "nvs"
#define POOM_SECRETS_RECORD_KEY_DATA_FMT  "d%08" PRIx32
#define POOM_SECRETS_RECORD_KEY_ID_FMT    "i%08" PRIx32
#define POOM_SECRETS_RECORD_KEY_BUF_LEN   (NVS_KEY_NAME_MAX_SIZE)

static bool poom_secrets_is_valid_key_(const char* key) {
    size_t len;

    if(key == NULL) {
        return false;
    }

    len = strlen(key);
    if((len == 0U) || (len >= NVS_KEY_NAME_MAX_SIZE)) {
        return false;
    }

    return true;
}

static bool poom_secrets_is_valid_record_id_(const char* id) {
    size_t len;

    if(id == NULL) {
        return false;
    }

    len = strlen(id);
    if((len == 0U) || (len > POOM_SECRETS_RECORD_ID_MAX_LEN)) {
        return false;
    }

    return true;
}

static uint32_t poom_secrets_fnv1a32_(const char* text) {
    uint32_t hash = 2166136261UL;
    size_t i;
    size_t len = strlen(text);

    for(i = 0U; i < len; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619UL;
    }

    return hash;
}

static esp_err_t poom_secrets_build_record_keys_(
    const char* id,
    char* out_key_data,
    size_t out_key_data_len,
    char* out_key_id,
    size_t out_key_id_len) {
    uint32_t hash;
    int nw_data;
    int nw_id;

    if((id == NULL) || (out_key_data == NULL) || (out_key_id == NULL) ||
       (out_key_data_len == 0U) || (out_key_id_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!poom_secrets_is_valid_record_id_(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    hash = poom_secrets_fnv1a32_(id);
    nw_data = snprintf(out_key_data, out_key_data_len, POOM_SECRETS_RECORD_KEY_DATA_FMT, hash);
    nw_id = snprintf(out_key_id, out_key_id_len, POOM_SECRETS_RECORD_KEY_ID_FMT, hash);
    if((nw_data < 0) || ((size_t)nw_data >= out_key_data_len) ||
       (nw_id < 0) || ((size_t)nw_id >= out_key_id_len)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t poom_secrets_open_(nvs_open_mode_t mode, nvs_handle_t* out_handle) {
    if(out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_open(POOM_SECRETS_NAMESPACE, mode, out_handle);
}

static esp_err_t poom_secrets_commit_and_close_(nvs_handle_t handle, esp_err_t op_err) {
    esp_err_t commit_err = ESP_OK;

    if(op_err == ESP_OK) {
        commit_err = nvs_commit(handle);
    }

    nvs_close(handle);

    if(op_err != ESP_OK) {
        return op_err;
    }

    return commit_err;
}

esp_err_t poom_secrets_init(void) {
    esp_err_t err = nvs_flash_init();

    if((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        err = nvs_flash_erase();
        if(err != ESP_OK) {
            return err;
        }

        err = nvs_flash_init();
    }

    return err;
}

esp_err_t poom_secrets_set_str(const char* key, const char* value) {
    nvs_handle_t handle;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_get_str(const char* key, char* out_value, size_t* inout_len) {
    nvs_handle_t handle;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (inout_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, key, out_value, inout_len);
    nvs_close(handle);
    return err;
}

esp_err_t poom_secrets_set_u32(const char* key, uint32_t value) {
    nvs_handle_t handle;
    esp_err_t err;

    if(!poom_secrets_is_valid_key_(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, key, value);
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_get_u32(const char* key, uint32_t* out_value) {
    nvs_handle_t handle;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (out_value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, key, out_value);
    nvs_close(handle);
    return err;
}

esp_err_t poom_secrets_set_blob(const char* key, const void* data, size_t data_len) {
    nvs_handle_t handle;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (data == NULL) || (data_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, key, data, data_len);
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_get_blob(const char* key, void* out_data, size_t* inout_len) {
    nvs_handle_t handle;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (inout_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, key, out_data, inout_len);
    nvs_close(handle);
    return err;
}

esp_err_t poom_secrets_erase_key(const char* key) {
    nvs_handle_t handle;
    esp_err_t err;

    if(!poom_secrets_is_valid_key_(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_clear(void) {
    nvs_handle_t handle;
    esp_err_t err;

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_key_exists(const char* key, bool* out_exists) {
    nvs_iterator_t it;
    esp_err_t err;

    if((!poom_secrets_is_valid_key_(key)) || (out_exists == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_exists = false;
    it = NULL;
    err = nvs_entry_find(POOM_SECRETS_NVS_PARTITION_NAME, POOM_SECRETS_NAMESPACE, NVS_TYPE_ANY, &it);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(err != ESP_OK) {
        return err;
    }

    while((err == ESP_OK) && (it != NULL)) {
        nvs_entry_info_t info;
        err = nvs_entry_info(it, &info);
        if(err != ESP_OK) {
            nvs_release_iterator(it);
            return err;
        }

        if(strcmp(info.key, key) == 0) {
            *out_exists = true;
            nvs_release_iterator(it);
            return ESP_OK;
        }

        err = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);
    if((err != ESP_OK) && (err != ESP_ERR_NVS_NOT_FOUND)) {
        return err;
    }
    return ESP_OK;
}

esp_err_t poom_secrets_set_record_blob(const char* id, const void* data, size_t data_len) {
    nvs_handle_t handle;
    esp_err_t err;
    char key_data[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};
    char key_id[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};

    if((!poom_secrets_is_valid_record_id_(id)) || (data == NULL) || (data_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_build_record_keys_(id, key_data, sizeof(key_data), key_id, sizeof(key_id));
    if(err != ESP_OK) {
        return err;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key_id, id);
    if(err == ESP_OK) {
        err = nvs_set_blob(handle, key_data, data, data_len);
    }
    return poom_secrets_commit_and_close_(handle, err);
}

esp_err_t poom_secrets_get_record_blob(const char* id, void* out_data, size_t* inout_len) {
    nvs_handle_t handle;
    esp_err_t err;
    size_t saved_id_len = 0U;
    char* saved_id = NULL;
    char key_data[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};
    char key_id[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};

    if((!poom_secrets_is_valid_record_id_(id)) || (inout_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_build_record_keys_(id, key_data, sizeof(key_data), key_id, sizeof(key_id));
    if(err != ESP_OK) {
        return err;
    }

    err = poom_secrets_open_(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, key_id, NULL, &saved_id_len);
    if(err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    saved_id = (char*)calloc(1U, saved_id_len);
    if(saved_id == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, key_id, saved_id, &saved_id_len);
    if(err != ESP_OK) {
        free(saved_id);
        nvs_close(handle);
        return err;
    }

    if(strcmp(saved_id, id) != 0) {
        free(saved_id);
        nvs_close(handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    err = nvs_get_blob(handle, key_data, out_data, inout_len);
    free(saved_id);
    nvs_close(handle);
    return err;
}

esp_err_t poom_secrets_get_record_size(const char* id, size_t* out_len) {
    size_t len = 0U;
    esp_err_t err;

    if((!poom_secrets_is_valid_record_id_(id)) || (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_get_record_blob(id, NULL, &len);
    if(err != ESP_OK) {
        return err;
    }

    *out_len = len;
    return ESP_OK;
}

esp_err_t poom_secrets_get_record_blob_alloc(const char* id, void** out_data, size_t* out_len) {
    void* buffer = NULL;
    size_t needed_len = 0U;
    size_t read_len = 0U;
    esp_err_t err;

    if((!poom_secrets_is_valid_record_id_(id)) || (out_data == NULL) || (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_len = 0U;

    err = poom_secrets_get_record_size(id, &needed_len);
    if(err != ESP_OK) {
        return err;
    }

    if(needed_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer = calloc(1U, needed_len);
    if(buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    read_len = needed_len;
    err = poom_secrets_get_record_blob(id, buffer, &read_len);
    if(err != ESP_OK) {
        free(buffer);
        return err;
    }

    *out_data = buffer;
    *out_len = read_len;
    return ESP_OK;
}

esp_err_t poom_secrets_record_exists(const char* id, bool* out_exists) {
    size_t tmp_len = 0U;
    esp_err_t err;

    if((!poom_secrets_is_valid_record_id_(id)) || (out_exists == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_get_record_blob(id, NULL, &tmp_len);
    if(err == ESP_OK) {
        *out_exists = true;
        return ESP_OK;
    }
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        *out_exists = false;
        return ESP_OK;
    }

    *out_exists = false;
    return err;
}

esp_err_t poom_secrets_erase_record(const char* id) {
    nvs_handle_t handle;
    esp_err_t err;
    char key_data[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};
    char key_id[POOM_SECRETS_RECORD_KEY_BUF_LEN] = {0};

    if(!poom_secrets_is_valid_record_id_(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_secrets_build_record_keys_(id, key_data, sizeof(key_data), key_id, sizeof(key_id));
    if(err != ESP_OK) {
        return err;
    }

    err = poom_secrets_open_(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key_id);
    if((err != ESP_OK) && (err != ESP_ERR_NVS_NOT_FOUND)) {
        nvs_close(handle);
        return err;
    }

    err = nvs_erase_key(handle, key_data);
    if((err != ESP_OK) && (err != ESP_ERR_NVS_NOT_FOUND)) {
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t poom_secrets_set_wifi_ssid(const char* ssid) {
    return poom_secrets_set_str(POOM_SECRETS_KEY_WIFI_SSID, ssid);
}

esp_err_t poom_secrets_get_wifi_ssid(char* out_ssid, size_t* inout_len) {
    return poom_secrets_get_str(POOM_SECRETS_KEY_WIFI_SSID, out_ssid, inout_len);
}

esp_err_t poom_secrets_set_wifi_pass(const char* password) {
    return poom_secrets_set_str(POOM_SECRETS_KEY_WIFI_PASS, password);
}

esp_err_t poom_secrets_get_wifi_pass(char* out_password, size_t* inout_len) {
    return poom_secrets_get_str(POOM_SECRETS_KEY_WIFI_PASS, out_password, inout_len);
}

esp_err_t poom_secrets_set_api_token(const char* token) {
    return poom_secrets_set_str(POOM_SECRETS_KEY_API_TOKEN, token);
}

esp_err_t poom_secrets_get_api_token(char* out_token, size_t* inout_len) {
    return poom_secrets_get_str(POOM_SECRETS_KEY_API_TOKEN, out_token, inout_len);
}
