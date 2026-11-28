// SPDX-License-Identifier: MIT
#include "menu_remote_suite.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#define SOURCE_DIR SD_CARD_PATH "/ir/remotes"
#define LAYOUT_DIR SD_CARD_PATH "/ir/suite"
#define SUITE_SLOTS 11U
#define SUITE_A_SLOT 8U
#define SUITE_B_SLOT 9U
#define SUITE_AB_SLOT 10U
#define BACK_HOLD_MS 1000U
#define SAVE_HOLD_MS 1000U
#define MAPPING_SAVE UINT8_MAX
#define MAX_COMMANDS 255U
#define MAX_FILES 256U
#define MAX_FILENAME 255U
#define LAYOUT_PATH_SIZE (sizeof(LAYOUT_DIR) + MAX_FILENAME + 2U)
#define EDIT_BACKUP ".suite-bak"
#define EDIT_STAGE ".suite-new"
#define PRESS_DOWN 0U
#define PRESS_UP 1U
#define NO_COMMAND UINT16_MAX
#define SUITE_LAYOUT_V1 "# Remote Suite v1 slots: "
#define SUITE_LAYOUT_V2 "# Remote Suite v2 slots: "
#define SOURCE_PREFIX "# Remote Suite source: "
#define FULL_NEC_MARKER "# POOM IR IDENTIFY"

static void resume_menu(void)
{
    const uint8_t token = MENU_REMOTE_SUITE_RESUME_TOKEN;
    poom_sbus_publish("poom/menu/resume", &token, sizeof(token), pdMS_TO_TICKS(100));
}

#if defined(PIN_NUM_IR_TX)

typedef struct
{
    char* line;
    ir_raw_signal_t* raw;
} suite_scratch_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
} suite_key_t;

static StaticQueue_t s_queue_storage;
static uint8_t s_queue_data[12 * sizeof(suite_key_t)];
static QueueHandle_t s_keys;

static const char* const slot_names[SUITE_SLOTS] = {
    "POOM D-PAD UP", "POOM D-PAD DOWN", "POOM D-PAD LEFT", "POOM D-PAD RIGHT",
    "POOM D-PAD UP + A", "POOM D-PAD DOWN + A", "POOM D-PAD LEFT + A", "POOM D-PAD RIGHT + A",
    "POOM A BUTTON", "POOM B BUTTON", "POOM B BUTTON + A"
};

static void button_callback(const poom_sbus_msg_t* msg, void* context)
{
    if(msg->len < sizeof(button_event_msg_t)) return;
    button_event_msg_t event;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != PRESS_DOWN && (event.event != PRESS_UP || (event.button != BUTTON_A && event.button != BUTTON_B))) return;

    const suite_key_t key = {.button = event.button, .event = event.event};
    if(xQueueSend(context, &key, 0) != pdPASS && event.button == BUTTON_B)
    {
        xQueueReset(context);
        xQueueSend(context, &key, 0);
    }
}

static suite_key_t next_key(void)
{
    suite_key_t key;
    xQueueReceive(s_keys, &key, portMAX_DELAY);
    return key;
}

static uint8_t next_press(void)
{
    suite_key_t key;
    do { key = next_key(); } while(key.event != PRESS_DOWN);
    return key.button;
}

static uint8_t next_mapping_key(void)
{
    const uint8_t pressed = next_press();
    if(pressed != BUTTON_A) return pressed;

    const TickType_t start = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(SAVE_HOLD_MS);
    while(true)
    {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        suite_key_t key;
        if(elapsed >= duration || xQueueReceive(s_keys, &key, duration - elapsed) != pdPASS)
            return MAPPING_SAVE;
        if(key.button == BUTTON_A && key.event == PRESS_UP)
            return xTaskGetTickCount() - start >= duration ? MAPPING_SAVE : BUTTON_A;
        if(key.button != BUTTON_A && key.event == PRESS_DOWN) return key.button;
    }
}

static void text_at(int16_t x, int16_t y, const char* text)
{
    poom_arduboy_set_cursor(x, y);
    poom_arduboy_print(text);
}

static void header(const char* title)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    text_at(2, 2, title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void notice(const char* title, const char* text)
{
    header(title);
    text_at(0, 25, text);
    text_at(0, 56, "A:OK         B:BACK");
    poom_arduboy_display();
    uint8_t key;
    do { key = next_press(); } while(key != BUTTON_A && key != BUTTON_B);
}

typedef const char* (*label_fn_t)(const void* context, unsigned index);

static int pick(const char* title, unsigned count, unsigned selected, label_fn_t label, const void* context)
{
    size_t scroll = 0;
    unsigned first = selected > 2 ? selected - 2 : 0;
    while(true)
    {
        if(selected < first) first = selected;
        else if(selected - first >= 3) first = selected - 2;
        header(title);
        for(unsigned row = 0; row < 3 && first + row < count; ++row)
        {
            const unsigned i = first + row;
            const int16_t y = (int16_t)(16 + row * 12);
            char text[20];
            const char* name = label(context, i);
            snprintf(text, sizeof(text), "%.19s", name + (i == selected ? scroll : 0));
            if(i == selected) text_at(0, y, ">");
            text_at(12, y, text);
        }
        text_at(0, 56, "A:SELECT     B:BACK");
        poom_arduboy_display();
        switch(next_press())
        {
            case BUTTON_UP: selected = (selected + count - 1) % count; scroll = 0; break;
            case BUTTON_DOWN: selected = (selected + 1) % count; scroll = 0; break;
            case BUTTON_LEFT: if(scroll) --scroll; break;
            case BUTTON_RIGHT: if(scroll + 19 < strlen(label(context, selected))) ++scroll; break;
            case BUTTON_A: return (int)selected;
            case BUTTON_B: return -1;
            default: break;
        }
    }
}

static const char* array_label(const void* context, unsigned index)
{
    return ((const char* const*)context)[index];
}

static int compare_names(const void* a, const void* b)
{
    return strcasecmp(*(const char* const*)a, *(const char* const*)b);
}

static void recover_layout_edits(void)
{
    DIR* dir = opendir(LAYOUT_DIR);
    if(!dir) return;
    struct dirent* entry;
    while((entry = readdir(dir)))
    {
        const size_t n = strlen(entry->d_name);
        const size_t suffix = sizeof(EDIT_BACKUP) - 1;
        if(n <= suffix + 3 || n > MAX_FILENAME ||
           strcasecmp(entry->d_name + n - suffix - 3, ".ir" EDIT_BACKUP) != 0) continue;
        char backup[LAYOUT_PATH_SIZE];
        snprintf(backup, sizeof(backup), LAYOUT_DIR "/%s", entry->d_name);
        char path[LAYOUT_PATH_SIZE];
        snprintf(path, sizeof(path), LAYOUT_DIR "/%.*s", (int)(n - suffix), entry->d_name);
        struct stat info;
        if(stat(path, &info) == 0) unlink(backup);
        else rename(backup, path);
    }
    closedir(dir);
}

static FILE* pick_file(const char* directory, const char* title, char* filename)
{
    if(strcmp(directory, LAYOUT_DIR) == 0) recover_layout_edits();
    DIR* dir = opendir(directory);
    if(!dir)
    {
        notice(title, "No remotes found");
        return NULL;
    }
    char** names = calloc(MAX_FILES, sizeof(*names));
    unsigned count = 0;
    struct dirent* entry;
    while(names && count < MAX_FILES && (entry = readdir(dir)))
    {
        const size_t n = strlen(entry->d_name);
        if(entry->d_type == DT_DIR || n <= 3 || n > MAX_FILENAME ||
           strcasecmp(entry->d_name + n - 3, ".ir") != 0) continue;
        names[count] = strdup(entry->d_name);
        if(!names[count]) break;
        ++count;
    }
    closedir(dir);

    FILE* file = NULL;
    if(count)
    {
        qsort(names, count, sizeof(*names), compare_names);
        const int selected = pick(title, count, 0, array_label, names);
        if(selected >= 0)
        {
            char path[sizeof(SOURCE_DIR) + MAX_FILENAME + 2];
            snprintf(path, sizeof(path), "%s/%s", directory, names[selected]);
            file = fopen(path, "rb");
            if(file) snprintf(filename, MAX_FILENAME + 1, "%s", names[selected]);
            else notice(title, "Could not open file");
        }
    }
    else notice(title, names ? "No .ir profiles" : "Out of memory");
    for(unsigned i = 0; i < count; ++i) free(names[i]);
    free(names);
    return file;
}

static bool valid_source_name(const char* name)
{
    const size_t length = strlen(name);
    if(length <= 3 || length > MAX_FILENAME || strcasecmp(name + length - 3, ".ir") != 0)
        return false;
    for(const unsigned char* p = (const unsigned char*)name; *p; ++p)
        if(*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':') return false;
    return true;
}

/* The layout is a Flipper file whose header declares the occupied slots.
 * Commands are stored in slot order, so the file is independent of its source. */
static bool read_header(FILE* file, suite_scratch_t* scratch, bool* full_nec,
                        uint16_t* mask, char* source_name)
{
    *full_nec = false;
    *mask = 0;
    if(source_name) source_name[0] = '\0';
    long offset;
    while((offset = ftell(file)) >= 0 && fgets(scratch->line, IR_FILE_LINE_SIZE, file))
    {
        if(strncmp(scratch->line, "name:", 5) == 0)
            return fseek(file, offset, SEEK_SET) == 0;
        scratch->line[strcspn(scratch->line, "\r\n")] = '\0';
        if(strcmp(scratch->line, FULL_NEC_MARKER) == 0) *full_nec = true;
        if(source_name && strncmp(scratch->line, SOURCE_PREFIX, sizeof(SOURCE_PREFIX) - 1) == 0)
        {
            const char* name = scratch->line + sizeof(SOURCE_PREFIX) - 1;
            if(valid_source_name(name)) memcpy(source_name, name, strlen(name) + 1);
        }
        const bool v1 = strncmp(scratch->line, SUITE_LAYOUT_V1, sizeof(SUITE_LAYOUT_V1) - 1) == 0;
        const bool v2 = strncmp(scratch->line, SUITE_LAYOUT_V2, sizeof(SUITE_LAYOUT_V2) - 1) == 0;
        if(v1 || v2)
        {
            const char* value = scratch->line + sizeof(SUITE_LAYOUT_V1) - 1;
            const size_t digits = v1 ? 2U : 3U;
            if(strlen(value) != digits) return false;
            for(size_t i = 0; i < digits; ++i)
                if(!isxdigit((unsigned char)value[i])) return false;
            const unsigned long slots = strtoul(value, NULL, 16);
            if(slots >= (1UL << SUITE_SLOTS)) return false;
            *mask = (uint16_t)slots;
        }
    }
    return false;
}

static bool copy_record(FILE* source, FILE* output, suite_scratch_t* scratch,
                        const ir_file_command_t* selected)
{
    ir_file_command_t command;
    if(!ir_file_read_at(source, scratch->line, scratch->raw, &command, selected->offset) ||
       strcmp(command.name, selected->name) != 0) return false;

    const long end = ftell(source);
    if(end <= selected->offset || fseek(source, selected->offset, SEEK_SET) != 0) return false;
    int last = '\n';
    for(long remaining = end - selected->offset; remaining > 0;)
    {
        const size_t wanted = remaining < (long)IR_FILE_LINE_SIZE ? (size_t)remaining : IR_FILE_LINE_SIZE;
        const size_t got = fread(scratch->line, 1, wanted, source);
        if(got != wanted || fwrite(scratch->line, 1, got, output) != got) return false;
        last = (unsigned char)scratch->line[got - 1];
        remaining -= (long)got;
    }
    return last == '\n' || fputc('\n', output) != EOF;
}

typedef struct
{
    const char* layout;
    const char* flipper;
    const char* source_name;
    unsigned saved_count;
    bool full_nec;
} suite_sources_t;

static bool write_layout(const suite_sources_t* sources, suite_scratch_t* scratch,
                         const ir_file_command_t* commands, const uint16_t* slots,
                         const char* basename, const char* temp)
{
    const int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(fd < 0) return false;
    FILE* out = fdopen(fd, "wb");
    if(!out)
    {
        close(fd);
        unlink(temp);
        return false;
    }

    unsigned mask = 0;
    for(unsigned i = 0; i < SUITE_SLOTS; ++i)
        if(slots[i] != NO_COMMAND) mask |= 1U << i;
    bool ok = mask && fprintf(out, "Filetype: IR signals file\nVersion: 1\n# %s\n"
                             SUITE_LAYOUT_V2 "%03X\n", basename, mask) > 0;
    if(ok && sources->full_nec) ok = fputs(FULL_NEC_MARKER "\n", out) >= 0;
    if(ok && sources->source_name[0])
        ok = fprintf(out, SOURCE_PREFIX "%s\n", sources->source_name) > 0;

    FILE* source = NULL;
    const char* active = NULL;
    for(unsigned i = 0; ok && i < SUITE_SLOTS; ++i)
    {
        if(slots[i] == NO_COMMAND) continue;
        const char* path = slots[i] < sources->saved_count ? sources->layout : sources->flipper;
        if(path != active)
        {
            if(source) fclose(source);
            source = fopen(path, "rb");
            active = path;
        }
        ok = source && copy_record(source, out, scratch, &commands[slots[i]]);
    }
    if(source) fclose(source);
    if(fclose(out) != 0) ok = false;
    if(!ok) unlink(temp);
    return ok;
}

static bool save_layout(const suite_sources_t* sources, suite_scratch_t* scratch,
                        const ir_file_command_t* commands, const uint16_t* slots, const char* name)
{
    char basename[21];
    unsigned i = 0;
    for(; name[i] && i + 1 < sizeof(basename); ++i)
    {
        const unsigned char c = (unsigned char)name[i];
        basename[i] = isalnum(c) || c == '-' || c == '_' ? (char)c : '_';
    }
    basename[i] = '\0';
    if(!i || sd_card_create_dir("/ir/suite") != ESP_OK) return false;
    char path[sizeof(LAYOUT_DIR) + sizeof(basename) + 8];
    char temp[sizeof(path)];
    snprintf(path, sizeof(path), LAYOUT_DIR "/%s.ir", basename);
    snprintf(temp, sizeof(temp), LAYOUT_DIR "/%s.tmp", basename);
    struct stat info;
    if(stat(path, &info) == 0 ||
       !write_layout(sources, scratch, commands, slots, basename, temp)) return false;
    if(rename(temp, path) == 0) return true;
    unlink(temp);
    return false;
}

static bool replace_layout(const char* path, const char* staged, const char* backup)
{
    struct stat info;
    if(stat(backup, &info) == 0 || rename(path, backup) != 0) return false;
    if(rename(staged, path) == 0)
    {
        unlink(backup);
        return true;
    }
    /* Leave the backup available for recovery if the card was removed. */
    rename(backup, path);
    return false;
}

static bool edit_save(const suite_sources_t* sources, suite_scratch_t* scratch,
                      const ir_file_command_t* commands, const uint16_t* slots,
                      const char* filename)
{
    char path[LAYOUT_PATH_SIZE];
    char staged[LAYOUT_PATH_SIZE + sizeof(EDIT_STAGE)];
    char backup[LAYOUT_PATH_SIZE + sizeof(EDIT_BACKUP)];
    snprintf(path, sizeof(path), LAYOUT_DIR "/%s", filename);
    snprintf(staged, sizeof(staged), "%s" EDIT_STAGE, path);
    snprintf(backup, sizeof(backup), "%s" EDIT_BACKUP, path);
    char title[MAX_FILENAME + 1];
    snprintf(title, sizeof(title), "%.*s", (int)strlen(filename) - 3, filename);

    /* A staged file from an interrupted edit has never been committed. */
    unlink(staged);
    if(!write_layout(sources, scratch, commands, slots, title, staged)) return false;
    if(replace_layout(path, staged, backup)) return true;
    unlink(staged);
    return false;
}

typedef struct
{
    const ir_file_command_t* commands;
    unsigned saved_count;
    char* label;
} command_menu_t;

static const char* command_label(const void* context, unsigned index)
{
    const command_menu_t* menu = context;
    if(index == 0) return "Unassigned";
    const char* name = menu->commands[index - 1].name;
    if(index > menu->saved_count) return name;
    snprintf(menu->label, IR_FILE_NAME_SIZE + sizeof("Saved "), "Saved %s", name);
    return menu->label;
}

static bool name_layout(char* name)
{
    poom_ui_keyboard_t keyboard;
    poom_ui_keyboard_init(&keyboard, name, 21, "REMOTE NAME");
    while(true)
    {
        poom_ui_keyboard_draw(&keyboard);
        const uint8_t key = next_press();
        if(key == BUTTON_B) return false;
        if(poom_ui_keyboard_handle_button(&keyboard, key) == POOM_UI_KEYBOARD_ACTION_ACCEPT && *name)
            return true;
    }
}

static unsigned load_commands(FILE* file, suite_scratch_t* scratch, bool full_nec,
                              ir_file_command_t* commands, unsigned capacity)
{
    unsigned count = 0;
    ir_file_command_t command;
    while(count < capacity && ir_file_read(file, scratch->line, scratch->raw, &command))
        if(!full_nec || ir_file_command_valid(&command)) commands[count++] = command;
    return ferror(file) ? 0 : count;
}

static void configure_remote(suite_scratch_t* scratch, bool editing)
{
    char filename[MAX_FILENAME + 1];
    FILE* source = editing ? pick_file(LAYOUT_DIR, "EDIT REMOTE", filename) :
                             pick_file(SOURCE_DIR, "IMPORT FLIPPER FILE", filename);
    if(!source) return;

    const unsigned capacity = MAX_COMMANDS + (editing ? SUITE_SLOTS : 0);
    ir_file_command_t* commands = calloc(capacity, sizeof(*commands));
    bool full_nec;
    uint16_t mask;
    char source_name[MAX_FILENAME + 1] = "";
    unsigned count = 0;
    if(commands && read_header(source, scratch, &full_nec, &mask, editing ? source_name : NULL))
        count = load_commands(source, scratch, full_nec, commands, editing ? SUITE_SLOTS + 1 : MAX_COMMANDS);
    fclose(source);
    if(!count)
    {
        notice("IMPORT FAILED", commands ? "No usable commands" : "Out of memory");
        goto cleanup;
    }

    char layout_path[LAYOUT_PATH_SIZE];
    char flipper_path[sizeof(SOURCE_DIR) + MAX_FILENAME + 2];
    uint16_t slots[SUITE_SLOTS];
    for(unsigned i = 0; i < SUITE_SLOTS; ++i) slots[i] = NO_COMMAND;
    suite_sources_t sources = {
        .layout = layout_path, .flipper = flipper_path, .source_name = source_name,
        .saved_count = editing ? count : 0, .full_nec = full_nec
    };
    if(editing)
    {
        unsigned command = 0;
        for(unsigned i = 0; i < SUITE_SLOTS; ++i)
            if(mask & (1U << i)) slots[i] = (uint16_t)command++;
        if(command != count)
        {
            notice("EDIT FAILED", "Invalid layout");
            goto cleanup;
        }

        snprintf(layout_path, sizeof(layout_path), LAYOUT_DIR "/%s", filename);
        FILE* original = NULL;
        if(source_name[0])
        {
            snprintf(flipper_path, sizeof(flipper_path), SOURCE_DIR "/%s", source_name);
            original = fopen(flipper_path, "rb");
        }
        char chosen[MAX_FILENAME + 1] = "";
        if(!original)
        {
            notice("LINK FLIPPER FILE", "Choose original file");
            original = pick_file(SOURCE_DIR, "CHOOSE SOURCE", chosen);
        }
        if(original)
        {
            bool source_nec;
            uint16_t ignored;
            unsigned added = 0;
            if((!chosen[0] || valid_source_name(chosen)) &&
               read_header(original, scratch, &source_nec, &ignored, NULL) && source_nec == full_nec)
                added = load_commands(original, scratch, source_nec, commands + count, MAX_COMMANDS);
            fclose(original);
            if(added)
            {
                if(chosen[0] && valid_source_name(chosen))
                {
                    snprintf(source_name, sizeof(source_name), "%s", chosen);
                    snprintf(flipper_path, sizeof(flipper_path), SOURCE_DIR "/%s", source_name);
                }
                count += added;
            }
            else notice("SOURCE UNAVAILABLE", "Using saved commands");
        }
    }
    else
    {
        if(valid_source_name(filename)) snprintf(source_name, sizeof(source_name), "%s", filename);
        snprintf(flipper_path, sizeof(flipper_path), SOURCE_DIR "/%s", filename);
    }
    static const uint8_t order[] = {0, 1, 2, 3, SUITE_A_SLOT, SUITE_B_SLOT, 4, 5, 6, 7, SUITE_AB_SLOT};
    unsigned selected = 0;
    while(true)
    {
        const unsigned slot = selected == SUITE_SLOTS ? SUITE_SLOTS : order[selected];
        header("MAP REMOTE BUTTONS");
        text_at(0, 14, selected == SUITE_SLOTS ? "SAVE LAYOUT" : slot_names[slot]);
        if(selected < SUITE_SLOTS)
        {
            char assignment[22];
            const char* name = slots[slot] == NO_COMMAND ? "Unassigned" : commands[slots[slot]].name;
            const int prefix = snprintf(assignment, sizeof(assignment), "%u/%u ", selected + 1, SUITE_SLOTS);
            snprintf(assignment + prefix, sizeof(assignment) - (size_t)prefix, "%.*s",
                     (int)sizeof(assignment) - prefix - 1, name);
            text_at(0, 25, assignment);
        }
        text_at(0, 43, "Hold A:Save Layout");
        text_at(0, 56, selected == SUITE_SLOTS ? "A:SAVE       B:BACK" : "A:ASSIGN     B:BACK");
        poom_arduboy_display();
        const uint8_t key = next_mapping_key();
        switch(key)
        {
            case BUTTON_UP: selected = (selected + SUITE_SLOTS) % (SUITE_SLOTS + 1); break;
            case BUTTON_DOWN: selected = (selected + 1) % (SUITE_SLOTS + 1); break;
            case BUTTON_B: goto cleanup;
            case MAPPING_SAVE:
            case BUTTON_A:
                if(key == MAPPING_SAVE || selected == SUITE_SLOTS)
                {
                    unsigned i = 0;
                    while(i < SUITE_SLOTS && slots[i] == NO_COMMAND) ++i;
                    if(i == SUITE_SLOTS)
                    {
                        notice("SAVE LAYOUT", "Assign a button first");
                        break;
                    }
                    if(editing)
                    {
                        if(edit_save(&sources, scratch, commands, slots, filename))
                        {
                            notice("REMOTE UPDATED", "Use Open Remote");
                            goto cleanup;
                        }
                        notice("SAVE FAILED", "Check the SD card");
                        break;
                    }
                    char name[21] = "";
                    if(!name_layout(name)) break;
                    if(save_layout(&sources, scratch, commands, slots, name))
                    {
                        notice("REMOTE SAVED", "Use Open Remote");
                        goto cleanup;
                    }
                    notice("SAVE FAILED", "Check name or SD");
                }
                else
                {
                    const unsigned initial = slots[slot] == NO_COMMAND ? 0 : slots[slot] + 1U;
                    char label[IR_FILE_NAME_SIZE + sizeof("Saved ")];
                    const command_menu_t menu = {.commands = commands, .saved_count = sources.saved_count,
                                                 .label = label};
                    const int value = pick(slot_names[slot], count + 1, initial, command_label, &menu);
                    if(value >= 0) slots[slot] = value ? (uint16_t)(value - 1) : NO_COMMAND;
                }
                break;
            default: break;
        }
    }
cleanup:
    free(commands);
}

static int direction_slot(uint8_t key)
{
    switch(key)
    {
        case BUTTON_UP: return 0;
        case BUTTON_DOWN: return 1;
        case BUTTON_LEFT: return 2;
        case BUTTON_RIGHT: return 3;
        default: return -1;
    }
}

static void draw_playback(const char* title, const ir_file_command_t* slots, uint16_t mask,
                          bool modified, const char* last, const char* status)
{
    static const uint8_t regular[] = {0, 1, 2, 3, SUITE_A_SLOT, SUITE_B_SLOT};
    static const uint8_t shortcuts[] = {4, 5, 6, 7, SUITE_AB_SLOT};
    static const char* const labels[] = {"U", "D", "L", "R", "A", "B"};
    static const char* const fn_labels[] = {"A+U", "A+D", "A+L", "A+R", "A+B"};
    char heading[22];
    snprintf(heading, sizeof(heading), modified ? "%.16s [FN]" : "%.21s", title);
    header(heading);
    const unsigned entries = modified ? sizeof(shortcuts) : sizeof(regular);
    for(unsigned i = 0; i < entries; ++i)
    {
        const unsigned slot = modified ? shortcuts[i] : regular[i];
        const char* label = modified ? fn_labels[i] : labels[i];
        const char* name = (mask & (1U << slot)) ? slots[slot].name : "-";
        char text[11];
        snprintf(text, sizeof(text), "%s:%.*s", label, 9 - (int)strlen(label), name);
        text_at(i % 2 ? 66 : 0, (int16_t)(15 + 10 * (i / 2)), text);
    }
    text_at(0, 46, "Hold A:FN Hold B:Exit");
    text_at(0, 56, status ? status : last);
    poom_arduboy_display();
}

static void play_remote(suite_scratch_t* scratch)
{
    char filename[MAX_FILENAME + 1];
    FILE* file = pick_file(LAYOUT_DIR, "OPEN REMOTE", filename);
    if(!file) return;

    bool full_nec;
    uint16_t mask;
    bool valid = read_header(file, scratch, &full_nec, &mask, NULL) && mask;
    ir_file_command_t slots[SUITE_SLOTS] = {0};
    for(unsigned i = 0; valid && i < SUITE_SLOTS; ++i)
    {
        if(mask & (1U << i))
            valid = ir_file_read(file, scratch->line, scratch->raw, &slots[i]) &&
                    (!full_nec || ir_file_command_valid(&slots[i]));
    }
    ir_file_command_t extra;
    if(!valid || ir_file_read(file, scratch->line, scratch->raw, &extra) || ferror(file))
    {
        notice("BAD REMOTE", "Recreate the layout");
        fclose(file);
        return;
    }

    ir_tx_handle_t tx = {0};
    ir_tx_config_t cfg = ir_tx_default_config();
    cfg.gpio = PIN_NUM_IR_TX;
    if(ir_tx_init(&tx, &cfg, "remote_suite") != ESP_OK)
    {
        notice("REMOTE UNAVAILABLE", "IR transmitter busy");
        fclose(file);
        return;
    }

    filename[strlen(filename) - 3] = '\0';
    char title[22];
    snprintf(title, sizeof(title), "%.21s", filename);
    bool modified = false;
    bool modifier_used = false;
    bool b_down = false;
    TickType_t b_started = 0;
    const TickType_t back_hold = pdMS_TO_TICKS(BACK_HOLD_MS);
    char last[22] = "Last: -";
    const char* status = NULL;
    while(true)
    {
        draw_playback(title, slots, mask, modified, last, status);

        TickType_t wait = portMAX_DELAY;
        if(b_down)
        {
            const TickType_t elapsed = xTaskGetTickCount() - b_started;
            if(elapsed >= back_hold) break;
            wait = back_hold - elapsed;
        }
        suite_key_t key;
        if(xQueueReceive(s_keys, &key, wait) != pdPASS) break;
        unsigned index;
        if(key.button == BUTTON_B)
        {
            if(key.event == PRESS_DOWN)
            {
                if(modified)
                {
                    modifier_used = true;
                    b_down = false;
                    index = SUITE_AB_SLOT;
                }
                else
                {
                    b_down = true;
                    b_started = xTaskGetTickCount();
                    continue;
                }
            }
            else
            {
                if(!b_down) continue;
                b_down = false;
                if(xTaskGetTickCount() - b_started >= back_hold || !(mask & (1U << SUITE_B_SLOT))) break;
                index = SUITE_B_SLOT;
            }
        }
        else if(key.button == BUTTON_A)
        {
            if(key.event == PRESS_DOWN)
            {
                modified = true;
                modifier_used = false;
                continue;
            }
            if(!modified) continue;
            modified = false;
            if(modifier_used || !(mask & (1U << SUITE_A_SLOT))) continue;
            index = SUITE_A_SLOT;
        }
        else
        {
            const int direction = direction_slot(key.button);
            if(direction < 0) continue;
            modifier_used |= modified;
            index = (modified ? 4U : 0U) + (unsigned)direction;
        }
        if(!(mask & (1U << index)))
        {
            status = "Unassigned";
            continue;
        }
        ir_file_command_t command;
        const bool sent = ir_file_read_at(file, scratch->line, scratch->raw, &command, slots[index].offset) &&
                          strcmp(command.name, slots[index].name) == 0 &&
                          ir_file_send(&tx, &command, scratch->raw, full_nec) == ESP_OK;
        if(sent) snprintf(last, sizeof(last), "Last: %.15s", command.name);
        status = sent ? NULL : "Send failed";
    }
    ir_tx_deinit(&tx);
    fclose(file);
}

static void delete_remote(void)
{
    char filename[MAX_FILENAME + 1];
    FILE* file = pick_file(LAYOUT_DIR, "DELETE REMOTE", filename);
    if(!file) return;
    fclose(file);

    char title[22];
    const size_t title_length = strlen(filename) - 3;
    snprintf(title, sizeof(title), "%.*s", (int)(title_length < 21 ? title_length : 21), filename);
    const char* const choices[] = {"Keep remote", "Delete layout"};
    if(pick(title, 2, 0, array_label, choices) != 1) return;

    char path[LAYOUT_PATH_SIZE];
    snprintf(path, sizeof(path), LAYOUT_DIR "/%s", filename);
    if(unlink(path) == 0) notice("REMOTE DELETED", title);
    else notice("DELETE FAILED", "Check the SD card");
}

static void suite_task(void* arg)
{
    (void)arg;
    if(!s_keys)
        s_keys = xQueueCreateStatic(12, sizeof(suite_key_t), s_queue_data, &s_queue_storage);
    xQueueReset(s_keys);
    if(poom_sbus_subscribe_cb("input/button", button_callback, s_keys))
    {
        suite_scratch_t scratch = {.line = malloc(IR_FILE_LINE_SIZE), .raw = malloc(IR_FILE_RAW_SIZE)};
        if(!scratch.line || !scratch.raw) notice("REMOTE SUITE", "Out of memory");
        else if(sd_card_mount() != ESP_OK) notice("REMOTE SUITE", "SD unavailable");
        else
        {
            const char* const options[] = {"Open Remote", "Create Remote", "Edit Remote", "Delete Remote"};
            unsigned selected = 0;
            while(true)
            {
                const int choice = pick("REMOTE SUITE", 4, selected, array_label, options);
                if(choice < 0) break;
                selected = (unsigned)choice;
                if(choice == 0) play_remote(&scratch);
                else if(choice == 3) delete_remote();
                else configure_remote(&scratch, choice == 2);
            }
        }
        free(scratch.raw);
        free(scratch.line);
        poom_sbus_unsubscribe_cb("input/button", button_callback, s_keys);
    }
    resume_menu();
    vTaskDelete(NULL);
}

void menu_remote_suite_show(void)
{
    if(xTaskCreate(suite_task, "remote_suite", 6144, NULL, 4, NULL) != pdPASS)
        resume_menu();
}

#else

void menu_remote_suite_show(void)
{
    resume_menu();
}

#endif
