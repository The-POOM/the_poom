// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ir_identify.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "ir_file.h"
#include "poom_sbus.h"
#include "poom_ui_keyboard.h"
#include "sd_card.h"

#define IDENTIFY_DIR SD_CARD_PATH "/ir/identify"
#define REMOTES_DIR SD_CARD_PATH "/ir/remotes"
#define MAX_SIGNALS 4096U
#define MAX_COMMANDS 255U
#define RECENT_COUNT 4U
#define PRESS_DOWN 0U
#define PRESS_UP 1U
#define HOLD_FLAG 0x80U
#define PROFILE_DOWN 0x20U
#define PROFILE_UP 0x40U
#define MAX_PROFILES 4096U
#define HOLD_MS 2000U
#define REPAIR_BATCH 3U
#define NO_BUTTON UINT8_MAX
#define INDEX_MAGIC "PID1"
#define PROFILE_MARKER "# POOM IR IDENTIFY\n"

static void line_at(unsigned row, const char* text)
{
    poom_arduboy_set_cursor(0, (int16_t)row);
    poom_arduboy_print(text);
}

static void draw_header(const char* title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    line_at(2, title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void resume_menu(void)
{
    const uint8_t token = MENU_IR_IDENTIFY_RESUME_TOKEN;
    poom_sbus_publish("poom/menu/resume", &token, sizeof(token), pdMS_TO_TICKS(100));
}

#if defined(PIN_NUM_IR_TX)

/* The source index is little-endian. ESP32-C5 and other supported targets
 * are little-endian, and the assertions keep its layout independent of padding. */
typedef struct
{
    char magic[4];
    uint32_t signals;
    uint32_t profiles;
} identify_header_t;

typedef struct
{
    uint32_t signal_offset;
    uint32_t profiles_offset;
    uint32_t profiles;
} identify_signal_t;

typedef struct
{
    uint32_t id;
    uint8_t hint;
    uint8_t reserved[3];
    char label[64];
} identify_profile_t;

_Static_assert(sizeof(identify_header_t) == 12, "Invalid index header layout");
_Static_assert(sizeof(identify_signal_t) == 12, "Invalid signal index layout");
_Static_assert(sizeof(identify_profile_t) == 72, "Invalid profile index layout");

typedef enum
{
    SEARCH_NONE,
    SEARCH_FOUND,
    SEARCH_EXIT,
    SEARCH_TX_ERROR,
} search_result_t;

typedef struct
{
    FILE* database;
    uint32_t file_size;
    identify_header_t header;
    ir_tx_handle_t tx;
    char* line;
    ir_raw_signal_t* raw;
    TickType_t gap;
    uint32_t match;
    unsigned invalid;
    unsigned other_count;
    uint8_t other_hits[(MAX_SIGNALS + 7U) / 8U];
    bool power_on;
} identify_t;

typedef enum
{
    COMMAND_UNTESTED,
    COMMAND_MATCH,
    COMMAND_WRONG_ACTION,
    COMMAND_NO_RESPONSE,
} command_check_t;

typedef struct
{
    long offset;
    char name[IR_FILE_NAME_SIZE];
    uint8_t check;
    char actual[IR_FILE_NAME_SIZE];
} profile_command_t;

/* The bus can retain a copy of a callback after unsubscribe. Its queue
 * therefore has static lifetime and is reset on the next launch. */
static StaticQueue_t s_button_storage;
static uint8_t s_button_data[8];
static QueueHandle_t s_buttons;
static atomic_bool s_profile_controls;
static void button_callback(const poom_sbus_msg_t* msg, void* context)
{
    if(msg->len < sizeof(button_event_msg_t)) return;
    button_event_msg_t event;
    memcpy(&event, msg->data, sizeof(event));
    const bool hold_key = event.button == BUTTON_A || event.button == BUTTON_LEFT ||
                          event.button == BUTTON_RIGHT;
    if(hold_key && atomic_load(&s_profile_controls))
    {
        if(event.event == PRESS_DOWN || event.event == PRESS_UP)
        {
            const uint8_t key = event.button | (event.event == PRESS_DOWN ? PROFILE_DOWN : PROFILE_UP);
            xQueueSend(context, &key, 0);
        }
        return;
    }
    if(event.event != PRESS_DOWN) return;
    if(event.button == BUTTON_B)
    {
        if(xQueueSendToFront(context, &event.button, 0) != pdPASS)
        {
            xQueueReset(context);
            xQueueSendToFront(context, &event.button, 0);
        }
    }
    else xQueueSend(context, &event.button, 0);
}

static uint8_t button(TickType_t timeout)
{
    uint8_t value;
    return xQueueReceive(s_buttons, &value, timeout) == pdPASS ? value : NO_BUTTON;
}

static uint8_t profile_button(void)
{
    uint8_t held = NO_BUTTON;
    TickType_t started = 0;
    const TickType_t duration = pdMS_TO_TICKS(HOLD_MS);
    while(true)
    {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t wait = held == NO_BUTTON ? portMAX_DELAY :
                                elapsed < duration ? duration - elapsed : 0;
        const uint8_t key = button(wait);
        if(key == NO_BUTTON)
        {
            if(held != NO_BUTTON) return held | HOLD_FLAG;
            continue;
        }
        if(key < PROFILE_DOWN) return key;
        if(key & PROFILE_DOWN)
        {
            held = key & ~PROFILE_DOWN;
            started = xTaskGetTickCount();
        }
        else if((key & ~PROFILE_UP) == held)
        {
            return held | (xTaskGetTickCount() - started >= duration ? HOLD_FLAG : 0);
        }
    }
}

static void message(const char* title, const char* text, const char* footer)
{
    draw_header(title);
    line_at(22, text);
    line_at(56, footer);
    poom_arduboy_display();
}

static int choose(const char* title, const char* const* entries, unsigned count, unsigned selected)
{
    while(true)
    {
        draw_header(title);
        for(unsigned i = 0; i < count; ++i)
        {
            const int16_t row = (int16_t)(16 + (count == 4 ? 10 : 12) * i);
            line_at(row, i == selected ? ">" : " ");
            poom_arduboy_set_cursor(12, row);
            poom_arduboy_print(entries[i]);
        }
        line_at(56, "A:SELECT  B:BACK");
        poom_arduboy_display();

        switch(button(portMAX_DELAY))
        {
            case BUTTON_UP: selected = (selected + count - 1) % count; break;
            case BUTTON_DOWN: selected = (selected + 1) % count; break;
            case BUTTON_A: return (int)selected;
            case BUTTON_B: return -1;
            default: break;
        }
    }
}

static bool open_database(identify_t* app)
{
    app->database = fopen(app->power_on ? IDENTIFY_DIR "/power.idx" : IDENTIFY_DIR "/tv.idx", "rb");
    if(!app->database) return false;
    if(fseek(app->database, 0, SEEK_END) != 0) return false;
    const long bytes = ftell(app->database);
    if(bytes < (long)sizeof(app->header) || fseek(app->database, 0, SEEK_SET) != 0)
        return false;
    app->file_size = (uint32_t)bytes;
    return fread(&app->header, sizeof(app->header), 1, app->database) == 1 &&
           memcmp(app->header.magic, INDEX_MAGIC, sizeof(app->header.magic)) == 0 &&
           app->header.signals <= MAX_SIGNALS &&
           app->header.signals <= (app->file_size - sizeof(app->header)) / sizeof(identify_signal_t) &&
           app->header.profiles <= MAX_PROFILES &&
           (!app->header.signals || app->header.profiles);
}

static bool read_signal(const identify_t* app, uint32_t index, identify_signal_t* signal)
{
    const uint32_t start = sizeof(app->header) + app->header.signals * sizeof(*signal);
    return index < app->header.signals &&
           fseek(app->database, (long)(sizeof(app->header) + index * sizeof(*signal)), SEEK_SET) == 0 &&
           fread(signal, sizeof(*signal), 1, app->database) == 1 &&
           signal->signal_offset < app->file_size &&
           signal->profiles_offset >= start && signal->profiles_offset < signal->signal_offset &&
           signal->profiles > 0 && signal->profiles <= app->header.profiles &&
           (signal->signal_offset - signal->profiles_offset) / sizeof(identify_profile_t) == signal->profiles &&
           (signal->signal_offset - signal->profiles_offset) % sizeof(identify_profile_t) == 0;
}

static bool load_candidate(identify_t* app, uint32_t index, ir_file_command_t* cmd)
{
    identify_signal_t signal;
    return read_signal(app, index, &signal) &&
           ir_file_read_at(app->database, app->line, app->raw, cmd, signal.signal_offset) &&
           strcmp(cmd->name, app->power_on ? "Power On" : "Mute") == 0 && ir_file_command_valid(cmd);
}

static void draw_progress(bool power_on, unsigned candidates, unsigned group, unsigned done, unsigned total)
{
    char text[32];
    draw_header(power_on ? "POWER ON SEARCH" : "IR DISCOVERY");
    snprintf(text, sizeof(text), "Candidates: %u", candidates);
    line_at(14, text);
    snprintf(text, sizeof(text), "Group %u/3", group + 1);
    line_at(24, text);
    snprintf(text, sizeof(text), "Testing %u/%u", done, total);
    line_at(34, text);
    poom_arduboy_draw_rect(0, 44, 128, 7, WHITE);
    poom_arduboy_fill_rect(1, 45, (int16_t)(126 * done / total), 5, WHITE);
    line_at(56, "A:HIT         B:STOP");
    poom_arduboy_display();
}

/* Stop at the first observed button event in the gap, favouring cancellation
 * if both events arrived during one physical press. */
static uint8_t stop_button(TickType_t timeout, bool can_hit)
{
    uint8_t result = NO_BUTTON;
    const TickType_t began = xTaskGetTickCount();
    const TickType_t duration = timeout;
    uint8_t key;
    while((key = button(timeout)) != NO_BUTTON)
    {
        if(key == BUTTON_B) return key;
        if(key == BUTTON_A && can_hit)
        {
            result = key;
            timeout = 0;
        }
        if(timeout)
        {
            const TickType_t elapsed = xTaskGetTickCount() - began;
            timeout = elapsed < duration ? duration - elapsed : 0;
        }
    }
    return result;
}

enum { RESPONSE_YES, RESPONSE_REPEAT, RESPONSE_NO, RESPONSE_OTHER, RESPONSE_COUNT };

static const char* const response_options[RESPONSE_COUNT] = {
    [RESPONSE_YES] = "YES",
    [RESPONSE_REPEAT] = "REPEAT",
    [RESPONSE_NO] = "NO",
    [RESPONSE_OTHER] = "YES (Other Signal)",
};

static int ask_response(const identify_t* app)
{
    return choose(app->power_on ? "DID TV TURN ON?" : "ANY TV RESPONSE?",
                  response_options, app->power_on ? RESPONSE_OTHER : RESPONSE_COUNT,
                  RESPONSE_REPEAT);
}

static bool has_other_hit(const identify_t* app, uint32_t index)
{
    return (app->other_hits[index / 8U] & (1U << (index % 8U))) != 0;
}

static search_result_t verify_candidate(identify_t* app, uint32_t index, unsigned candidate, unsigned count)
{
    ir_file_command_t cmd;
    if(!load_candidate(app, index, &cmd))
    {
        ++app->invalid;
        return SEARCH_NONE;
    }

    char details[22];
    snprintf(details, sizeof(details), "%u/%u %s", candidate + 1, count,
             cmd.raw ? "RAW" : ir_protocol_name(cmd.protocol));
    while(true)
    {
        if(app->power_on)
        {
            draw_header("VERIFY POWER ON");
            line_at(16, details);
            line_at(32, "TV must be off");
            line_at(56, "A:SEND        B:STOP");
            poom_arduboy_display();
        }
        else message("VERIFY MUTE", details, "A:SEND        B:STOP");
        uint8_t key;
        do { key = button(portMAX_DELAY); } while(key != BUTTON_A && key != BUTTON_B);
        if(key == BUTTON_B) return SEARCH_EXIT;
        if(ir_file_send(&app->tx, &cmd, app->raw, true) != ESP_OK) return SEARCH_TX_ERROR;
        const int answer = ask_response(app);
        if(answer < 0) return SEARCH_EXIT;
        if(answer == RESPONSE_NO) return SEARCH_NONE;
        if(answer == RESPONSE_OTHER)
        {
            if(!has_other_hit(app, index))
            {
                app->other_hits[index / 8U] |= 1U << (index % 8U);
                ++app->other_count;
            }
            return SEARCH_NONE;
        }
        if(answer == RESPONSE_YES)
        {
            app->match = index;
            return SEARCH_FOUND;
        }
    }
}

/* Each call owns a contiguous range. The only recursive ranges are a
 * subgroup's tested prefix and its untested suffix, each strictly smaller.
 * The caller keeps the remaining siblings while a positive branch is tried. */
static search_result_t search_range(identify_t* app, uint32_t first, unsigned count, bool recent_first)
{
    if(count <= 3)
    {
        for(unsigned step = 0; step < count; ++step)
        {
            const unsigned candidate = recent_first ? count - 1 - step : step;
            const search_result_t result = verify_candidate(app, first + candidate, step, count);
            if(result != SEARCH_NONE) return result;
        }
        return SEARCH_NONE;
    }

    for(unsigned step = 0; step < 3; ++step)
    {
        const unsigned group = recent_first ? 2 - step : step;
        const uint32_t start = first + group * (count / 3) + (group < count % 3 ? group : count % 3);
        const unsigned size = count / 3 + (group < count % 3);
        unsigned processed;
        unsigned recent_length;
        uint32_t recent[RECENT_COUNT];
        bool hit;
        int answer;

        do
        {
            if(app->power_on)
            {
                message("POWER ON SEARCH", "TV in standby?", "A:START       B:STOP");
                uint8_t key;
                do { key = button(portMAX_DELAY); } while(key != BUTTON_A && key != BUTTON_B);
                if(key == BUTTON_B) return SEARCH_EXIT;
            }
            processed = 0;
            recent_length = 0;
            hit = false;
            draw_progress(app->power_on, count, group, 0, size);
            while(processed < size && !hit)
            {
                ir_file_command_t cmd;
                const bool valid = load_candidate(app, start + processed, &cmd);
                uint8_t key = stop_button(0, recent_length != 0);
                if(key == BUTTON_B) return SEARCH_EXIT;
                if(key == BUTTON_A) { hit = true; break; }
                if(valid)
                {
                    if(ir_file_send(&app->tx, &cmd, app->raw, true) != ESP_OK)
                        return SEARCH_TX_ERROR;
                    recent[recent_length++ % RECENT_COUNT] = start + processed;
                }
                else ++app->invalid;
                ++processed;
                draw_progress(app->power_on, count, group, processed, size);
                if(valid)
                {
                    key = stop_button(app->gap, true);
                    if(key == BUTTON_B) return SEARCH_EXIT;
                    hit = key == BUTTON_A;
                }
            }
            if(!recent_length) { answer = RESPONSE_NO; break; }
            answer = hit && app->power_on ? RESPONSE_YES : ask_response(app);
            if(answer < 0) return SEARCH_EXIT;
        } while(answer == RESPONSE_REPEAT);

        if(answer == RESPONSE_YES || answer == RESPONSE_OTHER)
        {
            /* On HIT, try the most recent part first. Every candidate in the
             * tested prefix still gets the same individual confirmation. */
            const bool prioritize_recent = hit &&
                recent[(recent_length > RECENT_COUNT ? recent_length - RECENT_COUNT : 0) %
                       RECENT_COUNT] >= start + processed / 3;
            search_result_t result = search_range(app, start, processed, prioritize_recent);
            if(result != SEARCH_NONE) return result;
        }
        if(processed < size)
        {
            const search_result_t result = search_range(app, start + processed, size - processed, false);
            if(result != SEARCH_NONE) return result;
        }
    }
    return SEARCH_NONE;
}

static bool save_profile(identify_t* app, FILE* source, const char* name)
{
    char filename[21];
    char title[sizeof(filename)];
    unsigned i;
    for(i = 0; name[i] && i + 1 < sizeof(filename); ++i)
    {
        const unsigned char c = (unsigned char)name[i];
        filename[i] = isalnum(c) || c == '_' || c == '-' ? c : '_';
        title[i] = isprint(c) ? c : '_';
    }
    filename[i] = '\0';
    title[i] = '\0';
    if(!i || sd_card_create_dir("/ir/remotes") != ESP_OK) return false;

    char destination[sizeof(REMOTES_DIR) + sizeof(filename) + 4];
    char temporary[sizeof(destination)];
    snprintf(destination, sizeof(destination), REMOTES_DIR "/%s.ir", filename);
    snprintf(temporary, sizeof(temporary), REMOTES_DIR "/%s.tmp", filename);
    struct stat info;
    if(stat(destination, &info) == 0 || fseek(source, 0, SEEK_SET) != 0) return false;

    const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(fd < 0) return false;
    FILE* out = fdopen(fd, "wb");
    if(!out)
    {
        close(fd);
        unlink(temporary);
        return false;
    }
    char* buffer = app->line;
    /* Replace the first comment with the user label for the IR UNIV browser. */
    bool ok = fgets(buffer, IR_FILE_LINE_SIZE, source) &&
              strcmp(buffer, "Filetype: IR signals file\n") == 0 &&
              fgets(buffer, IR_FILE_LINE_SIZE, source) && strcmp(buffer, "Version: 1\n") == 0 &&
              fgets(buffer, IR_FILE_LINE_SIZE, source) && buffer[0] == '#' &&
              fgets(buffer, IR_FILE_LINE_SIZE, source) && strcmp(buffer, PROFILE_MARKER) == 0 &&
              fprintf(out, "Filetype: IR signals file\nVersion: 1\n# %s\n" PROFILE_MARKER,
                      title) > 0;
    while(ok)
    {
        const size_t n = fread(buffer, 1, IR_FILE_LINE_SIZE, source);
        ok = fwrite(buffer, 1, n, out) == n && !ferror(source);
        if(n < IR_FILE_LINE_SIZE) break;
    }
    if(fclose(out) != 0) ok = false;
    if(ok && rename(temporary, destination) == 0) return true;
    unlink(temporary);
    return false;
}

static void prompt_save(identify_t* app, FILE* source)
{
    poom_ui_keyboard_t keyboard;
    char name[21] = "";
    poom_ui_keyboard_init(&keyboard, name, sizeof(name), "REMOTE NAME");
    while(true)
    {
        poom_ui_keyboard_draw(&keyboard);
        const uint8_t key = button(portMAX_DELAY);
        if(key == BUTTON_B) return;
        if(poom_ui_keyboard_handle_button(&keyboard, key) == POOM_UI_KEYBOARD_ACTION_ACCEPT && *name)
        {
            const bool saved = save_profile(app, source, name);
            if(saved)
            {
                draw_header("REMOTE SAVED");
                line_at(20, "Saved to IR UNIVERSAL");
                line_at(33, "> Flipper File");
                line_at(56, "A:OK        B:BACK");
                poom_arduboy_display();
            }
            else message("SAVE FAILED", "Check name or SD", "A:OK        B:BACK");
            uint8_t dismiss;
            do { dismiss = button(portMAX_DELAY); } while(dismiss != BUTTON_A && dismiss != BUTTON_B);
            if(saved || dismiss == BUTTON_B) return;
        }
    }
}

static void action_key(char* key, const char* name)
{
    size_t n = 0;
    for(; *name && n + 1 < IR_FILE_NAME_SIZE; ++name)
    {
        const unsigned char c = (unsigned char)*name;
        if(isalnum(c)) key[n++] = (char)tolower(c);
    }
    key[n] = '\0';
    if(strcmp(key, "sleeptimer") == 0) strcpy(key, "sleep");
    if(strcmp(key, "enter") == 0 || strcmp(key, "select") == 0) strcpy(key, "ok");
}

static bool same_action(const char* a, const char* b)
{
    char left[IR_FILE_NAME_SIZE];
    char right[IR_FILE_NAME_SIZE];
    action_key(left, a);
    action_key(right, b);
    return left[0] && strcmp(left, right) == 0;
}

static bool pick_actual_action(char* action)
{
    const char* const options[] = {
        "Mute", "Volume Up", "Volume Down", "Menu", "Sleep Timer",
        "OK", "Play/Pause", "Play", "Pause", "Input", "Home", "Back",
        "Channel Up", "Channel Down", "Power", "Power On", "Power Off", "Type action"
    };
    const unsigned count = sizeof(options) / sizeof(options[0]);
    unsigned selected = 0;
    while(true)
    {
        draw_header("ACTUAL TV ACTION");
        for(unsigned i = 0; i < 3; ++i)
        {
            const unsigned option = (selected + count + i - 1) % count;
            const int16_t row = (int16_t)(16 + 12 * i);
            line_at(row, i == 1 ? ">" : " ");
            poom_arduboy_set_cursor(12, row);
            poom_arduboy_print(options[option]);
        }
        line_at(56, "A:SELECT     B:BACK");
        poom_arduboy_display();
        switch(button(portMAX_DELAY))
        {
            case BUTTON_UP: selected = (selected + count - 1) % count; break;
            case BUTTON_DOWN: selected = (selected + 1) % count; break;
            case BUTTON_B: return false;
            case BUTTON_A:
                if(selected != count - 1)
                {
                    snprintf(action, IR_FILE_NAME_SIZE, "%s", options[selected]);
                    return true;
                }
                poom_ui_keyboard_t keyboard;
                poom_ui_keyboard_init(&keyboard, action, IR_FILE_NAME_SIZE, "TV ACTION");
                while(true)
                {
                    poom_ui_keyboard_draw(&keyboard);
                    const uint8_t key = button(portMAX_DELAY);
                    if(key == BUTTON_B) return false;
                    if(poom_ui_keyboard_handle_button(&keyboard, key) == POOM_UI_KEYBOARD_ACTION_ACCEPT)
                    {
                        char normalized[IR_FILE_NAME_SIZE];
                        action_key(normalized, action);
                        if(normalized[0]) return true;
                    }
                }
            default: break;
        }
    }
}

typedef struct
{
    ir_file_command_t signal;
    ir_raw_signal_t* raw;
    const char* action;
} observed_signal_t;

static void normalize_signal(ir_file_command_t* signal)
{
    if(signal->protocol == IR_PROTOCOL_NEC)
    {
        signal->address |= (signal->address ^ 0xffU) << 8;
        signal->command |= (signal->command ^ 0xffU) << 8;
        signal->protocol = IR_PROTOCOL_NEC_EXT;
    }
    else if(signal->protocol == IR_PROTOCOL_RC5X) signal->command &= 0x3fU;
}

static bool same_signal(const observed_signal_t* observation, const ir_file_command_t* signal,
                        const ir_raw_signal_t* raw)
{
    if(signal->raw != observation->signal.raw) return false;
    if(signal->raw)
    {
        const size_t size = ir_raw_signal_size(raw);
        return size == ir_raw_signal_size(observation->raw) &&
               memcmp(raw, observation->raw, size) == 0;
    }
    return signal->protocol == observation->signal.protocol &&
           signal->address == observation->signal.address &&
           signal->command == observation->signal.command;
}

static FILE* open_profile(uint32_t id)
{
    char path[sizeof(IDENTIFY_DIR) + 24];
    snprintf(path, sizeof(path), IDENTIFY_DIR "/profiles/%08lx.ir", (unsigned long)id);
    return fopen(path, "rb");
}

static uint32_t pick_replacement(identify_t* app, const uint32_t* matches, unsigned count)
{
    unsigned selected = 0;
    while(true)
    {
        char label[65] = "";
        FILE* profile = open_profile(matches[selected]);
        if(profile)
        {
            for(unsigned line = 0; line < 3; ++line)
            {
                if(!fgets(app->line, IR_FILE_LINE_SIZE, profile)) break;
                if(line == 2 && strncmp(app->line, "# ", 2) == 0)
                {
                    app->line[strcspn(app->line, "\r\n")] = '\0';
                    snprintf(label, sizeof(label), "%.64s", app->line + 2);
                }
            }
            fclose(profile);
        }
        draw_header("3-ACTION MATCHES");
        char position[22];
        snprintf(position, sizeof(position), "%u/%u", selected + 1, count);
        line_at(15, position);
        /* Preserve the model suffix when a brand/path label is long. */
        const size_t length = strlen(label);
        const char* visible = label + (length > 42 ? length - 42 : 0);
        snprintf(position, sizeof(position), "%.21s", visible);
        line_at(28, position);
        line_at(40, visible + strlen(position));
        line_at(56, "A:USE        B:BACK");
        poom_arduboy_display();
        switch(button(portMAX_DELAY))
        {
            case BUTTON_UP: selected = (selected + count - 1) % count; break;
            case BUTTON_DOWN: selected = (selected + 1) % count; break;
            case BUTTON_A: return matches[selected];
            case BUTTON_B: return UINT32_MAX;
            default: break;
        }
    }
}

static uint32_t find_replacement(identify_t* app, FILE* source, uint32_t current,
                                 const profile_command_t* commands, unsigned count)
{
    observed_signal_t observed[REPAIR_BATCH] = {0};
    unsigned used = 0;
    uint32_t replacement = UINT32_MAX;
    const char* error = "Need 3 response codes";
    for(unsigned i = 0; i < count && used < REPAIR_BATCH; ++i)
    {
        const profile_command_t* command = &commands[i];
        if(!command->actual[0]) continue;
        ir_file_command_t signal;
        if(!ir_file_read_at(source, app->line, app->raw, &signal, command->offset) ||
           strcmp(signal.name, command->name) != 0)
        {
            error = "Could not read signal";
            goto done;
        }
        normalize_signal(&signal);
        bool duplicate = false;
        for(unsigned j = 0; j < used; ++j) duplicate |= same_signal(&observed[j], &signal, app->raw);
        if(duplicate) continue;
        if(signal.raw)
        {
            const size_t size = ir_raw_signal_size(app->raw);
            observed[used].raw = malloc(size);
            if(!observed[used].raw)
            {
                error = "Out of memory";
                goto done;
            }
            memcpy(observed[used].raw, app->raw, size);
        }
        observed[used].signal = signal;
        observed[used++].action = command->actual;
    }
    if(used != REPAIR_BATCH) goto done;

    uint32_t* matches = malloc(app->header.profiles * sizeof(*matches));
    if(!matches)
    {
        error = "Out of memory";
        goto done;
    }
    unsigned match_count = 0;
    error = NULL;
    for(uint32_t id = 0; id < app->header.profiles; ++id)
    {
        if(button(0) == BUTTON_B) goto finish_search;
        if(id % 16U == 0)
        {
            char text[32];
            snprintf(text, sizeof(text), "Checked %lu/%lu", (unsigned long)id,
                     (unsigned long)app->header.profiles);
            draw_header("SEARCHING SD");
            line_at(20, "Matching 3 actions");
            line_at(34, text);
            line_at(56, "B:CANCEL");
            poom_arduboy_display();
        }
        if(id == current) continue;
        FILE* profile = open_profile(id);
        if(!profile) continue;

        unsigned found = 0;
        ir_file_command_t signal;
        while(ir_file_read(profile, app->line, app->raw, &signal))
        {
            if(!ir_file_command_valid(&signal)) continue;
            normalize_signal(&signal);
            for(unsigned j = 0; j < used; ++j)
            {
                if(!(found & (1U << j)) && same_action(observed[j].action, signal.name) &&
                   same_signal(&observed[j], &signal, app->raw))
                    found |= 1U << j;
            }
            if(found == (1U << used) - 1U)
            {
                matches[match_count++] = id;
                break;
            }
        }
        fclose(profile);
    }
    if(match_count) replacement = pick_replacement(app, matches, match_count);
    else error = "No 3-action match";
finish_search:
    free(matches);
done:
    for(unsigned i = 0; i < used; ++i) free(observed[i].raw);
    if(error)
    {
        message("FIND REMOTE", error, "B:BACK");
        while(button(portMAX_DELAY) != BUTTON_B) {}
    }
    return replacement;
}

static uint32_t use_profile(identify_t* app, const identify_profile_t* profile, bool remapped)
{
    uint32_t replacement = UINT32_MAX;
    FILE* file = open_profile(profile->id);
    profile_command_t* commands = file ? calloc(MAX_COMMANDS, sizeof(*commands)) : NULL;
    unsigned count = 0;
    ir_file_command_t cmd;
    const bool header_ok = file &&
        fgets(app->line, IR_FILE_LINE_SIZE, file) && strcmp(app->line, "Filetype: IR signals file\n") == 0 &&
        fgets(app->line, IR_FILE_LINE_SIZE, file) && strcmp(app->line, "Version: 1\n") == 0 &&
        fgets(app->line, IR_FILE_LINE_SIZE, file) && app->line[0] == '#' &&
        fgets(app->line, IR_FILE_LINE_SIZE, file) && strcmp(app->line, PROFILE_MARKER) == 0;
    while(header_ok && commands && count < MAX_COMMANDS && ir_file_read(file, app->line, app->raw, &cmd))
    {
        if(button(0) == BUTTON_B) goto cleanup;
        if(!ir_file_command_valid(&cmd)) continue;
        commands[count].offset = cmd.offset;
        memcpy(commands[count++].name, cmd.name, sizeof(cmd.name));
    }
    if(!count || ferror(file))
    {
        message("PROFILE UNAVAILABLE", "Check the SD files", "B:BACK");
        while(button(portMAX_DELAY) != BUTTON_B) {}
        goto cleanup;
    }

    const char* const verify[] = {"CORRECT ACTION", "WRONG ACTION", "NO RESPONSE"};
    const char* const status[] = {"Untested", "Confirmed", "Wrong action", "No response"};
    const unsigned hint = profile->hint;
    unsigned selected = hint < count ? hint : 0;
    while(true)
    {
        atomic_store(&s_profile_controls, true);
        unsigned checked = 0;
        bool mismatch = false;
        for(unsigned i = 0; i < count; ++i)
        {
            const profile_command_t* command = &commands[i];
            checked += command->actual[0] != 0;
            mismatch |= command->check == COMMAND_WRONG_ACTION || command->check == COMMAND_NO_RESPONSE;
        }
        draw_header(remapped ? "REMAPPED REMOTE" : "COMPATIBLE REMOTE");
        const profile_command_t* command = &commands[selected];
        char text[32];
        line_at(23, command->name);
        snprintf(text, sizeof(text), "%u/%u  %s", selected + 1, count,
                 command->check == COMMAND_WRONG_ACTION ? command->actual :
                 command->check == COMMAND_UNTESTED && selected == hint
                     ? "Try first" : status[command->check]);
        line_at(37, text);

        if(checked < REPAIR_BATCH)
            snprintf(text, sizeof(text), "Hold L:check %u/3", checked);
        else
            snprintf(text, sizeof(text), "Hold L:check R:find");
        line_at(47, text);
        line_at(56, "A:SEND  Hold A:Save");
        poom_arduboy_display();

        const uint8_t key = profile_button();
        atomic_store(&s_profile_controls, false);
        switch(key)
        {
            case BUTTON_UP: selected = (selected + count - 1) % count; break;
            case BUTTON_DOWN: selected = (selected + 1) % count; break;
            case BUTTON_LEFT | HOLD_FLAG:
            {
                const int result = choose(command->name, verify, 3, 2);
                profile_command_t* edited = &commands[selected];
                if(result == 1)
                {
                    char action[IR_FILE_NAME_SIZE] = "";
                    if(!pick_actual_action(action)) break;
                    if(!same_action(action, edited->name))
                    {
                        memcpy(edited->actual, action, sizeof(action));
                        edited->check = COMMAND_WRONG_ACTION;
                        break;
                    }
                }
                if(result == 0 || result == 1)
                    memcpy(edited->actual, edited->name, sizeof(edited->actual));
                else if(result == 2) edited->actual[0] = '\0';
                if(result >= 0) edited->check = result == 2 ? COMMAND_NO_RESPONSE : COMMAND_MATCH;
                break;
            }
            case BUTTON_RIGHT | HOLD_FLAG:
                replacement = find_replacement(app, file, profile->id, commands, count);
                if(replacement != UINT32_MAX) goto cleanup;
                break;
            case BUTTON_A | HOLD_FLAG:
                {
                    const char* const save[] = {"KEEP TESTING", "SAVE FULL PROFILE"};
                    if(!mismatch || choose("SOME BUTTONS FAILED", save, 2, 0) == 1)
                        prompt_save(app, file);
                }
                break;
            case BUTTON_A:
            {
                esp_err_t send = ESP_ERR_INVALID_RESPONSE;
                if(ir_file_read_at(file, app->line, app->raw, &cmd, command->offset) &&
                        strcmp(cmd.name, command->name) == 0)
                    send = ir_file_send(&app->tx, &cmd, app->raw, true);
                if(send != ESP_OK)
                {
                    message("SEND FAILED", "Check signal or SD", "A:OK        B:BACK");
                    uint8_t key;
                    do { key = button(portMAX_DELAY); } while(key != BUTTON_A && key != BUTTON_B);
                    if(key == BUTTON_B) goto cleanup;
                }
                break;
            }
            case BUTTON_B: goto cleanup;
            default: break;
        }
    }
cleanup:
    free(commands);
    if(file) fclose(file);
    return replacement;
}

static void browse_profiles(identify_t* app, uint32_t index, bool other)
{
    identify_signal_t signal;
    if(!read_signal(app, index, &signal)) return;
    uint32_t selected = 0;
    uint32_t scroll = 0;
    while(true)
    {
        identify_profile_t profile;
        const bool valid = fseek(app->database,
                                 (long)(signal.profiles_offset + selected * sizeof(profile)), SEEK_SET) == 0 &&
                           fread(&profile, sizeof(profile), 1, app->database) == 1 &&
                           profile.id < app->header.profiles &&
                           memchr(profile.label, '\0', sizeof(profile.label)) != NULL;
        if(valid)
        {
            char text[22];
            draw_header(other ? "RESPONDING PROFILES" : "COMPATIBLE PROFILES");
            snprintf(text, sizeof(text), "%lu/%lu", (unsigned long)selected + 1,
                     (unsigned long)signal.profiles);
            line_at(15, text);
            const char* label = profile.label + (scroll < strlen(profile.label) ? scroll : 0);
            snprintf(text, sizeof(text), "%.21s", label);
            line_at(28, text);
            snprintf(text, sizeof(text), "%.21s", label + strlen(text));
            line_at(39, text);
            line_at(56, "A:OPEN        B:BACK");
            poom_arduboy_display();
        }
        else message("BAD PROFILE ENTRY", "Select another", "B:BACK");
        switch(button(portMAX_DELAY))
        {
            case BUTTON_UP: selected = (selected + signal.profiles - 1) % signal.profiles; scroll = 0; break;
            case BUTTON_DOWN: selected = (selected + 1) % signal.profiles; scroll = 0; break;
            case BUTTON_LEFT: if(scroll) --scroll; break;
            case BUTTON_RIGHT: if(valid && scroll + 42 < strlen(profile.label)) ++scroll; break;
            case BUTTON_A:
                if(valid)
                {
                    /* Leave both slots available to the source and destination when saving. */
                    fclose(app->database);
                    app->database = NULL;
                    bool remapped = false;
                    uint32_t replacement;
                    while((replacement = use_profile(app, &profile, remapped)) != UINT32_MAX)
                    {
                        profile.id = replacement;
                        profile.hint = 0;
                        remapped = true;
                    }
                    if(!open_database(app))
                    {
                        if(app->database) fclose(app->database);
                        app->database = NULL;
                        message("IR DISCOVERY", "SD index unavailable", "B:BACK");
                        while(button(portMAX_DELAY) != BUTTON_B) {}
                        return;
                    }
                    if(!read_signal(app, index, &signal)) return;
                    if(selected >= signal.profiles)
                    {
                        selected = 0;
                        scroll = 0;
                    }
                }
                break;
            case BUTTON_B: return;
            default: break;
        }
    }
}

static void browse_other_hits(identify_t* app)
{
    uint32_t index = 0;
    while(index < app->header.signals && !has_other_hit(app, index)) ++index;
    if(index == app->header.signals) return;

    unsigned selected = 0;
    while(true)
    {
        char details[22];
        snprintf(details, sizeof(details), "Hit %u/%u", selected + 1, app->other_count);
        message("OTHER SIGNAL HITS", details, "A:OPEN        B:BACK");
        const uint8_t key = button(portMAX_DELAY);
        if(key == BUTTON_B) return;
        if(key == BUTTON_A)
        {
            browse_profiles(app, index, true);
            if(!app->database || index >= app->header.signals) return;
        }
        else if(key == BUTTON_UP || key == BUTTON_DOWN)
        {
            const uint32_t total = app->header.signals;
            const uint32_t step = key == BUTTON_UP ? total - 1 : 1;
            const unsigned ordinal = key == BUTTON_UP ? app->other_count - 1 : 1;
            do { index = (index + step) % total; } while(!has_other_hit(app, index));
            selected = (selected + ordinal) % app->other_count;
        }
    }
}

static void run_identify(identify_t* app)
{
    const char* error = NULL;
    if(!app->line || !app->raw) error = "Out of memory";
    else if(sd_card_mount() != ESP_OK) error = "SD unavailable";
    else
    {
        const char* const modes[] = {"Mute", "Power On"};
        const int mode = choose("IR DISCOVERY", modes, 2, 0);
        if(mode < 0) return;

        app->power_on = mode == 1;
        if(!open_database(app))
            error = app->power_on ? "Update SD database" : "Bad/missing index";
        else if(!app->header.signals)
            error = app->power_on ? "No Power On signals" : "No Mute signals";
    }

    if(error)
    {
        message("IR DISCOVERY", error, "B:BACK");
        while(button(portMAX_DELAY) != BUTTON_B) {}
        return;
    }

    ir_tx_config_t config = ir_tx_default_config();
    config.gpio = PIN_NUM_IR_TX;
    if(ir_tx_init(&app->tx, &config, "ir_identify") != ESP_OK)
    {
        message("IR DISCOVERY", "IR TX busy or failed", "B:BACK");
        while(button(portMAX_DELAY) != BUTTON_B) {}
        return;
    }

    /* A running TV ignores a discrete Power On, and a HIT confirms every
     * candidate in the tested prefix individually. The gap only has to keep
     * the responsible signal within the last RECENT_COUNT sends while the
     * standby light changes and the user reacts. */
    const unsigned normal_gap_ms = app->power_on ? 750U : 350U;
    const unsigned slow_gap_ms = app->power_on ? 1500U : 800U;
    app->gap = pdMS_TO_TICKS(normal_gap_ms);
    const char* const retry[] = {"SLOW RETRY", "RESTART", "VIEW OTHER HITS"};
    while(true)
    {
        app->invalid = 0;
        const search_result_t result = search_range(app, 0, app->header.signals, false);
        if(result == SEARCH_FOUND)
        {
            browse_profiles(app, app->match, false);
            if(app->database && app->other_count)
            {
                const char* const done[] = {"DONE", "VIEW OTHER HITS"};
                if(choose("MUTE CONFIRMED", done, 2, 0) == 1) browse_other_hits(app);
            }
        }
        if(result == SEARCH_TX_ERROR)
        {
            message("IR DISCOVERY", "IR transmission fail", "B:BACK");
            while(button(portMAX_DELAY) != BUTTON_B) {}
        }
        if(result != SEARCH_NONE) break;
        const char* title = app->invalid ? "NO MATCH / BAD DATA" :
                            app->power_on ? "NO POWER ON MATCH" : "NO MUTE MATCH";
        int action;
        do
        {
            action = choose(title, retry, app->other_count ? 3 : 2, app->other_count ? 2 : 0);
            if(action == 2) browse_other_hits(app);
        } while(action == 2 && app->database);
        if(action < 0 || !app->database) break;
        app->gap = pdMS_TO_TICKS(action == 0 ? slow_gap_ms : normal_gap_ms);
    }
    ir_tx_deinit(&app->tx);
}

static void identify_task(void* arg)
{
    (void)arg;
    if(!s_buttons)
        s_buttons = xQueueCreateStatic(sizeof(s_button_data), sizeof(s_button_data[0]),
                                      s_button_data, &s_button_storage);
    xQueueReset(s_buttons);
    if(poom_sbus_subscribe_cb("input/button", button_callback, s_buttons))
    {
        identify_t app = {.line = malloc(IR_FILE_LINE_SIZE), .raw = malloc(IR_FILE_RAW_SIZE)};
        run_identify(&app);
        if(app.database) fclose(app.database);
        free(app.raw);
        free(app.line);
        poom_sbus_unsubscribe_cb("input/button", button_callback, s_buttons);
    }
    resume_menu();
    vTaskDelete(NULL);
}

void menu_ir_identify_show(void)
{
    if(xTaskCreate(identify_task, "ir_identify", 6144, NULL, 4, NULL) != pdPASS)
    {
        message("IR DISCOVERY", "Task creation failed", "");
        vTaskDelay(pdMS_TO_TICKS(900));
        resume_menu();
    }
}

#else

void menu_ir_identify_show(void)
{
    draw_header("IR DISCOVERY");
    line_at(22, "IR TX unavailable");
    poom_arduboy_display();
    vTaskDelay(pdMS_TO_TICKS(900));
    resume_menu();
}

#endif
