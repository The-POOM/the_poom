// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#ifndef MENU_DETECTOR_VIEW_H
#define MENU_DETECTOR_VIEW_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_DETECTOR_VIEW_MAX_ITEMS   (12)
#define MENU_DETECTOR_VIEW_VISIBLE_ROWS (4)
#define MENU_DETECTOR_VIEW_NAME_LEN    (24U)

typedef struct
{
    char name[MENU_DETECTOR_VIEW_NAME_LEN];
    int rssi;
    bool rssi_known;
} menu_detector_view_row_t;

void menu_detector_view_draw_error(const char *title,
                                   const char *line1,
                                   const char *line2);
void menu_detector_view_draw_waiting(const char *title);
void menu_detector_view_draw_waiting_actions(const char *title,
                                             const char *back_action);
void menu_detector_view_draw_selector(const char *title,
                                      const char *const *labels,
                                      size_t count,
                                      int selected_index);
void menu_detector_view_draw_list(const char *title,
                                  const menu_detector_view_row_t *rows,
                                  int count,
                                  int selected_index,
                                  int scroll);
void menu_detector_view_draw_list_actions(const char *title,
                                          const menu_detector_view_row_t *rows,
                                          int count,
                                          int selected_index,
                                          int scroll,
                                          const char *select_action,
                                          const char *back_action);
void menu_detector_view_draw_detail_actions(const char *const lines[5], int selected_index, int count, const char *action);

void menu_detector_view_draw_detail(const char *const lines[5],
                                    int selected_index,
                                    int count);

#ifdef __cplusplus
}
#endif

#endif /* MENU_DETECTOR_VIEW_H */
