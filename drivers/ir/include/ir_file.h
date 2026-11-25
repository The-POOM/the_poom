// SPDX-License-Identifier: MIT
#pragma once

#include <stdio.h>

#include "ir_tx.h"

#define IR_FILE_NAME_SIZE 21U
#define IR_FILE_LINE_SIZE (8U * IR_RAW_MAX_TIMINGS + 16U)
#define IR_FILE_RAW_SIZE (sizeof(ir_raw_signal_t) + IR_RAW_MAX_TIMINGS * sizeof(uint32_t))

typedef struct
{
    char name[IR_FILE_NAME_SIZE];
    long offset;
    ir_protocol_t protocol;
    uint32_t address;
    uint32_t command;
    bool raw;
} ir_file_command_t;

/* Scratch buffers are caller-owned. Returns the next valid command, skipping
 * malformed or unsupported records. RAW data remains valid until the next read. */
bool ir_file_read(FILE* file, char* line, ir_raw_signal_t* raw, ir_file_command_t* command);
bool ir_file_read_at(FILE* file, char* line, ir_raw_signal_t* raw,
                     ir_file_command_t* command, long offset);
/* Checks canonical Flipper payload widths. Legacy POOM files may use
 * driver-specific encodings and do not use this check. */
bool ir_file_command_valid(const ir_file_command_t* command);
esp_err_t ir_file_send(ir_tx_handle_t* tx, const ir_file_command_t* command,
                       const ir_raw_signal_t* raw, bool full_nec_ext);
