// SPDX-License-Identifier: MIT
#pragma once
/* Hardware-independent Wardrive UI helpers: long-press timing, elapsed
 * recording time, compact text formatting, word wrapping, menu composition
 * and small OLED glyphs. Host-tested by tools/wardrive_hostcheck. */
#include "poom_wardrive.h"
#include "wardrive_timezone.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_UI_COLUMNS 21U
#define WD_HOLD_PAUSE_MS 2000U
#define WD_HOLD_STOP_MS 3000U
#define WD_HOLD_DELETE_MS 3000U
#define WD_HOLD_BULK_MS 3000U
/* Below this, a press is a normal tap: don't flash hold feedback. */
#define WD_HOLD_FEEDBACK_MS 150U

/* One continuous button hold bound to the screen it started on. Fires once
 * when the threshold is reached; never repeats while still held. */
typedef struct {
    uint64_t start_ms;
    uint32_t duration_ms;
    unsigned screen;
    uint8_t button;
    bool active, fired;
} wd_hold_t;
void wd_hold_press(wd_hold_t *hold,uint8_t button,unsigned screen,uint32_t duration_ms,uint64_t now);
/* Release (or interruption) cancels an incomplete hold and resets progress. */
void wd_hold_release(wd_hold_t *hold,uint8_t button);
void wd_hold_cancel(wd_hold_t *hold);
/* True exactly once, when held continuously for duration on the same screen. */
bool wd_hold_poll(wd_hold_t *hold,unsigned screen,uint64_t now);
/* 0..1000 of the required time; 0 when not holding on this screen. */
unsigned wd_hold_permille(const wd_hold_t *hold,unsigned screen,uint64_t now);
/* True while visible feedback should be drawn for this button and screen. */
bool wd_hold_visible(const wd_hold_t *hold,uint8_t button,unsigned screen,uint64_t now);

/* Active recording time, excluding paused periods. */
typedef struct { uint64_t accumulated_ms, resumed_ms; bool running; } wd_elapsed_t;
void wd_elapsed_start(wd_elapsed_t *elapsed,uint64_t now);
void wd_elapsed_pause(wd_elapsed_t *elapsed,uint64_t now);
void wd_elapsed_resume(wd_elapsed_t *elapsed,uint64_t now);
uint64_t wd_elapsed_ms(const wd_elapsed_t *elapsed,uint64_t now);
/* HH:MM:SS; saturates at 99:59:59. */
void wd_elapsed_text(uint64_t ms,char out[9]);

/* HH:MM in the zone, or "--:--" until a plausible UTC clock exists. */
void wd_clock_text(int64_t utc,wd_timezone_t zone,char out[6]);
/* Compact counts: 0..999, 1.0k..999k, 1.0M.., 1.0G.. (max 4 chars). */
void wd_count_text(char out[5],uint32_t count);
/* 0m..999m, 1.0km..99.9km, 100km..9999km (max 6 chars). */
void wd_distance_text(double metres,char out[8]);

typedef enum { WD_GPS_ICON_FIX, WD_GPS_ICON_SEARCHING, WD_GPS_ICON_NONE } wd_gps_icon_t;
/* ✓ fix; ... searching (receiver answering, no fix yet); X offline or a fix
 * that was held earlier in this app visit and has been lost. */
wd_gps_icon_t wd_gps_icon(bool connected,bool fix,bool had_fix);

/* Main menu composition (spec order). Stop Session follows Resume. */
typedef enum { WD_MENU_START, WD_MENU_STOP, WD_MENU_GPS, WD_MENU_SAVED, WD_MENU_BULK, WD_MENU_API, WD_MENU_WIFI, WD_MENU_SETTINGS } wd_menu_item_t;
unsigned wd_main_menu(bool active,wd_menu_item_t out[8]);
const char *wd_main_menu_label(wd_menu_item_t item,bool active);

/* Word wrap: '\n' ends a line, "\n\n" leaves a blank line. Returns the line
 * count (may exceed max; only the first max lines are written). */
unsigned wd_wrap(const char *text,unsigned width,char lines[][WD_UI_COLUMNS+1],unsigned max);
/* Same wrapping, writing only lines [first, first+max): lets a scrolling view
 * keep a few visible lines instead of the whole text. lines may be NULL. */
unsigned wd_wrap_from(const char *text,unsigned width,unsigned first,char lines[][WD_UI_COLUMNS+1],unsigned max);

/* Additional Settings: 0 Scan Type, 1 Scan Gap, 2 Log Gap, 3 RSSI, 4 BLE
 * Window, 5 Time Zone (submenu). direction +1/-1 cycles within the limits. */
#define WD_SETTINGS_ROWS 6U
#define WD_SETTINGS_TIMEZONE_ROW 5U
void wd_settings_adjust(wd_settings_t *settings,unsigned row,int direction);
void wd_settings_row_text(const wd_settings_t *settings,const wd_timezone_config_t *zone,unsigned row,char out[WD_UI_COLUMNS+1]);
const char *wd_scan_type_text(wd_mode_t mode);

/* 5-column glyphs (LSB = top row, 8 rows), drawn like the 5x7 font. */
extern const uint8_t wd_glyph_check[5];
extern const uint8_t wd_glyph_bluetooth[5];
/* Clockwise ring (radius 6) starting at 12 o'clock; distinct pixels. */
typedef struct { int8_t x, y; } wd_point_t;
extern const wd_point_t wd_ring_points[];
extern const unsigned wd_ring_point_count;
