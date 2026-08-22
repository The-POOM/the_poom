// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_web_ir_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bsp_pong.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ir_tx.h"

#define POOM_WEB_IR_PATH_MAX_LEN (320U)
#define POOM_WEB_IR_TOKEN_MAX_LEN (24U)
#define POOM_WEB_IR_NEC_CLK_HZ (1000000U)
#define POOM_WEB_IR_NEC_CARRIER_HZ (38000U)
#define POOM_WEB_IR_NEC_DUTY_CYCLE (0.33f)

typedef struct
{
    bool active;
    bool transmitter_ready;
    char token[POOM_WEB_IR_TOKEN_MAX_LEN];
    char path[POOM_WEB_IR_PATH_MAX_LEN];
    size_t command_count;
    poom_web_ir_command_t* commands;
    ir_tx_handle_t transmitter;
} poom_web_ir_session_state_t;

static SemaphoreHandle_t s_poom_web_ir_mutex = NULL;
static poom_web_ir_session_state_t s_poom_web_ir_session = {0};

/**
 * @brief Internal helper for `poom_web_ir_mutex_get`.
 *
 * @return SemaphoreHandle_t
 */
static SemaphoreHandle_t poom_web_ir_mutex_get_(void)
{
    if(s_poom_web_ir_mutex == NULL)
    {
        s_poom_web_ir_mutex = xSemaphoreCreateMutex();
    }

    return s_poom_web_ir_mutex;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_byte Parameter passed to the function.
 * @return bool
 */
static bool poom_web_ir_parse_hex_byte_(const char* text, uint8_t* out_byte)
{
    unsigned int value = 0U;

    if((text == NULL) || (out_byte == NULL))
    {
        return false;
    }

    if(sscanf(text, " %2x", &value) != 1)
    {
        return false;
    }

    if(value > 0xFFU)
    {
        return false;
    }

    *out_byte = (uint8_t)value;
    return true;
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] out_bytes Parameter passed to the function.
 * @return bool
 */
static bool poom_web_ir_parse_hex4_(const char* text, uint8_t out_bytes[4])
{
    const char* cursor = text;

    if((cursor == NULL) || (out_bytes == NULL))
    {
        return false;
    }

    for(int i = 0; i < 4; i++)
    {
        while((*cursor == ' ') || (*cursor == '\t'))
        {
            cursor++;
        }

        if(*cursor == '\0')
        {
            return false;
        }

        if(!poom_web_ir_parse_hex_byte_(cursor, &out_bytes[i]))
        {
            return false;
        }

        while((*cursor != '\0') &&
              (*cursor != ' ') &&
              (*cursor != '\t') &&
              (*cursor != '\r') &&
              (*cursor != '\n'))
        {
            cursor++;
        }
    }

    return true;
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] abs_path Parameter passed to the function.
 * @param[in] out_commands Parameter passed to the function.
 * @param[in] max_commands Parameter passed to the function.
 * @param[in] out_count Parameter passed to the function.
 * @return bool
 */
static bool poom_web_ir_load_commands_(const char* abs_path,
                                       poom_web_ir_command_t* out_commands,
                                       size_t max_commands,
                                       size_t* out_count)
{
    FILE* file = NULL;
    char line[96];
    char cur_name[POOM_WEB_IR_NAME_MAX_LEN + 1U] = {0};
    char cur_type[16] = {0};
    char cur_proto[16] = {0};
    uint8_t cur_addr[4] = {0};
    uint8_t cur_cmd[4] = {0};
    bool have_name = false;
    bool have_type = false;
    bool have_proto = false;
    bool have_addr = false;
    bool have_cmd = false;
    size_t count = 0U;

    if((abs_path == NULL) || (abs_path[0] == '\0') || (out_commands == NULL) || (out_count == NULL))
    {
        return false;
    }

    *out_count = 0U;

    file = fopen(abs_path, "r");
    if(file == NULL)
    {
        return false;
    }

    while(fgets(line, (int)sizeof(line), file) != NULL)
    {
        size_t line_len = strlen(line);
        while((line_len > 0U) && ((line[line_len - 1U] == '\r') || (line[line_len - 1U] == '\n')))
        {
            line[line_len - 1U] = '\0';
            line_len--;
        }

        if(line[0] == '#')
        {
            continue;
        }

        if((strncmp(line, "name:", 5) == 0) || (strncmp(line, "name: ", 6) == 0))
        {
            const char* value = strchr(line, ':');
            value = (value != NULL) ? (value + 1) : "";
            while((*value == ' ') || (*value == '\t'))
            {
                value++;
            }
            (void)snprintf(cur_name, sizeof(cur_name), "%.*s", (int)POOM_WEB_IR_NAME_MAX_LEN, value);
            have_name = (cur_name[0] != '\0');
        }
        else if((strncmp(line, "type:", 5) == 0) || (strncmp(line, "type: ", 6) == 0))
        {
            const char* value = strchr(line, ':');
            value = (value != NULL) ? (value + 1) : "";
            while((*value == ' ') || (*value == '\t'))
            {
                value++;
            }
            (void)snprintf(cur_type, sizeof(cur_type), "%.*s", 15, value);
            have_type = (cur_type[0] != '\0');
        }
        else if((strncmp(line, "protocol:", 9) == 0) || (strncmp(line, "protocol: ", 10) == 0))
        {
            const char* value = strchr(line, ':');
            value = (value != NULL) ? (value + 1) : "";
            while((*value == ' ') || (*value == '\t'))
            {
                value++;
            }
            (void)snprintf(cur_proto, sizeof(cur_proto), "%.*s", 15, value);
            have_proto = (cur_proto[0] != '\0');
        }
        else if((strncmp(line, "address:", 8) == 0) || (strncmp(line, "address: ", 9) == 0))
        {
            const char* value = strchr(line, ':');
            value = (value != NULL) ? (value + 1) : "";
            have_addr = poom_web_ir_parse_hex4_(value, cur_addr);
        }
        else if((strncmp(line, "command:", 8) == 0) || (strncmp(line, "command: ", 9) == 0))
        {
            const char* value = strchr(line, ':');
            value = (value != NULL) ? (value + 1) : "";
            have_cmd = poom_web_ir_parse_hex4_(value, cur_cmd);
        }

        if(have_name && have_type && have_proto && have_addr && have_cmd)
        {
            if((strcasecmp(cur_type, "parsed") == 0) && (count < max_commands))
            {
                poom_web_ir_proto_t proto = IR_PROTOCOL_NONE;
                (void)ir_protocol_parse_name(cur_proto, &proto);

                if(proto != IR_PROTOCOL_NONE)
                {
                    poom_web_ir_command_t* command = &out_commands[count];
                    (void)snprintf(command->name, sizeof(command->name), "%s", cur_name);
                    command->proto = proto;
                    command->address = (uint32_t)cur_addr[0] |
                                       ((uint32_t)cur_addr[1] << 8) |
                                       ((uint32_t)cur_addr[2] << 16) |
                                       ((uint32_t)cur_addr[3] << 24);
                    command->command = (uint32_t)cur_cmd[0] |
                                       ((uint32_t)cur_cmd[1] << 8) |
                                       ((uint32_t)cur_cmd[2] << 16) |
                                       ((uint32_t)cur_cmd[3] << 24);
                    count++;
                }
            }

            have_name = false;
            have_type = false;
            have_proto = false;
            have_addr = false;
            have_cmd = false;
            cur_name[0] = '\0';
            cur_type[0] = '\0';
            cur_proto[0] = '\0';
            (void)memset(cur_addr, 0, sizeof(cur_addr));
            (void)memset(cur_cmd, 0, sizeof(cur_cmd));
        }
    }

    (void)fclose(file);
    *out_count = count;
    return count > 0U;
}

/**
 * @brief Internal helper for `poom_web_ir_session_reset_locked`.
 *
 * @return void
 */
static void poom_web_ir_session_reset_locked_(void)
{
    if(s_poom_web_ir_session.transmitter_ready)
    {
        (void)ir_tx_deinit(&s_poom_web_ir_session.transmitter);
    }

    free(s_poom_web_ir_session.commands);
    s_poom_web_ir_session.commands = NULL;

    (void)memset(&s_poom_web_ir_session, 0, sizeof(s_poom_web_ir_session));
}

bool poom_web_ir_is_supported_path(const char* path)
{
    const char* dot = NULL;

    if((path == NULL) || (path[0] == '\0'))
    {
        return false;
    }

    dot = strrchr(path, '.');
    if(dot == NULL)
    {
        return false;
    }

    return strcasecmp(dot, ".ir") == 0;
}

const char* poom_web_ir_proto_name(poom_web_ir_proto_t proto)
{
    return ir_protocol_name(proto);
}

esp_err_t poom_web_ir_session_open(const char* abs_path,
                                   char* out_token,
                                   size_t out_token_len,
                                   poom_web_ir_command_t* out_commands,
                                   size_t max_commands,
                                   size_t* out_count)
{
#if !defined(PIN_NUM_IR_TX)
    (void)abs_path;
    (void)out_token;
    (void)out_token_len;
    (void)out_commands;
    (void)max_commands;
    (void)out_count;
    return ESP_ERR_NOT_SUPPORTED;
#else
    SemaphoreHandle_t mutex = NULL;
    poom_web_ir_command_t* parsed_commands = NULL;
    size_t parsed_count = 0U;
    int64_t token_seed = 0;

    if((abs_path == NULL) || (out_token == NULL) || (out_token_len == 0U) || (out_count == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(((out_commands == NULL) && (max_commands != 0U)) || ((out_commands != NULL) && (max_commands == 0U)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    parsed_commands = (poom_web_ir_command_t*)calloc(POOM_WEB_IR_MAX_COMMANDS, sizeof(*parsed_commands));
    if(parsed_commands == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if(!poom_web_ir_load_commands_(abs_path, parsed_commands, POOM_WEB_IR_MAX_COMMANDS, &parsed_count))
    {
        free(parsed_commands);
        return ESP_ERR_INVALID_ARG;
    }

    if((out_commands != NULL) && (parsed_count > max_commands))
    {
        free(parsed_commands);
        return ESP_ERR_INVALID_SIZE;
    }

    mutex = poom_web_ir_mutex_get_();
    if(mutex == NULL)
    {
        free(parsed_commands);
        return ESP_ERR_NO_MEM;
    }

    if(xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        free(parsed_commands);
        return ESP_FAIL;
    }

    poom_web_ir_session_reset_locked_();

    s_poom_web_ir_session.commands = parsed_commands;
    s_poom_web_ir_session.command_count = parsed_count;
    s_poom_web_ir_session.active = true;
    (void)snprintf(s_poom_web_ir_session.path, sizeof(s_poom_web_ir_session.path), "%s", abs_path);

    token_seed = esp_timer_get_time();
    (void)snprintf(s_poom_web_ir_session.token,
                   sizeof(s_poom_web_ir_session.token),
                   "%llX",
                   (unsigned long long)token_seed);

    if(out_commands != NULL)
    {
        (void)memcpy(out_commands, parsed_commands, parsed_count * sizeof(parsed_commands[0]));
    }
    *out_count = parsed_count;
    (void)snprintf(out_token, out_token_len, "%s", s_poom_web_ir_session.token);

    (void)xSemaphoreGive(mutex);
    return ESP_OK;
#endif
}

esp_err_t poom_web_ir_session_get_command(const char* token, size_t index, poom_web_ir_command_t* out_command)
{
#if !defined(PIN_NUM_IR_TX)
    (void)token;
    (void)index;
    (void)out_command;
    return ESP_ERR_NOT_SUPPORTED;
#else
    SemaphoreHandle_t mutex = NULL;

    if((token == NULL) || (token[0] == '\0') || (out_command == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    mutex = poom_web_ir_mutex_get_();
    if(mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if(xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if((!s_poom_web_ir_session.active) || (strcmp(token, s_poom_web_ir_session.token) != 0))
    {
        (void)xSemaphoreGive(mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if(index >= s_poom_web_ir_session.command_count)
    {
        (void)xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_ARG;
    }

    *out_command = s_poom_web_ir_session.commands[index];
    (void)xSemaphoreGive(mutex);
    return ESP_OK;
#endif
}

esp_err_t poom_web_ir_session_send(const char* token, size_t index)
{
#if !defined(PIN_NUM_IR_TX)
    (void)token;
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
#else
    SemaphoreHandle_t mutex = NULL;
    ir_tx_config_t transmitter_cfg;
    const poom_web_ir_command_t* command = NULL;

    if((token == NULL) || (token[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    mutex = poom_web_ir_mutex_get_();
    if(mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if(xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if((!s_poom_web_ir_session.active) || (strcmp(token, s_poom_web_ir_session.token) != 0))
    {
        (void)xSemaphoreGive(mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if(index >= s_poom_web_ir_session.command_count)
    {
        (void)xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if(!s_poom_web_ir_session.transmitter_ready)
    {
        esp_err_t err;

        transmitter_cfg = ir_tx_default_config();
        transmitter_cfg.gpio = PIN_NUM_IR_TX;
        transmitter_cfg.clk_hz = POOM_WEB_IR_NEC_CLK_HZ;
        transmitter_cfg.carrier_hz = POOM_WEB_IR_NEC_CARRIER_HZ;
        transmitter_cfg.duty_cycle = POOM_WEB_IR_NEC_DUTY_CYCLE;

        err = ir_tx_init(&s_poom_web_ir_session.transmitter, &transmitter_cfg, "poom_web_ir_tx");
        if(err != ESP_OK)
        {
            (void)xSemaphoreGive(mutex);
            return err;
        }

        s_poom_web_ir_session.transmitter_ready = true;
    }

    command = &s_poom_web_ir_session.commands[index];

    esp_err_t send_err =
        ir_tx_send(&s_poom_web_ir_session.transmitter, command->proto, command->address, command->command);

    (void)xSemaphoreGive(mutex);
    return send_err;
#endif
}

esp_err_t poom_web_ir_session_close(const char* token)
{
    SemaphoreHandle_t mutex = NULL;

    if((token == NULL) || (token[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    mutex = poom_web_ir_mutex_get_();
    if(mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if(xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if((!s_poom_web_ir_session.active) || (strcmp(token, s_poom_web_ir_session.token) != 0))
    {
        (void)xSemaphoreGive(mutex);
        return ESP_ERR_NOT_FOUND;
    }

    poom_web_ir_session_reset_locked_();
    (void)xSemaphoreGive(mutex);
    return ESP_OK;
}

void poom_web_ir_session_force_reset(void)
{
    SemaphoreHandle_t mutex = poom_web_ir_mutex_get_();

    if(mutex == NULL)
    {
        return;
    }

    if(xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    poom_web_ir_session_reset_locked_();
    (void)xSemaphoreGive(mutex);
}
