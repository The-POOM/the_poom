// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#include "menu_detector_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"

#define MENU_DETECTOR_HEADER_H     (11)
#define MENU_DETECTOR_LIST_Y0      (14)
#define MENU_DETECTOR_ROW_STEP     (10)
#define MENU_DETECTOR_ROW_HILITE_H (9)

static int16_t menu_detector_view_center_x_(const char *text)
{
    size_t len = (text != NULL) ? strlen(text) : 0U;
    int width = (int)len * 6;
    int x = ((int)ARDUBOY_WIDTH - width) / 2;
    return (int16_t)((x > 0) ? x : 0);
}

static void menu_detector_view_draw_header_(const char *title)
{
    poom_arduboy_set_cursor(menu_detector_view_center_x_(title), 2);
    (void)poom_arduboy_print((title != NULL) ? title : "DETECTOR");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_DETECTOR_HEADER_H, INVERT);
}

void menu_detector_view_draw_error(const char *title,
                                   const char *line1,
                                   const char *line2)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_detector_view_draw_header_(title);
    poom_arduboy_set_cursor(10, 24);
    (void)poom_arduboy_print((line1 != NULL) ? line1 : "");
    poom_arduboy_set_cursor(10, 34);
    (void)poom_arduboy_print((line2 != NULL) ? line2 : "");
    poom_arduboy_display();
}

void menu_detector_view_draw_waiting(const char *title)
{
    menu_detector_view_draw_waiting_actions(title, "B:EXIT");
}

void menu_detector_view_draw_waiting_actions(const char *title,
                                              const char *back_action)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_detector_view_draw_header_(title);
    poom_arduboy_set_cursor(22, 28);
    (void)poom_arduboy_print(F("Scanning..."));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print((back_action != NULL) ? back_action : "B:EXIT");
    poom_arduboy_display();
}

void menu_detector_view_draw_selector(const char *title,
                                      const char *const *labels,
                                      size_t count,
                                      int selected_index)
{
    size_t row;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_detector_view_draw_header_(title);
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SELECT"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    for(row = 0U; (row < count) && (row < MENU_DETECTOR_VIEW_VISIBLE_ROWS); ++row)
    {
        int16_t y = (int16_t)(MENU_DETECTOR_LIST_Y0 + (int)row * MENU_DETECTOR_ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(labels[row]);
        if((int)row == selected_index)
        {
            poom_arduboy_fill_rect(0,
                                   (int16_t)(y - 1),
                                   ARDUBOY_WIDTH,
                                   MENU_DETECTOR_ROW_HILITE_H,
                                   INVERT);
        }
    }
    poom_arduboy_display();
}

void menu_detector_view_draw_list(const char *title,
                                  const menu_detector_view_row_t *rows,
                                  int count,
                                  int selected_index,
                                  int scroll)
{
    menu_detector_view_draw_list_actions(title,
                                         rows,
                                         count,
                                         selected_index,
                                         scroll,
                                         "A:DETAIL",
                                         "B:EXIT");
}

void menu_detector_view_draw_list_actions(const char *title,
                                          const menu_detector_view_row_t *rows,
                                          int count,
                                          int selected_index,
                                          int scroll,
                                          const char *select_action,
                                          const char *back_action)
{
    int max_scroll = count - MENU_DETECTOR_VIEW_VISIBLE_ROWS;
    int row;

    if(max_scroll < 0)
    {
        max_scroll = 0;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_detector_view_draw_header_(title);
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((select_action != NULL) ? select_action : "A:DETAIL");
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print((back_action != NULL) ? back_action : "B:EXIT");

    for(row = 0; row < MENU_DETECTOR_VIEW_VISIBLE_ROWS; ++row)
    {
        int index = scroll + row;
        int16_t y;
        char name_line[16];
        char rssi_line[10];

        if(index >= count)
        {
            break;
        }

        y = (int16_t)(MENU_DETECTOR_LIST_Y0 + row * MENU_DETECTOR_ROW_STEP);
        (void)snprintf(name_line, sizeof(name_line), "%.14s", rows[index].name);
        if(rows[index].rssi_known)
        {
            (void)snprintf(rssi_line, sizeof(rssi_line), "%ddBm", rows[index].rssi);
        }
        else
        {
            (void)snprintf(rssi_line, sizeof(rssi_line), "---");
        }
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(name_line);
        poom_arduboy_set_cursor(83, y);
        (void)poom_arduboy_print(rssi_line);

        if(index == selected_index)
        {
            poom_arduboy_fill_rect(0,
                                   (int16_t)(y - 1),
                                   ARDUBOY_WIDTH,
                                   MENU_DETECTOR_ROW_HILITE_H,
                                   INVERT);
        }
    }

    if(scroll > 0)
    {
        poom_arduboy_fill_triangle(124,
                                   (int16_t)(MENU_DETECTOR_LIST_Y0 - 3),
                                   120,
                                   (int16_t)(MENU_DETECTOR_LIST_Y0 + 1),
                                   127,
                                   (int16_t)(MENU_DETECTOR_LIST_Y0 + 1),
                                   WHITE);
    }
    if(scroll < max_scroll)
    {
        int16_t y = (int16_t)(MENU_DETECTOR_LIST_Y0 +
                              (MENU_DETECTOR_VIEW_VISIBLE_ROWS - 1) * MENU_DETECTOR_ROW_STEP + 6);
        poom_arduboy_fill_triangle(120, y, 127, y, 124, (int16_t)(y + 4), WHITE);
    }

    poom_arduboy_display();
}

void menu_detector_view_draw_detail_actions(const char *const lines[5],
                                            int selected_index,
                                            int count,
                                            const char *action)
{
    int i;
    char index_line[24];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(36, 2);
    (void)poom_arduboy_print(F("DETAIL"));
    (void)snprintf(index_line, sizeof(index_line), "%d/%d", selected_index + 1, count);
    poom_arduboy_set_cursor(92, 2);
    (void)poom_arduboy_print(index_line);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_DETECTOR_HEADER_H, INVERT);

    for(i = 0; i < 5; ++i)
    {
        poom_arduboy_set_cursor(0, (int16_t)(14 + i * 8));
        (void)poom_arduboy_print((lines[i] != NULL) ? lines[i] : "");
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((action != NULL) ? action : "A:NAME");
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

void menu_detector_view_draw_detail(const char *const lines[5], int selected_index, int count)
{
    menu_detector_view_draw_detail_actions(lines, selected_index, count, "A:NAME");
}
