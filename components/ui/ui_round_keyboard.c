/* Text-entry keyboard for round panels. All keys use the same circle-aware
 * bands as Remote; no essential control is placed in a clipped corner. */
#include "ui/ui.h"
#include "ui/keyboard_layout.h"
#include "core/lv_port.h"
#include "hardware/board.h"

#include <ctype.h>
#include <stdlib.h>

typedef struct round_keyboard round_keyboard_t;
typedef struct
{
    round_keyboard_t *keyboard;
    int index;
} round_key_t;

struct round_keyboard
{
    lv_obj_t *screen;
    lv_obj_t *textarea;
    lv_obj_t *labels[34];
    round_key_t events[34];
    unsigned layer;
    bool upper;
    bool accented;
};

static const char *const s_keys[3][28] = {
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "a", "s", "d", "f",
     "g", "h", "j", "k", "l", "z", "x", "c", "v", "b", "n", "m", ",", "."},
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "è", "é", "à", "ò",
     "ì", "ù", "?", "!", "@", "#", "€", "(", ")", "-", "+", "/", ":", "'"},
    {"*",  "&", "%", "=", "\"", "_", "~", "<", ">", "[", "]", "{", "}", "|",
     "\\", "^", "`", "$", "£",  "°", "§", "!", "?", "@", "#", ";", ",", "."},
};

static void refresh(round_keyboard_t *kb)
{
    for (int i = 0; i < 28; i++)
    {
        const char *text = s_keys[kb->layer][i];
        if (kb->layer == 0 && kb->upper && text[1] == '\0')
        {
            char upper[2] = {(char)toupper((unsigned char)text[0]), '\0'};
            lv_label_set_text(kb->labels[i], upper);
        }
        else
        {
            lv_label_set_text(kb->labels[i], text);
        }
    }
    lv_label_set_text(kb->labels[28], kb->layer == 0 ? LV_SYMBOL_UP : kb->layer == 1 ? "#+=" : "123");
    lv_obj_set_style_text_color(kb->labels[28], kb->upper && kb->layer == 0 ? ui_accent() : ui_color(UI_COLOR_TEXT), 0);
    lv_label_set_text(kb->labels[29], kb->layer == 0 ? "123" : "abc");
}

static void key_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT &&
        code != LV_EVENT_CLICKED)
    {
        return;
    }
    round_key_t *key = lv_event_get_user_data(e);
    round_keyboard_t *kb = key->keyboard;
    int index = key->index;
    if (code == LV_EVENT_PRESSED)
    {
        kb->accented = false;
    }
    if (index == 32 && (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT))
    {
        lv_textarea_delete_char(kb->textarea);
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED && index < 28 && kb->layer == 0)
    {
        const char *text = s_keys[0][index];
        const char *accent = NULL;
        switch (text[0])
        {
        case 'a':
            accent = kb->upper ? "À" : "à";
            break;
        case 'e':
            accent = kb->upper ? "È" : "è";
            break;
        case 'i':
            accent = kb->upper ? "Ì" : "ì";
            break;
        case 'o':
            accent = kb->upper ? "Ò" : "ò";
            break;
        case 'u':
            accent = kb->upper ? "Ù" : "ù";
            break;
        }
        if (accent != NULL)
        {
            lv_textarea_add_text(kb->textarea, accent);
            kb->accented = true;
        }
        return;
    }
    if (code != LV_EVENT_CLICKED || kb->accented)
    {
        return;
    }
    if (index < 28)
    {
        const char *text = s_keys[kb->layer][index];
        if (kb->layer == 0 && kb->upper && text[1] == '\0')
        {
            char upper[2] = {(char)toupper((unsigned char)text[0]), '\0'};
            lv_textarea_add_text(kb->textarea, upper);
        }
        else
        {
            lv_textarea_add_text(kb->textarea, text);
        }
    }
    else if (index == 28)
    {
        if (kb->layer == 0)
        {
            kb->upper = !kb->upper;
        }
        else
        {
            kb->layer = kb->layer == 1 ? 2 : 1;
        }
        refresh(kb);
    }
    else if (index == 29)
    {
        kb->layer = kb->layer == 0 ? 1 : 0;
        refresh(kb);
    }
    else if (index == 30)
    {
        lv_obj_send_event(kb->screen, LV_EVENT_CANCEL, NULL);
    }
    else if (index == 31)
    {
        lv_textarea_add_text(kb->textarea, " ");
    }
    else if (index == 33)
    {
        lv_obj_send_event(kb->screen, LV_EVENT_READY, NULL);
    }
}

static void deleted(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void password_clicked(lv_event_t *e)
{
    round_keyboard_t *kb = lv_event_get_user_data(e);
    bool hidden = !lv_textarea_get_password_mode(kb->textarea);
    lv_textarea_set_password_mode(kb->textarea, hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_current_target_obj(e), 0),
                      hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

bool ui_round_keyboard_create(lv_obj_t *screen, lv_obj_t *textarea, const char *title)
{
    round_keyboard_t *kb = calloc(1, sizeof(*kb));
    if (kb == NULL)
    {
        return false;
    }
    kb->screen = screen;
    kb->textarea = textarea;
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, kb);
    int32_t width = board_info()->width;
    int32_t height = board_info()->height;
    ui_keyboard_layout_t layout;
    ui_keyboard_layout_init(&layout, width, height, true);
    lv_obj_t *heading = lv_label_create(screen);
    lv_label_set_text(heading, title);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(heading, layout.header.w);
    lv_obj_set_height(heading, layout.header.h);
    lv_obj_set_style_text_font(heading, UI_FONT_BODY, 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, layout.header.x, layout.header.y);
    if (lv_textarea_get_password_mode(textarea))
    {
        lv_obj_set_width(heading, layout.header.w - 40);
        lv_obj_t *eye = lv_obj_create(screen);
        lv_obj_remove_style_all(eye);
        lv_obj_set_pos(eye, layout.header.x + layout.header.w - 36, layout.header.y);
        lv_obj_set_size(eye, 36, layout.header.h);
        lv_obj_set_scrollable(eye, false);
        lv_obj_add_event_cb(eye, password_clicked, LV_EVENT_CLICKED, kb);
        lv_port_set_focusable(eye);
        lv_obj_t *symbol = lv_label_create(eye);
        lv_label_set_text(symbol, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(symbol, UI_FONT_BODY, 0);
        lv_obj_center(symbol);
    }
    ui_key_rect_t field = layout.suggestions;
    lv_obj_set_pos(textarea, field.x, field.y);
    lv_obj_set_size(textarea, field.w, field.h);
    lv_obj_set_style_text_font(textarea, UI_FONT_SMALL, 0);
    lv_obj_set_style_pad_ver(textarea, 2, 0);
    static const char *const controls[] = {LV_SYMBOL_UP,        "123",       LV_SYMBOL_CLOSE, "spazio",
                                           LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK};
    for (int i = 0; i < 34; i++)
    {
        ui_key_rect_t rect = i < 28 ? layout.keys[i] : layout.controls[i - 28];
        if (i < 28)
        {
            rect.y += layout.pad.y;
        }
        lv_obj_t *button = lv_obj_create(screen);
        lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, rect.x + 2, rect.y + 2);
        lv_obj_set_size(button, rect.w - 4, rect.h - 4);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, ui_color(i == 33 ? UI_COLOR_BLUE : UI_COLOR_CARD), 0);
        lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
        lv_obj_set_scrollable(button, false);
        lv_obj_set_gesture_bubble(button, false);
        kb->events[i] = (round_key_t){kb, i};
        lv_obj_add_event_cb(button, key_event, LV_EVENT_ALL, &kb->events[i]);
        lv_port_set_focusable(button);
        kb->labels[i] = lv_label_create(button);
        lv_label_set_text(kb->labels[i], i < 28 ? s_keys[0][i] : controls[i - 28]);
        lv_obj_set_style_text_font(kb->labels[i], i < 28 ? UI_FONT_LARGE : UI_FONT_BODY, 0);
        lv_obj_center(kb->labels[i]);
    }
    return true;
}
