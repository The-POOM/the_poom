// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WEB_IR_SESSION_H
#define POOM_WEB_IR_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_dec.h"

#define POOM_WEB_IR_MAX_COMMANDS (48U)
#define POOM_WEB_IR_NAME_MAX_LEN (20U)

typedef ir_protocol_t poom_web_ir_proto_t;

typedef struct
{
    char name[POOM_WEB_IR_NAME_MAX_LEN + 1U];
    poom_web_ir_proto_t proto;
    uint32_t address;
    uint32_t command;
} poom_web_ir_command_t;

bool poom_web_ir_is_supported_path(const char* path);

const char* poom_web_ir_proto_name(poom_web_ir_proto_t proto);

esp_err_t poom_web_ir_session_open(const char* abs_path,
                                   char* out_token,
                                   size_t out_token_len,
                                   poom_web_ir_command_t* out_commands,
                                   size_t max_commands,
                                   size_t* out_count);

esp_err_t poom_web_ir_session_get_command(const char* token,
                                          size_t index,
                                          poom_web_ir_command_t* out_command);

esp_err_t poom_web_ir_session_send(const char* token, size_t index);

esp_err_t poom_web_ir_session_close(const char* token);

void poom_web_ir_session_force_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WEB_IR_SESSION_H */
