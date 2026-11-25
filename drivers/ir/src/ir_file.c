// SPDX-License-Identifier: MIT
#include "ir_file.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool read_hex(const char* value, uint32_t* result)
{
    *result = 0;
    for(unsigned i = 0; i < 4; ++i)
    {
        if(!isxdigit((unsigned char)value[0]) || !isxdigit((unsigned char)value[1]))
            return false;
        char byte[] = {value[0], value[1], '\0'};
        *result |= (uint32_t)strtoul(byte, NULL, 16) << (i * 8U);
        value += 2;
        if(*value && !isspace((unsigned char)*value)) return false;
        while(isspace((unsigned char)*value)) ++value;
    }
    return *value == '\0';
}

static bool read_timings(const char* value, ir_raw_signal_t* raw)
{
    raw->count = 0;
    while(*value)
    {
        char* end;
        if(!isdigit((unsigned char)*value) || raw->count == IR_RAW_MAX_TIMINGS) return false;
        unsigned long duration = strtoul(value, &end, 10);
        if(!duration || duration > IR_RAW_MAX_TOTAL_US ||
           (*end && !isspace((unsigned char)*end))) return false;
        raw->timings_us[raw->count++] = duration;
        value = end;
        while(isspace((unsigned char)*value)) ++value;
    }
    if(raw->count && raw->count % 2U == 0U) --raw->count;
    return true;
}

static bool read_command(FILE* file, char* line, ir_raw_signal_t* raw,
                         ir_file_command_t* command, bool exact)
{
    enum { NAME = 1, TYPE = 2, PROTOCOL = 4, ADDRESS = 8, COMMAND = 16,
           FREQUENCY = 32, DUTY = 64, DATA = 128 };
    unsigned fields = 0;
    *command = (ir_file_command_t){0};
    bool invalid = false;
    unsigned lines = 0;
    while(true)
    {
        if(exact && ++lines > 12) return false;
        const long offset = ftell(file);
        const bool eof = fgets(line, IR_FILE_LINE_SIZE, file) == NULL;
        const bool next = !eof && strncmp(line, "name:", 5) == 0;
        if(eof || next || line[0] == '#')
        {
            const unsigned required = NAME | TYPE | (command->raw ?
                FREQUENCY | DUTY | DATA : PROTOCOL | ADDRESS | COMMAND);
            if(!invalid && (fields & required) == required &&
               (!command->raw || ir_raw_signal_valid(raw, ir_raw_signal_size(raw))))
            {
                return !next || (offset >= 0 && fseek(file, offset, SEEK_SET) == 0);
            }
            if(eof || (exact && fields)) return false;
            fields = 0;
            invalid = false;
        }

        const size_t length = strlen(line);
        if(exact && !fields && !next) return false;
        if(length == IR_FILE_LINE_SIZE - 1 && line[length - 1] != '\n')
        {
            int c;
            do { c = fgetc(file); } while(c != '\n' && c != EOF);
            if(exact) return false;
            invalid = true;
            continue;
        }

        char* value = strchr(line, ':');
        if(!value) continue;
        *value++ = '\0';
        while(isspace((unsigned char)*value)) ++value;
        char* tail = value + strlen(value);
        while(tail > value && isspace((unsigned char)tail[-1])) *--tail = '\0';

        if(next)
        {
            if(exact && !*value) return false;
            *command = (ir_file_command_t){.offset = offset};
            raw->magic = IR_RAW_MAGIC;
            raw->version = 1U;
            raw->count = 0;
            raw->carrier_hz = 0;
            raw->duty_permille = 0;
            if(*value)
            {
                snprintf(command->name, sizeof(command->name), "%s", value);
                fields = NAME;
            }
        }
        else if(!(fields & NAME)) continue;
        else if(strcmp(line, "type") == 0)
        {
            command->raw = strcasecmp(value, "raw") == 0;
            invalid |= !command->raw && strcasecmp(value, "parsed") != 0;
            fields |= TYPE;
        }
        else if(strcmp(line, "protocol") == 0)
        {
            invalid |= !ir_protocol_parse_name(value, &command->protocol);
            fields |= PROTOCOL;
        }
        else if(strcmp(line, "address") == 0)
        {
            invalid |= !read_hex(value, &command->address);
            fields |= ADDRESS;
        }
        else if(strcmp(line, "command") == 0)
        {
            invalid |= !read_hex(value, &command->command);
            fields |= COMMAND;
        }
        else if(strcmp(line, "frequency") == 0)
        {
            char* end;
            raw->carrier_hz = strtoul(value, &end, 10);
            invalid |= end == value || *end || !isdigit((unsigned char)*value);
            fields |= FREQUENCY;
        }
        else if(strcmp(line, "duty_cycle") == 0)
        {
            char* end;
            float duty = strtof(value, &end);
            if(end == value || *end || !(duty > 0.0f && duty < 1.0f)) invalid = true;
            else raw->duty_permille = (uint32_t)(duty * 1000.0f + 0.5f);
            fields |= DUTY;
        }
        else if(strcmp(line, "data") == 0)
        {
            invalid |= !read_timings(value, raw);
            fields |= DATA;
        }
    }
}

bool ir_file_read(FILE* file, char* line, ir_raw_signal_t* raw, ir_file_command_t* command)
{
    return read_command(file, line, raw, command, false);
}

bool ir_file_read_at(FILE* file, char* line, ir_raw_signal_t* raw,
                     ir_file_command_t* command, long offset)
{
    return offset >= 0 && fseek(file, offset, SEEK_SET) == 0 &&
           read_command(file, line, raw, command, true) && command->offset == offset;
}

bool ir_file_command_valid(const ir_file_command_t* command)
{
    static const struct { uint8_t address; uint8_t command; } widths[] = {
        [IR_PROTOCOL_NEC] = {8, 8},
        [IR_PROTOCOL_NEC_EXT] = {16, 16},
        [IR_PROTOCOL_SAMSUNG32] = {8, 8},
        [IR_PROTOCOL_SIRC] = {5, 7},
        [IR_PROTOCOL_SIRC15] = {8, 7},
        [IR_PROTOCOL_SIRC20] = {13, 7},
        [IR_PROTOCOL_RC5] = {5, 6},
        [IR_PROTOCOL_RC5X] = {5, 7},
        [IR_PROTOCOL_RC6] = {8, 8},
        [IR_PROTOCOL_RCA] = {4, 8},
        [IR_PROTOCOL_PIONEER] = {8, 8},
        [IR_PROTOCOL_KASEIKYO] = {26, 10},
        [IR_PROTOCOL_NEC42] = {13, 8},
        [IR_PROTOCOL_NEC42_EXT] = {26, 16},
    };
    if(command->raw) return true; /* The parser already checked the waveform. */
    const unsigned protocol = (unsigned)command->protocol;
    return protocol < sizeof(widths) / sizeof(widths[0]) && widths[protocol].address &&
           !(command->address >> widths[protocol].address) &&
           !(command->command >> widths[protocol].command);
}

esp_err_t ir_file_send(ir_tx_handle_t* tx, const ir_file_command_t* command,
                       const ir_raw_signal_t* raw, bool full_nec_ext)
{
    if(command->raw)
        return raw ? ir_tx_send_raw(tx, raw, ir_raw_signal_size(raw)) : ESP_ERR_INVALID_ARG;
    if(full_nec_ext && !ir_file_command_valid(command)) return ESP_ERR_INVALID_ARG;
    return ir_tx_send_press(tx, command->protocol, command->address, command->command, full_nec_ext);
}
