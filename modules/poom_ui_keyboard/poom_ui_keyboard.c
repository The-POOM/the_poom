// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ui_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
#endif
#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif
#ifndef BTN_UP
#define BTN_UP (4U)
#endif
#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif

#define POOM_UI_KB_COLS (16)
#define POOM_UI_KB_CELL_W (8)
#define POOM_UI_KB_CELL_H (10)

typedef enum
{
    POOM_UI_KB_KEY_CHAR = 0,
    POOM_UI_KB_KEY_BACKSPACE,
    POOM_UI_KB_KEY_SHIFT,
    POOM_UI_KB_KEY_MODE,
    POOM_UI_KB_KEY_SPACE,
    POOM_UI_KB_KEY_OK,
} poom_ui_kb_key_type_t;

typedef struct
{
    poom_ui_kb_key_type_t type;
    char ch;
    const char *label; // optional for special keys
    uint8_t w_cols;    // width in keyboard columns
} poom_ui_kb_key_t;

typedef struct
{
    const poom_ui_kb_key_t *keys;
    uint8_t key_count;
} poom_ui_kb_row_t;

typedef struct
{
    poom_ui_kb_row_t rows[3];
} poom_ui_kb_layout_t;

static const poom_ui_kb_key_t s_kb_alpha_row0[] = {
    {POOM_UI_KB_KEY_CHAR, 'q', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'w', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'e', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'r', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 't', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'y', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'u', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'i', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'o', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'p', NULL, 1},
    {POOM_UI_KB_KEY_MODE, 0, "123", 3},
    {POOM_UI_KB_KEY_BACKSPACE, 0, "<-", 3},
};

static const poom_ui_kb_key_t s_kb_alpha_row1[] = {
    {POOM_UI_KB_KEY_CHAR, 'a', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 's', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'd', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'f', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'g', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'h', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'j', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'k', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'l', NULL, 1},
    {POOM_UI_KB_KEY_SHIFT, 0, "UP", 3},
    {POOM_UI_KB_KEY_CHAR, '.', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '-', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '_', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '/', NULL, 1},
};

static const poom_ui_kb_key_t s_kb_alpha_row2[] = {
    {POOM_UI_KB_KEY_CHAR, 'z', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'x', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'c', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'v', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'b', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'n', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, 'm', NULL, 1},
    {POOM_UI_KB_KEY_SPACE, 0, "SPACE", 5},
    {POOM_UI_KB_KEY_OK, 0, "OK", 4},
};

static const poom_ui_kb_key_t s_kb_num_row0[] = {
    {POOM_UI_KB_KEY_CHAR, '1', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '2', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '3', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '4', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '5', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '6', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '7', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '8', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '9', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '0', NULL, 1},
    {POOM_UI_KB_KEY_MODE, 0, "ABC", 3},
    {POOM_UI_KB_KEY_BACKSPACE, 0, "<-", 3},
};

static const poom_ui_kb_key_t s_kb_num_row1[] = {
    {POOM_UI_KB_KEY_CHAR, '!', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '@', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '#', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '$', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '%', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '^', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '&', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '*', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '.', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '-', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '_', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '/', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '?', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '+', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '(', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, ')', NULL, 1},
};

static const poom_ui_kb_key_t s_kb_num_row2[] = {
    {POOM_UI_KB_KEY_CHAR, '[', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, ']', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '{', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '}', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '<', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '>', NULL, 1},
    {POOM_UI_KB_KEY_CHAR, '=', NULL, 1},
    {POOM_UI_KB_KEY_SPACE, 0, "SPACE", 5},
    {POOM_UI_KB_KEY_OK, 0, "OK", 4},
};

/**
 * @brief Internal helper for `poom_ui_keyboard_layout`.
 *
 * @param[in] kb Parameter passed to the function.
 * @return poom_ui_kb_layout_t
 */
static poom_ui_kb_layout_t poom_ui_keyboard_layout_(const poom_ui_keyboard_t *kb)
{
    poom_ui_kb_layout_t layout;

    if((kb != NULL) && kb->numeric)
    {
        layout.rows[0] = (poom_ui_kb_row_t){s_kb_num_row0, (uint8_t)(sizeof(s_kb_num_row0) / sizeof(s_kb_num_row0[0]))};
        layout.rows[1] = (poom_ui_kb_row_t){s_kb_num_row1, (uint8_t)(sizeof(s_kb_num_row1) / sizeof(s_kb_num_row1[0]))};
        layout.rows[2] = (poom_ui_kb_row_t){s_kb_num_row2, (uint8_t)(sizeof(s_kb_num_row2) / sizeof(s_kb_num_row2[0]))};
    }
    else
    {
        layout.rows[0] = (poom_ui_kb_row_t){s_kb_alpha_row0, (uint8_t)(sizeof(s_kb_alpha_row0) / sizeof(s_kb_alpha_row0[0]))};
        layout.rows[1] = (poom_ui_kb_row_t){s_kb_alpha_row1, (uint8_t)(sizeof(s_kb_alpha_row1) / sizeof(s_kb_alpha_row1[0]))};
        layout.rows[2] = (poom_ui_kb_row_t){s_kb_alpha_row2, (uint8_t)(sizeof(s_kb_alpha_row2) / sizeof(s_kb_alpha_row2[0]))};
    }

    return layout;
}

/**
 * @brief Internal helper for `poom_ui_keyboard_key_x_cols`.
 *
 * @param[in] row Parameter passed to the function.
 * @param[in] key_index Parameter passed to the function.
 * @return uint16_t
 */
static uint16_t poom_ui_keyboard_key_x_cols_(const poom_ui_kb_row_t *row, uint8_t key_index)
{
    uint16_t x = 0U;

    if((row == NULL) || (key_index >= row->key_count))
    {
        return 0U;
    }

    for(uint8_t i = 0U; i < key_index; i++)
    {
        x = (uint16_t)(x + row->keys[i].w_cols);
    }

    return x;
}

/**
 * @brief Internal helper for `poom_ui_keyboard_find_nearest_key`.
 *
 * @param[in] row Parameter passed to the function.
 * @param[in] x_center_px Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t poom_ui_keyboard_find_nearest_key_(const poom_ui_kb_row_t *row, uint16_t x_center_px)
{
    uint8_t best = 0U;
    uint16_t best_dist = 0xFFFFU;

    if((row == NULL) || (row->key_count == 0U))
    {
        return 0U;
    }

    for(uint8_t i = 0U; i < row->key_count; i++)
    {
        const uint16_t x_cols = poom_ui_keyboard_key_x_cols_(row, i);
        const uint16_t w_cols = row->keys[i].w_cols;
        const uint16_t center_px = (uint16_t)((x_cols * POOM_UI_KB_CELL_W) + ((w_cols * POOM_UI_KB_CELL_W) / 2U));
        const uint16_t dist = (center_px > x_center_px) ? (center_px - x_center_px) : (x_center_px - center_px);

        if(dist < best_dist)
        {
            best_dist = dist;
            best = i;
        }
    }

    return best;
}

/**
 * @brief Internal helper for `poom_ui_keyboard_insert_char`.
 *
 * @param[in] kb Parameter passed to the function.
 * @param[in] ch Parameter passed to the function.
 * @return void
 */
static void poom_ui_keyboard_insert_char_(poom_ui_keyboard_t *kb, char ch)
{
    if((kb == NULL) || (kb->text == NULL) || (kb->text_cap < 2U))
    {
        return;
    }

    if(kb->text_len >= (kb->text_cap - 1U))
    {
        return;
    }

    kb->text[kb->text_len] = ch;
    kb->text_len++;
    kb->text[kb->text_len] = '\0';
}

/**
 * @brief Internal helper for `poom_ui_keyboard_backspace`.
 *
 * @param[in] kb Parameter passed to the function.
 * @return void
 */
static void poom_ui_keyboard_backspace_(poom_ui_keyboard_t *kb)
{
    if((kb == NULL) || (kb->text == NULL) || (kb->text_cap == 0U))
    {
        return;
    }

    if(kb->text_len == 0U)
    {
        return;
    }

    kb->text_len--;
    kb->text[kb->text_len] = '\0';
}

void poom_ui_keyboard_init(poom_ui_keyboard_t *kb, char *text, size_t text_cap, const char *target_label)
{
    if(kb == NULL)
    {
        return;
    }

    (void)memset(kb, 0, sizeof(*kb));
    kb->target_label = target_label;
    kb->text = text;
    kb->text_cap = text_cap;
    kb->caps = false;
    kb->numeric = false;
    kb->sel_row = 0U;
    kb->sel_key = 0U;

    if((kb->text != NULL) && (kb->text_cap > 0U))
    {
        kb->text[kb->text_cap - 1U] = '\0';
        kb->text_len = strnlen(kb->text, kb->text_cap - 1U);
    }
}

void poom_ui_keyboard_set_target_label(poom_ui_keyboard_t *kb, const char *target_label)
{
    if(kb == NULL)
    {
        return;
    }

    kb->target_label = target_label;
}

void poom_ui_keyboard_clear_text(poom_ui_keyboard_t *kb)
{
    if((kb == NULL) || (kb->text == NULL) || (kb->text_cap == 0U))
    {
        return;
    }

    kb->text[0] = '\0';
    kb->text_len = 0U;
}

/**
 * @brief Internal helper for `poom_ui_keyboard_apply_caps`.
 *
 * @param[in] kb Parameter passed to the function.
 * @param[in] ch Parameter passed to the function.
 * @return char
 */
static char poom_ui_keyboard_apply_caps_(const poom_ui_keyboard_t *kb, char ch)
{
    if((kb == NULL) || kb->numeric || !kb->caps)
    {
        return ch;
    }

    if((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - 'a' + 'A');
    }

    return ch;
}

poom_ui_keyboard_action_t poom_ui_keyboard_handle_button(poom_ui_keyboard_t *kb, uint8_t button)
{
    poom_ui_keyboard_action_t action = POOM_UI_KEYBOARD_ACTION_NONE;

    if(kb == NULL)
    {
        return action;
    }

    const poom_ui_kb_layout_t layout = poom_ui_keyboard_layout_(kb);
    const uint8_t row_count = 3U;

    if(kb->sel_row >= row_count)
    {
        kb->sel_row = 0U;
    }

    poom_ui_kb_row_t row = layout.rows[kb->sel_row];
    if(row.key_count == 0U)
    {
        kb->sel_key = 0U;
        return action;
    }
    if(kb->sel_key >= row.key_count)
    {
        kb->sel_key = (uint8_t)(row.key_count - 1U);
    }

    if(button == BTN_LEFT)
    {
        if(kb->sel_key > 0U)
        {
            kb->sel_key--;
        }
    }
    else if(button == BTN_RIGHT)
    {
        if(kb->sel_key + 1U < row.key_count)
        {
            kb->sel_key++;
        }
    }
    else if((button == BTN_UP) || (button == BTN_DOWN))
    {
        const uint16_t x_cols = poom_ui_keyboard_key_x_cols_(&row, kb->sel_key);
        const uint16_t w_cols = row.keys[kb->sel_key].w_cols;
        const uint16_t x_center_px =
            (uint16_t)((x_cols * POOM_UI_KB_CELL_W) + ((w_cols * POOM_UI_KB_CELL_W) / 2U));

        if((button == BTN_UP) && (kb->sel_row > 0U))
        {
            kb->sel_row--;
        }
        else if((button == BTN_DOWN) && (kb->sel_row + 1U < row_count))
        {
            kb->sel_row++;
        }

        const poom_ui_kb_row_t target_row = layout.rows[kb->sel_row];
        kb->sel_key = poom_ui_keyboard_find_nearest_key_(&target_row, x_center_px);
    }
    else if(button == BTN_A)
    {
        const poom_ui_kb_key_t key = row.keys[kb->sel_key];

        switch(key.type)
        {
            case POOM_UI_KB_KEY_CHAR:
                poom_ui_keyboard_insert_char_(kb, poom_ui_keyboard_apply_caps_(kb, key.ch));
                break;
            case POOM_UI_KB_KEY_BACKSPACE:
                poom_ui_keyboard_backspace_(kb);
                break;
            case POOM_UI_KB_KEY_SHIFT:
                if(!kb->numeric)
                {
                    kb->caps = !kb->caps;
                }
                break;
            case POOM_UI_KB_KEY_MODE:
                kb->numeric = !kb->numeric;
                if(kb->numeric)
                {
                    kb->caps = false;
                }
                kb->sel_row = 0U;
                kb->sel_key = 0U;
                break;
            case POOM_UI_KB_KEY_SPACE:
                poom_ui_keyboard_insert_char_(kb, ' ');
                break;
            case POOM_UI_KB_KEY_OK:
                action = POOM_UI_KEYBOARD_ACTION_ACCEPT;
                break;
            default:
                break;
        }
    }
    else if(button == BTN_B)
    {
    }

    return action;
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] row Parameter passed to the function.
 * @param[in] y Parameter passed to the function.
 * @param[in] sel_key Parameter passed to the function.
 * @param[in] caps Parameter passed to the function.
 * @return void
 */
static void poom_ui_keyboard_draw_key_row_(const poom_ui_kb_row_t *row, int16_t y, uint8_t sel_key, bool caps)
{
    int16_t x = 0;

    if((row == NULL) || (row->key_count == 0U))
    {
        return;
    }

    for(uint8_t i = 0U; i < row->key_count; i++)
    {
        const poom_ui_kb_key_t *key = &row->keys[i];
        const int16_t w_px = (int16_t)(key->w_cols * POOM_UI_KB_CELL_W);

        const char *label = key->label;
        char char_label[2] = {0};
        if(label == NULL)
        {
            char c = key->ch;
            if(caps && (c >= 'a') && (c <= 'z'))
            {
                c = (char)(c - 'a' + 'A');
            }
            char_label[0] = c;
            char_label[1] = '\0';
            label = char_label;
        }

        const int16_t label_px = (int16_t)(strlen(label) * 6U);
        int16_t x_text = (int16_t)(x + ((w_px - label_px) / 2));
        if(x_text < x)
        {
            x_text = x;
        }

        poom_arduboy_set_cursor(x_text, (int16_t)(y + 2));
        (void)poom_arduboy_print(label);

        if(i == sel_key)
        {
            poom_arduboy_fill_rect(x, y, w_px, POOM_UI_KB_CELL_H, INVERT);
        }

        x = (int16_t)(x + w_px);
    }
}

void poom_ui_keyboard_draw(const poom_ui_keyboard_t *kb)
{
    char title[22];
    char pass_view[22];
    const poom_ui_kb_layout_t layout = poom_ui_keyboard_layout_(kb);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_draw_rect(0, 0, ARDUBOY_WIDTH, 55, WHITE);

    (void)snprintf(title,
                   sizeof(title),
                   "To:%-.12s",
                   (kb != NULL && kb->target_label != NULL) ? kb->target_label : "");
    poom_arduboy_set_cursor(3, 1);
    (void)poom_arduboy_print(title);

    poom_arduboy_set_cursor(90, 1);
    (void)poom_arduboy_print((kb != NULL && kb->numeric) ? "123" : "ABC");
    poom_arduboy_set_cursor(112, 1);
    (void)poom_arduboy_print((kb != NULL && kb->caps) ? "^" : " ");
    poom_arduboy_set_cursor(120, 1);
    (void)poom_arduboy_print(F("X"));
    poom_arduboy_fill_rect(1, 1, ARDUBOY_WIDTH - 2, 8, INVERT);

    poom_arduboy_draw_rect(4, 10, 120, 10, WHITE);
    (void)memset(pass_view, 0, sizeof(pass_view));
    if((kb != NULL) && (kb->text != NULL))
    {
        const size_t max_visible = 18U;
        if(kb->text_len > max_visible)
        {
            const size_t tail = max_visible - 3U;
            const size_t start = kb->text_len - tail;
            (void)snprintf(pass_view, sizeof(pass_view), "...%.*s_", (int)tail, &kb->text[start]);
        }
        else
        {
            (void)snprintf(pass_view, sizeof(pass_view), "%s_", kb->text);
        }
    }
    else
    {
        (void)snprintf(pass_view, sizeof(pass_view), "_");
    }
    poom_arduboy_set_cursor(6, 12);
    (void)poom_arduboy_print(pass_view);

    const int16_t y0 = 22;
    poom_ui_keyboard_draw_key_row_(&layout.rows[0], y0, (kb != NULL && kb->sel_row == 0U) ? kb->sel_key : 255U,
                                   (kb != NULL) ? kb->caps : false);
    poom_ui_keyboard_draw_key_row_(&layout.rows[1],
                                   (int16_t)(y0 + POOM_UI_KB_CELL_H),
                                   (kb != NULL && kb->sel_row == 1U) ? kb->sel_key : 255U,
                                   (kb != NULL) ? kb->caps : false);
    poom_ui_keyboard_draw_key_row_(&layout.rows[2],
                                   (int16_t)(y0 + 2 * POOM_UI_KB_CELL_H),
                                   (kb != NULL && kb->sel_row == 2U) ? kb->sel_key : 255U,
                                   (kb != NULL) ? kb->caps : false);

    poom_arduboy_draw_rect(0, 0, ARDUBOY_WIDTH, 55, WHITE);

    poom_arduboy_set_cursor(2, 56);
    (void)poom_arduboy_print(F("A:SEL  B:BACK"));
    poom_arduboy_set_cursor(92, 56);
    (void)poom_arduboy_print(F("OK:GO"));

    poom_arduboy_display();
}
