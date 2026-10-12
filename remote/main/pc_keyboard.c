/* Traditional PC keyboard. Modifiers are selected for the next complete
 * key transaction, so navigating away never leaves Ctrl/Alt/Shift held. */
#include "remote.h"
#include "hardware/board.h"
#include "core/lv_port.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"

#include <ctype.h>
#include <stdlib.h>

typedef struct
{
    const char *label;
    uint32_t character;
    uint8_t code;
} pc_key_t;

#define CHAR(label, cp) {label, cp, 0}
#define KEY(label, code) {label, 0, code}
static const pc_key_t s_keys[3][27] = {
    {
        CHAR("q", 'q'), CHAR("w", 'w'), CHAR("e", 'e'),
        CHAR("r", 'r'), CHAR("t", 't'), CHAR("y", 'y'),
        CHAR("u", 'u'), CHAR("i", 'i'), CHAR("o", 'o'),
        CHAR("p", 'p'), CHAR("a", 'a'), CHAR("s", 's'),
        CHAR("d", 'd'), CHAR("f", 'f'), CHAR("g", 'g'),
        CHAR("h", 'h'), CHAR("j", 'j'), CHAR("k", 'k'),
        CHAR("l", 'l'), CHAR("z", 'z'), CHAR("x", 'x'),
        CHAR("c", 'c'), CHAR("v", 'v'), CHAR("b", 'b'),
        CHAR("n", 'n'), CHAR("m", 'm'), KEY(LV_SYMBOL_BACKSPACE, HIDL_KEY_BACKSPACE),
    },
    {
        CHAR("1", '1'),   CHAR("2", '2'),  CHAR("3", '3'),
        CHAR("4", '4'),   CHAR("5", '5'),  CHAR("6", '6'),
        CHAR("7", '7'),   CHAR("8", '8'),  CHAR("9", '9'),
        CHAR("0", '0'),   CHAR("@", '@'),  CHAR("#", '#'),
        CHAR("!", '!'),   CHAR("?", '?'),  CHAR("+", '+'),
        CHAR("-", '-'),   CHAR("_", '_'),  CHAR("/", '/'),
        CHAR("\\", '\\'), CHAR(".", '.'),  CHAR(",", ','),
        CHAR("'", '\''),  CHAR("\"", '"'), CHAR(":", ':'),
        CHAR(";", ';'),   CHAR("=", '='),  KEY(LV_SYMBOL_BACKSPACE, HIDL_KEY_BACKSPACE),
    },
    {
        KEY("F1", 0x3A),
        KEY("F2", 0x3B),
        KEY("F3", 0x3C),
        KEY("F4", 0x3D),
        KEY("F5", 0x3E),
        KEY("F6", 0x3F),
        KEY("F7", 0x40),
        KEY("F8", 0x41),
        KEY("F9", 0x42),
        KEY("F10", 0x43),
        KEY("F11", 0x44),
        KEY("F12", 0x45),
        KEY("Prt", 0x46),
        KEY("Caps", 0x39),
        KEY("Scr", 0x47),
        KEY("Ins", 0x49),
        KEY("Del", HIDL_KEY_DELETE),
        KEY("Home", HIDL_KEY_HOME),
        KEY("End", HIDL_KEY_END),
        KEY(LV_SYMBOL_LEFT, HIDL_KEY_LEFT),
        KEY(LV_SYMBOL_DOWN, HIDL_KEY_DOWN),
        KEY(LV_SYMBOL_UP, HIDL_KEY_UP),
        KEY(LV_SYMBOL_RIGHT, HIDL_KEY_RIGHT),
        KEY("Menu", 0x65),
        KEY("Pause", 0x48),
        KEY("PgUp", HIDL_KEY_PAGE_UP),
        KEY("PgDn", HIDL_KEY_PAGE_DOWN),
    },
};

typedef struct pc_keyboard pc_keyboard_t;
typedef struct
{
    pc_keyboard_t *keyboard;
    unsigned index;
} pc_event_t;

struct pc_keyboard
{
    lv_obj_t *labels[37];
    lv_obj_t *buttons[37];
    lv_obj_t *status;
    lv_timer_t *timer;
    pc_event_t events[37];
    unsigned layer;
    uint8_t modifiers;
    uint32_t version;
};

static const uint8_t s_modifiers[] = {HIDL_MOD_CTRL, HIDL_MOD_ALT, HIDL_MOD_ALTGR, HIDL_MOD_SHIFT, HIDL_MOD_GUI};

static void pc_refresh(pc_keyboard_t *kb)
{
    for (unsigned i = 0; i < 27; i++)
    {
        const pc_key_t *key = &s_keys[kb->layer][i];
        lv_obj_set_style_text_font(kb->labels[i], kb->layer == 2 ? UI_FONT_SMALL : UI_FONT_BODY, 0);
        if (kb->layer == 0 && key->character && (kb->modifiers & HIDL_MOD_SHIFT))
        {
            char text[2] = {(char)toupper((int)key->character), '\0'};
            lv_label_set_text(kb->labels[i], text);
        }
        else
        {
            lv_label_set_text(kb->labels[i], key->label);
        }
    }
    for (unsigned i = 0; i < 5; i++)
    {
        lv_obj_set_style_bg_color(kb->buttons[27 + i],
                                  kb->modifiers & s_modifiers[i] ? ui_accent() : ui_color(UI_COLOR_CARD_HI), 0);
    }
    lv_label_set_text(kb->labels[33], kb->layer == 0 ? "123" : kb->layer == 1 ? "Fn" : "abc");
    lv_label_set_text(kb->status, hid_link_state() == HID_LINK_CONNECTED
                                      ? (kb->modifiers ? "Modificatori: prossimo tasto" : "Connesso")
                                      : "Associa Remote");
}

static void pc_pressed(lv_event_t *event)
{
    pc_event_t *key_event = lv_event_get_user_data(event);
    pc_keyboard_t *kb = key_event->keyboard;
    unsigned i = key_event->index;
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && (i >= 27 || s_keys[kb->layer][i].code == 0))
    {
        return;
    }
    if (i >= 27 && i < 32)
    {
        kb->modifiers ^= s_modifiers[i - 27];
    }
    else if (i == 33)
    {
        kb->layer = (kb->layer + 1) % 3;
    }
    else
    {
        bool accepted;
        if (i < 27)
        {
            const pc_key_t *key = &s_keys[kb->layer][i];
            accepted = key->character ? hid_link_character(key->character, kb->modifiers)
                                      : hid_link_key(kb->modifiers, key->code);
        }
        else
        {
            static const uint8_t codes[] = {HIDL_KEY_ESC, 0, HIDL_KEY_TAB, HIDL_KEY_SPACE, HIDL_KEY_ENTER};
            accepted = hid_link_key(kb->modifiers, codes[i - 32]);
        }
        if (remote_send_ok(accepted))
        {
            kb->modifiers = 0;
        }
    }
    pc_refresh(kb);
}

static void pc_back(lv_event_t *event)
{
    (void)event;
    app_back();
}

static void pc_tick(lv_timer_t *timer)
{
    pc_keyboard_t *kb = lv_timer_get_user_data(timer);
    if (kb->version != hid_link_version())
    {
        kb->version = hid_link_version();
        kb->modifiers = 0;
        pc_refresh(kb);
    }
}

static void pc_deleted(lv_event_t *event)
{
    pc_keyboard_t *kb = lv_event_get_user_data(event);
    lv_timer_delete(kb->timer);
    free(kb);
}

static lv_obj_t *pc_button(lv_obj_t *screen, ui_key_rect_t rect, const char *text, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, rect.x + 2, rect.y + 2);
    lv_obj_set_size(button, rect.w - 4, rect.h - 4);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_gesture_bubble(button, false);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_center(label);
    return button;
}

static void pc_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_set_scrollable(screen, false);
    pc_keyboard_t *kb = calloc(1, sizeof(*kb));
    if (kb == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    int32_t w = board_info()->width, h = board_info()->height;
    bool round = board_info()->round;
    ui_key_rect_t header = ui_keyboard_band(w, h, round, h * 8 / 100, h * 10 / 100, 6);
    pc_button(screen, (ui_key_rect_t){header.x, header.y, 36, header.h}, LV_SYMBOL_LEFT, pc_back, NULL);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Tastiera PC");
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_pos(title, header.x + 42, header.y + 8);
    kb->status = lv_label_create(screen);
    lv_obj_set_style_text_font(kb->status, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(kb->status, LV_TEXT_ALIGN_CENTER, 0);
    ui_key_rect_t status = ui_keyboard_band(w, h, round, h * 19 / 100, h * 5 / 100, 6);
    lv_obj_set_pos(kb->status, status.x, status.y);
    lv_obj_set_width(kb->status, status.w);
    static const unsigned counts[] = {10, 9, 8};
    static const char *const modifier_names[] = {"Ctrl", "Alt", "AltGr", "Shift", "Win"};
    static const char *const control_names[] = {"Esc", "123", "Tab", "spazio", LV_SYMBOL_NEW_LINE};
    unsigned index = 0;
    for (unsigned row = 0; row < 5; row++)
    {
        unsigned count = row < 3 ? counts[row] : 5;
        int32_t y = h * (row < 3 ? 25 + 12 * row : row == 3 ? 62 : 74) / 100;
        int32_t height = h * (row < 3 ? 11 : row == 3 ? 10 : 13) / 100;
        ui_key_rect_t band = ui_keyboard_band(w, h, round, y, height, 6);
        unsigned units = row == 4 ? 6 : count, position = 0;
        for (unsigned column = 0; column < count; column++, index++)
        {
            unsigned weight = row == 4 && column == 3 ? 2 : 1;
            int32_t x0 = band.w * position / units;
            position += weight;
            int32_t x1 = band.w * position / units;
            const char *label = row < 3 ? s_keys[0][index].label
                                : row == 3
                                    ? (column == 4 && remote_settings()->host_os ? "Cmd" : modifier_names[column])
                                    : control_names[column];
            kb->events[index] = (pc_event_t){kb, index};
            kb->buttons[index] = pc_button(screen, (ui_key_rect_t){band.x + x0, band.y, x1 - x0, band.h}, label,
                                           pc_pressed, &kb->events[index]);
            kb->labels[index] = lv_obj_get_child(kb->buttons[index], 0);
            if (row < 3)
            {
                lv_obj_set_style_text_font(kb->labels[index], UI_FONT_BODY, 0);
                lv_obj_add_event_cb(kb->buttons[index], pc_pressed, LV_EVENT_LONG_PRESSED_REPEAT, &kb->events[index]);
            }
        }
    }
    kb->version = hid_link_version();
    kb->timer = lv_timer_create(pc_tick, 300, kb);
    lv_obj_add_event_cb(screen, pc_deleted, LV_EVENT_DELETE, kb);
    pc_refresh(kb);
}

const app_t remote_pc_keyboard_app = {
    .id = "remote.pc",
    .name = "Tastiera PC",
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
    .create = pc_create,
};
