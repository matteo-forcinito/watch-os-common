/* HID control surfaces. Commands operate the focused host application;
 * a HID link cannot read its microphone, playback or slide state. */
#include "remote.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    lv_obj_t *status;
    lv_timer_t *timer;
} panel_t;

static void panel_tick(lv_timer_t *timer)
{
    panel_t *panel = lv_timer_get_user_data(timer);
    lv_label_set_text(panel->status, hid_link_state() == HID_LINK_CONNECTED ? "Connesso" : "Associa Remote");
    lv_obj_set_style_text_color(panel->status,
                                ui_color(hid_link_state() == HID_LINK_CONNECTED ? UI_COLOR_GREEN : UI_COLOR_ORANGE), 0);
}

static void panel_deleted(lv_event_t *e)
{
    panel_t *panel = lv_event_get_user_data(e);
    lv_timer_delete(panel->timer);
    free(panel);
}

static lv_obj_t *button_at(lv_obj_t *screen, ui_key_rect_t rect, const char *text, uint32_t color, lv_event_cb_t cb,
                           lv_event_code_t code, void *data)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, rect.x + 3, rect.y + 3);
    lv_obj_set_size(button, rect.w - 6, rect.h - 6);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, ui_color(color), 0);
    lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_gesture_bubble(button, false);
    lv_obj_add_event_cb(button, cb, code, data);
    lv_port_set_focusable(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, rect.w - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(label);
    return button;
}

/* Lowest usable line: round panels stop at the last comfortable chord. */
static int32_t panel_bottom(void)
{
    int32_t h = remote_screen_h();
    return board_info()->round ? h * 91 / 100 : h - 4;
}

static panel_t *panel_create(lv_obj_t *screen, const char *title)
{
    lv_obj_set_scrollable(screen, false);
    int32_t w = remote_screen_w();
    int32_t h = remote_screen_h();
    ui_key_rect_t header = remote_header_band();
    remote_switcher_button(screen);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, title);
    /* Centred on the screen, clear of the switcher on both sides. */
    lv_obj_set_width(label, header.w - 2 * 52);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(label, strlen(title) > 12 ? UI_FONT_BODY : UI_FONT_LARGE, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(label);
    lv_obj_set_pos(label, header.x + 52, header.y + (header.h - lv_obj_get_height(label)) / 2);
    panel_t *panel = calloc(1, sizeof(*panel));
    if (panel == NULL)
        return NULL;
    panel->status = lv_label_create(screen);
    int32_t status_h = lv_font_get_line_height(UI_FONT_SMALL);
    ui_key_rect_t footer =
        ui_keyboard_band(w, h, board_info()->round, panel_bottom() - status_h, status_h, 8);
    lv_obj_set_pos(panel->status, footer.x, footer.y);
    lv_obj_set_width(panel->status, footer.w);
    lv_obj_set_style_text_align(panel->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(panel->status, UI_FONT_SMALL, 0);
    panel->timer = lv_timer_create(panel_tick, 300, panel);
    lv_obj_add_event_cb(screen, panel_deleted, LV_EVENT_DELETE, panel);
    panel_tick(panel->timer);
    return panel;
}

/* Four rows with two generous controls each, between the header and the status
 * line; every row fits its circular band. */
static ui_key_rect_t grid_rect(int index)
{
    int32_t h = remote_screen_h();
    ui_key_rect_t header = remote_header_band();
    int32_t top = header.y + header.h + 4;
    int32_t bottom = panel_bottom() - lv_font_get_line_height(UI_FONT_SMALL) - 4;
    int row = index / 2;
    int32_t y0 = top + (bottom - top) * row / 4, y1 = top + (bottom - top) * (row + 1) / 4;
    ui_key_rect_t band = ui_keyboard_band(remote_screen_w(), h, board_info()->round, y0, y1 - y0, 8);
    int32_t middle = band.w / 2;
    return (ui_key_rect_t){band.x + (index % 2 ? middle : 0), band.y, index % 2 ? band.w - middle : middle, band.h};
}

static bool mac(void)
{
    return remote_settings()->host_os == 1;
}
static uint8_t command_modifier(void)
{
    return mac() ? HIDL_MOD_GUI : HIDL_MOD_CTRL;
}

static void media_clicked(lv_event_t *e)
{
    static const uint16_t usages[] = {HIDL_MEDIA_PLAY_PAUSE,
                                      HIDL_MEDIA_MUTE,
                                      HIDL_MEDIA_PREVIOUS,
                                      HIDL_MEDIA_NEXT,
                                      HIDL_MEDIA_VOLUME_DOWN,
                                      HIDL_MEDIA_VOLUME_UP,
                                      0x00B7,
                                      0x00B8};
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    remote_send_ok(hid_link_media(usages[i]));
}

static void control_knob(lv_event_t *event);

static void media_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    panel_create(screen, "Media");
    lv_obj_add_event_cb(screen, control_knob, lv_port_knob_event(), (void *)(intptr_t)1);
    static const char *const names[] = {LV_SYMBOL_PLAY " / " LV_SYMBOL_PAUSE,
                                        "Mute",
                                        LV_SYMBOL_PREV " Precedente",
                                        "Successivo " LV_SYMBOL_NEXT,
                                        "Volume -",
                                        "Volume +",
                                        "Stop",
                                        "Espelli"};
    for (int i = 0; i < 8; i++)
    {
        lv_obj_t *button = button_at(screen, grid_rect(i), names[i], i == 0 ? UI_COLOR_PINK : UI_COLOR_CARD,
                                     media_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i == 4 || i == 5)
        {
            lv_obj_add_event_cb(button, media_clicked, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
        }
    }
}

static void control_knob(lv_event_t *event)
{
    if (lv_port_focus_mode())
    {
        return;
    }
    lv_port_knob_t *knob = lv_event_get_param(event);
    int mode = (int)(intptr_t)lv_event_get_user_data(event);
    int count = abs(knob->delta);
    if (count > 12)
    {
        count = 12;
    }
    knob->handled = true;
    for (int i = 0; i < count; i++)
    {
        bool accepted = mode == 1 ? hid_link_media(knob->delta > 0 ? HIDL_MEDIA_VOLUME_UP : HIDL_MEDIA_VOLUME_DOWN)
                                  : hid_link_key(0, knob->delta > 0 ? HIDL_KEY_RIGHT : HIDL_KEY_LEFT);
        if (!remote_send_ok(accepted))
        {
            break;
        }
    }
}

static void presenter_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t modifiers = 0;
    static const uint8_t keys[] = {HIDL_KEY_LEFT, HIDL_KEY_RIGHT, 0x3E, 0x3E, 0x05, 0x1A, HIDL_KEY_HOME, HIDL_KEY_ESC};
    uint8_t key = keys[i];
    if (i == 2 || i == 3)
    {
        if (mac())
        {
            key = HIDL_KEY_ENTER;
            modifiers = HIDL_MOD_GUI | (i == 2 ? HIDL_MOD_SHIFT : 0);
        }
        else if (i == 3)
            modifiers = HIDL_MOD_SHIFT;
    }
    remote_send_ok(hid_link_key(modifiers, key));
}

static void presenter_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    panel_create(screen, "Presentazioni");
    lv_obj_add_event_cb(screen, control_knob, lv_port_knob_event(), (void *)(intptr_t)2);
    static const char *const names[] = {"Precedente",   "Successiva",     "Avvia",       "Da qui",
                                        "Schermo nero", "Schermo bianco", "Prima slide", "Termina"};
    for (int i = 0; i < 8; i++)
    {
        button_at(screen, grid_rect(i), names[i], i == 1 ? UI_COLOR_ORANGE : UI_COLOR_CARD, presenter_clicked,
                  LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void meeting_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t modifier, key;
    if (remote_settings()->meeting == 1)
    {
        static const uint8_t keys[] = {0x04, 0x19, 0x1C, 0x16, 0x0B, 0x14}; /* A V Y S H Q */
        key = keys[i];
        modifier = mac() ? HIDL_MOD_GUI | HIDL_MOD_SHIFT : HIDL_MOD_ALT;
        if (mac() && i == 2)
            modifier = HIDL_MOD_ALT;
        if (mac() && i == 5)
        {
            modifier = HIDL_MOD_GUI;
            key = 0x1A;
        }
    }
    else
    {
        static const uint8_t keys[] = {0x10, 0x12, 0x0E, 0x08, HIDL_KEY_SPACE, 0x0B}; /* M O K E Space H */
        key = keys[i];
        modifier = command_modifier() | HIDL_MOD_SHIFT;
    }
    remote_send_ok(hid_link_key(modifier, key));
}

static void meeting_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = remote_page(screen, remote_settings()->meeting ? "Zoom" : "Teams");
    ui_text(page, "Apri la riunione sul computer. I pulsanti alternano lo stato; Remote non legge microfono o camera.",
            true);
    static const char *const names[] = {"Microfono: alterna", "Camera: alterna", "Alza / abbassa mano",
                                        "Condivisione",       "Toolbar / chat",  "Lascia riunione"};
    for (int i = 0; i < 6; i++)
    {
        static const char *const icons[] = {LV_SYMBOL_AUDIO, LV_SYMBOL_VIDEO,   LV_SYMBOL_UP,
                                            LV_SYMBOL_UPLOAD, LV_SYMBOL_LIST, LV_SYMBOL_CLOSE};
        ui_row(page, icons[i], i == 5 ? UI_COLOR_RED : UI_COLOR_INDIGO, names[i], NULL, meeting_clicked,
               (void *)(intptr_t)i);
    }
}

static void shortcut_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const uint8_t keys[] = {0x06, 0x19, 0x1B, 0x1D, 0x04, 0x09, HIDL_KEY_TAB, 0x0F};
    uint8_t modifier = command_modifier();
    if (i == 6)
        modifier = mac() ? HIDL_MOD_GUI : HIDL_MOD_ALT;
    if (i == 7)
        modifier = mac() ? HIDL_MOD_GUI : HIDL_MOD_CTRL; /* address bar */
    remote_send_ok(hid_link_key(modifier, keys[i]));
}

/* Static shortcut descriptions stay valid across queued app navigation. */
typedef struct
{
    const char *name;
    uint8_t win_mod, win_key, mac_mod, mac_key;
} shortcut_t;
#define C HIDL_MOD_CTRL
#define A HIDL_MOD_ALT
#define S HIDL_MOD_SHIFT
#define G HIDL_MOD_GUI
#define SAME(name, mods, key) {name, mods, key, mods, key}
#define CMD(name, mods, key) {name, C | (mods), key, G | (mods), key}
static const shortcut_t s_shortcuts[4][8] = {
    {CMD("Ripeti", S, 0x1D), CMD("Salva", 0, 0x16), CMD("Apri", 0, 0x12), CMD("Nuovo", 0, 0x11), CMD("Stampa", 0, 0x13),
     CMD("Grassetto", 0, 0x05), CMD("Corsivo", 0, 0x0C), CMD("Sottolineato", 0, 0x18)},
    {{"Scheda successiva", C, HIDL_KEY_TAB, G | A, HIDL_KEY_RIGHT},
     {"Scheda precedente", C | S, HIDL_KEY_TAB, G | A, HIDL_KEY_LEFT},
     CMD("Nuova scheda", 0, 0x17), CMD("Chiudi scheda", 0, 0x1A), CMD("Riapri scheda", S, 0x17),
     CMD("Ricarica", 0, 0x15), CMD("Cerca nella pagina", 0, 0x09), {"Download (Chrome)", C, 0x0D, G | S, 0x0D}},
    {{"Parola precedente", C, HIDL_KEY_LEFT, A, HIDL_KEY_LEFT},
     {"Parola successiva", C, HIDL_KEY_RIGHT, A, HIDL_KEY_RIGHT},
     {"Seleziona parola prima", C | S, HIDL_KEY_LEFT, A | S, HIDL_KEY_LEFT},
     {"Seleziona parola dopo", C | S, HIDL_KEY_RIGHT, A | S, HIDL_KEY_RIGHT},
     {"Inizio documento", C, HIDL_KEY_HOME, G, HIDL_KEY_UP},
     {"Fine documento", C, HIDL_KEY_END, G, HIDL_KEY_DOWN},
     SAME("Pagina precedente", 0, HIDL_KEY_PAGE_UP),
     SAME("Pagina successiva", 0, HIDL_KEY_PAGE_DOWN)},
    {{"Finestra successiva", A, HIDL_KEY_TAB, G, HIDL_KEY_TAB},
     {"Finestra precedente", A | S, HIDL_KEY_TAB, G | S, HIDL_KEY_TAB},
     {"Chiudi finestra", A, 0x3D, G, 0x1A},
     {"Minimizza", G, HIDL_KEY_DOWN, G, 0x10},
     {"Panoramica finestre", G, HIDL_KEY_TAB, C, HIDL_KEY_UP},
     {"Massimizza / scrivania", G, HIDL_KEY_UP, C, HIDL_KEY_DOWN},
     {"Sinistra / spazio prima", G, HIDL_KEY_LEFT, C, HIDL_KEY_LEFT},
     {"Destra / spazio dopo", G, HIDL_KEY_RIGHT, C, HIDL_KEY_RIGHT}}};
#undef C
#undef A
#undef S
#undef G
#undef SAME
#undef CMD

static void extra_shortcut_clicked(lv_event_t *e)
{
    const shortcut_t *shortcut = lv_event_get_user_data(e);
    remote_send_ok(
        hid_link_key(mac() ? shortcut->mac_mod : shortcut->win_mod, mac() ? shortcut->mac_key : shortcut->win_key));
}

static void shortcut_page(lv_obj_t *screen, unsigned category, const char *title)
{
    lv_obj_t *page = ui_page(screen, title);
    if (category == 3)
        ui_text(page,
                mac() ? "Comandi macOS. Dipendono dalle impostazioni del Mac."
                      : "Comandi Windows. Su Linux dipendono dal desktop.",
                true);
    for (unsigned i = 0; i < 8; i++)
        ui_row(page, LV_SYMBOL_KEYBOARD, UI_COLOR_INDIGO, s_shortcuts[category][i].name, NULL, extra_shortcut_clicked,
               (void *)&s_shortcuts[category][i]);
}
static void edit_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    shortcut_page(screen, 0, "Testo e file");
}
static void browser_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    shortcut_page(screen, 1, "Browser");
}
static void navigate_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    shortcut_page(screen, 2, "Navigazione");
}
static void windows_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    shortcut_page(screen, 3, "Finestre");
}
static const app_t s_shortcut_apps[] = {
    {.id = "remote.shortcuts.edit", .name = "Testo e file", .flags = APP_FLAG_HIDDEN, .create = edit_create},
    {.id = "remote.shortcuts.browser", .name = "Browser", .flags = APP_FLAG_HIDDEN, .create = browser_create},
    {.id = "remote.shortcuts.navigate", .name = "Navigazione", .flags = APP_FLAG_HIDDEN, .create = navigate_create},
    {.id = "remote.shortcuts.windows", .name = "Finestre", .flags = APP_FLAG_HIDDEN, .create = windows_create}};
static void shortcut_category(lv_event_t *e)
{
    app_open_app(lv_event_get_user_data(e), NULL);
}
static void shortcuts_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = remote_page(screen, "Scorciatoie");
    for (unsigned i = 0; i < 4; i++)
        ui_row(page, LV_SYMBOL_LIST, UI_COLOR_TEAL, s_shortcut_apps[i].name, NULL, shortcut_category,
               (void *)&s_shortcut_apps[i]);
    static const char *const names[] = {"Copia",           "Incolla", "Taglia",     "Annulla",
                                        "Seleziona tutto", "Cerca",   "Cambia app", "Indirizzo web"};
    ui_text(page, "Comandi rapidi", true);
    for (int i = 0; i < 8; i++)
        ui_row(page, LV_SYMBOL_KEYBOARD, UI_COLOR_CARD, names[i], NULL, shortcut_clicked, (void *)(intptr_t)i);
}

/* ------------------------------------------------------------------ trackpad
 *
 * The whole screen is the pad and gestures replace the buttons (one-touch panels):
 *   move                              pointer
 *   tap                               left click; two taps make a double click
 *   hold still for 0.45 s             right click
 *   tap, then touch again and move    drag with the left button held until the finger lifts
 *   along an edge                     scroll, volume, brightness (remote_gestures.c) */

#define PAD_SLOP 8         /* px before a touch counts as a move */
#define PAD_HOLD_MS 450    /* still this long: right click */
#define PAD_TAP_MS 450     /* longer touches are never taps */
#define PAD_REDRAG_MS 300  /* a touch this soon after a tap can start a drag */

typedef enum
{
    TOUCH_IDLE,
    TOUCH_UNDECIDED, /* down, not moved yet: tap, hold or the start of a move */
    TOUCH_POINTER,
    TOUCH_DRAG,
    TOUCH_EDGE,
    TOUCH_DONE, /* right click sent: the rest of this touch is ignored */
} touch_t;

typedef struct
{
    lv_obj_t *pad, *hint, *pill, *marks[4];
    lv_timer_t *timer;
    lv_point_t start, last;
    float dx, dy; /* pointer counts not sent yet */
    float along;  /* edge travel not turned into steps yet */
    uint32_t pressed_at, released_at;
    uint32_t version;
    remote_edge_t edge; /* edge under the touch-down point */
    touch_t state;
    bool after_tap; /* this touch began right after a tap */
    bool tapped;    /* the last touch was a tap */
    bool dragging;  /* left button held */
} mouse_t;
static mouse_t *s_mouse;

static int8_t bounded(float value)
{
    return (int8_t)fmaxf(-127, fminf(127, value));
}

static void pill_show(mouse_t *mouse, const char *text)
{
    lv_label_set_text(lv_obj_get_child(mouse->pill, 0), text);
    lv_obj_remove_flag(mouse->pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouse->hint, LV_OBJ_FLAG_HIDDEN);
}

static void touch_ended(mouse_t *mouse)
{
    if (mouse->dragging)
        hid_link_mouse(0, 0, 0, 0);
    mouse->dragging = false;
    mouse->dx = mouse->dy = 0; /* nothing moves after the finger lifts */
    mouse->state = TOUCH_IDLE;
    lv_obj_add_flag(mouse->pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mouse->hint, LV_OBJ_FLAG_HIDDEN);
    remote_edge_light(mouse->marks, REMOTE_EDGE_NONE);
}

static void mouse_clear(void)
{
    if (s_mouse != NULL)
    {
        s_mouse->dx = s_mouse->dy = 0;
        s_mouse->tapped = false;
        touch_ended(s_mouse);
    }
}

static void mouse_tick(lv_timer_t *timer)
{
    mouse_t *mouse = lv_timer_get_user_data(timer);
    if ((app_current() != &remote_mouse_app && mouse->state != TOUCH_IDLE) || mouse->version != hid_link_version())
    {
        mouse->version = hid_link_version();
        mouse_clear();
        return;
    }
    /* Held still: right click, once, while the finger is still down. */
    if (mouse->state == TOUCH_UNDECIDED && !mouse->after_tap && lv_tick_elaps(mouse->pressed_at) >= PAD_HOLD_MS)
    {
        mouse->state = TOUCH_DONE;
        if (remote_send_ok(hid_link_click(2, 1)))
            pill_show(mouse, "Clic destro");
        return;
    }
    int8_t dx = bounded(mouse->dx), dy = bounded(mouse->dy);
    if ((dx || dy) && hid_link_pointer(dx, dy, mouse->dragging ? 1 : 0, 0, 0))
    {
        mouse->dx -= dx;
        mouse->dy -= dy;
    }
}

static void mouse_touch(lv_event_t *e)
{
    mouse_t *mouse = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED)
    {
        mouse->start = mouse->last = point;
        mouse->pressed_at = lv_tick_get();
        mouse->after_tap = mouse->tapped && lv_tick_elaps(mouse->released_at) < PAD_REDRAG_MS;
        mouse->tapped = false;
        mouse->edge = remote_edge_at(point);
        mouse->along = 0;
        mouse->state = TOUCH_UNDECIDED;
        return;
    }
    if (code == LV_EVENT_PRESSING)
    {
        int32_t dx = point.x - mouse->last.x, dy = point.y - mouse->last.y;
        mouse->last = point;
        if (mouse->state == TOUCH_UNDECIDED)
        {
            int32_t tx = point.x - mouse->start.x, ty = point.y - mouse->start.y;
            if (LV_ABS(tx) + LV_ABS(ty) < PAD_SLOP)
                return;
            bool along = remote_edge_claims(mouse->edge, tx, ty);
            if (mouse->after_tap && remote_send_ok(hid_link_mouse(0, 0, 1, 0)))
            {
                mouse->dragging = true;
                mouse->state = TOUCH_DRAG;
                pill_show(mouse, "Trascina");
            }
            else if (along)
            {
                mouse->state = TOUCH_EDGE;
                remote_edge_light(mouse->marks, mouse->edge);
                pill_show(mouse, remote_edge_name(mouse->edge));
                dx = tx; /* the slop travel counts too */
                dy = ty;
            }
            else
                mouse->state = TOUCH_POINTER;
        }
        if (mouse->state == TOUCH_EDGE)
            remote_edge_move(mouse->edge, remote_edge_vertical(mouse->edge) ? dy : dx, &mouse->along);
        else if (mouse->state == TOUCH_POINTER || mouse->state == TOUCH_DRAG)
        {
            float speed = remote_settings()->pointer_speed * 0.6f;
            mouse->dx = fmaxf(-512, fminf(512, mouse->dx + dx * speed));
            mouse->dy = fmaxf(-512, fminf(512, mouse->dy + dy * speed));
        }
        return;
    }
    /* Released or lost */
    bool tap =
        code == LV_EVENT_RELEASED && mouse->state == TOUCH_UNDECIDED && lv_tick_elaps(mouse->pressed_at) < PAD_TAP_MS;
    if (tap)
        remote_send_ok(hid_link_click(1, 1));
    mouse->tapped = tap && !mouse->after_tap; /* a third quick touch never drags */
    mouse->released_at = lv_tick_get();
    touch_ended(mouse);
}

static void mouse_extra(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const uint8_t buttons[] = {4, 1, 8, 16};
    if (i < 4)
        remote_send_ok(hid_link_click(buttons[i], i == 1 ? 2 : 1));
    else
    {
        int8_t wheel = i == 4 ? 3 : i == 5 ? -3 : 0;
        int8_t pan = i == 6 ? -3 : i == 7 ? 3 : 0;
        remote_send_ok(hid_link_pointer(0, 0, 0, wheel, pan));
    }
}

static void mouse_more_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Mouse: comandi");
    static const char *const names[] = {"Clic centrale", "Doppio clic", "Indietro",          "Avanti",
                                        "Scorri su",     "Scorri giù",  "Scorri a sinistra", "Scorri a destra"};
    static const char *const icons[] = {LV_SYMBOL_GPS, LV_SYMBOL_GPS,  LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
                                        LV_SYMBOL_UP,  LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT};
    for (int i = 0; i < 8; i++)
        ui_row(page, icons[i], UI_COLOR_TEAL, names[i], NULL, mouse_extra, (void *)(intptr_t)i);
    ui_text(page,
            "Sul pad: tocca per il clic, tieni premuto per il destro, tocca e poi trascina per spostare. "
            "Bordo destro e inferiore: scorrimento. Bordo sinistro: volume. Bordo superiore: luminosità, "
            "se il PC la regola da tastiera.",
            true);
}
static const app_t s_mouse_more_app = {
    .id = "remote.mouse.more", .name = "Comandi mouse", .flags = APP_FLAG_HIDDEN, .create = mouse_more_create};
static void mouse_more(lv_event_t *e)
{
    (void)e;
    app_open_app(&s_mouse_more_app, NULL);
}

static void mouse_deleted(lv_event_t *e)
{
    mouse_t *mouse = lv_event_get_user_data(e);
    lv_timer_delete(mouse->timer);
    if (s_mouse == mouse)
    {
        hid_link_mouse(0, 0, 0, 0);
        s_mouse = NULL;
    }
    free(mouse);
}

static void mouse_knob(lv_event_t *event)
{
    if (lv_port_focus_mode())
    {
        return;
    }
    mouse_t *mouse = lv_event_get_user_data(event);
    lv_port_knob_t *knob = lv_event_get_param(event);
    knob->handled = true;
    remote_send_ok(hid_link_pointer(0, 0, mouse->dragging ? 1 : 0, remote_wheel(bounded(knob->delta)), 0));
}

static void mouse_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    mouse_t *mouse = calloc(1, sizeof(*mouse));
    if (mouse == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    s_mouse = mouse;
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, mouse_knob, lv_port_knob_event(), mouse);
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool round = board_info()->round;

    mouse->pad = lv_obj_create(screen);
    lv_obj_remove_style_all(mouse->pad);
    lv_obj_set_size(mouse->pad, w, h);
    lv_obj_set_style_radius(mouse->pad, round ? LV_RADIUS_CIRCLE : 0, 0);
    lv_obj_set_style_bg_opa(mouse->pad, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mouse->pad, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_scrollable(mouse->pad, false);
    lv_obj_set_press_lock(mouse->pad, true);
    lv_obj_set_gesture_bubble(mouse->pad, false);
    lv_obj_add_event_cb(mouse->pad, mouse_touch, LV_EVENT_PRESSED, mouse);
    lv_obj_add_event_cb(mouse->pad, mouse_touch, LV_EVENT_PRESSING, mouse);
    lv_obj_add_event_cb(mouse->pad, mouse_touch, LV_EVENT_RELEASED, mouse);
    lv_obj_add_event_cb(mouse->pad, mouse_touch, LV_EVENT_PRESS_LOST, mouse);

    for (int i = 0; i < 4; i++)
        mouse->marks[i] = remote_edge_mark(mouse->pad, (remote_edge_t)i);

    mouse->hint = lv_label_create(mouse->pad);
    lv_label_set_text(mouse->hint, "Tocca: clic\nTieni premuto: destro\nTocca e trascina: sposta");
    lv_obj_set_width(mouse->hint, w * 62 / 100);
    lv_obj_set_style_text_font(mouse->hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(mouse->hint, ui_color(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(mouse->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(mouse->hint, 6, 0);
    lv_obj_center(mouse->hint);

    /* What the current touch does, in place of the hint while it lasts */
    mouse->pill = lv_obj_create(mouse->pad);
    lv_obj_remove_style_all(mouse->pill);
    lv_obj_set_size(mouse->pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(mouse->pill, 18, 0);
    lv_obj_set_style_pad_ver(mouse->pill, 8, 0);
    lv_obj_set_style_radius(mouse->pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mouse->pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mouse->pill, ui_accent(), 0);
    lv_obj_remove_flag(mouse->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *pill_text = lv_label_create(mouse->pill);
    lv_label_set_text(pill_text, "");
    lv_obj_set_style_text_font(pill_text, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(pill_text, lv_color_black(), 0);
    lv_obj_center(mouse->pill);
    lv_obj_add_flag(mouse->pill, LV_OBJ_FLAG_HIDDEN);

    /* Header over the pad: switcher on the left, more commands on the right */
    ui_key_rect_t head = remote_header_band();
    remote_switcher_button(screen);
    lv_obj_t *more = button_at(screen, (ui_key_rect_t){head.x + head.w - 50, head.y, 50, head.h}, LV_SYMBOL_BARS,
                               UI_COLOR_CARD_HI, mouse_more, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_radius(more, 14, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(more, 0), ui_accent(), 0);

    mouse->version = hid_link_version();
    mouse->timer = lv_timer_create(mouse_tick, 20, mouse);
    lv_obj_add_event_cb(screen, mouse_deleted, LV_EVENT_DELETE, mouse);
}

typedef struct
{
    lv_obj_t *keys[12];
    lv_obj_t *mode;
    bool latch;
    uint16_t buttons;
    uint8_t directions;
    uint32_t version;
    lv_timer_t *timer;
} gamepad_t;
static gamepad_t *s_gamepad;

static bool gamepad_send(gamepad_t *pad)
{
    int8_t x = ((pad->directions & 8) != 0) * 127 - ((pad->directions & 4) != 0) * 127;
    int8_t y = ((pad->directions & 2) != 0) * 127 - ((pad->directions & 1) != 0) * 127;
    return hid_link_gamepad(x, y, pad->buttons);
}

static const uint32_t s_pad_colors[] = {UI_COLOR_GREEN, UI_COLOR_RED, UI_COLOR_BLUE, UI_COLOR_YELLOW};
static void gamepad_refresh(gamepad_t *pad)
{
    for (unsigned i = 0; i < 12; i++)
    {
        bool active = i < 4 ? (pad->directions & (1 << i)) != 0 : (pad->buttons & (1 << (i - 4))) != 0;
        uint32_t color = i < 4 || i >= 8 ? UI_COLOR_CARD_HI : s_pad_colors[i - 4];
        lv_obj_set_style_bg_color(pad->keys[i], active ? ui_accent() : ui_color(color), 0);
        lv_obj_set_style_border_width(pad->keys[i], active ? 3 : 0, 0);
        lv_obj_set_style_border_color(pad->keys[i], lv_color_white(), 0);
    }
    lv_label_set_text(lv_obj_get_child(pad->mode, 0), pad->latch ? "Direzioni: fisse" : "Direzioni: premute");
    lv_obj_set_style_bg_color(pad->mode, pad->latch ? ui_accent() : ui_color(UI_COLOR_CARD), 0);
}
static void gamepad_button(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_gamepad == NULL)
        return;
    gamepad_t *pad = s_gamepad;
    bool down = lv_event_get_code(e) == LV_EVENT_PRESSED;
    if (i < 4)
    {
        if (pad->latch)
        {
            if (!down)
                return;
            pad->directions ^= 1 << i;
            static const uint8_t opposite[] = {1, 0, 3, 2};
            pad->directions &= ~(1 << opposite[i]);
        }
        else if (down)
            pad->directions |= 1 << i;
        else
            pad->directions &= ~(1 << i);
    }
    else if (down)
        pad->buttons |= 1 << (i - 4);
    else
        pad->buttons &= ~(1 << (i - 4));
    gamepad_refresh(pad);
    remote_send_ok(gamepad_send(pad));
}
static void gamepad_center(lv_event_t *e)
{
    if (s_gamepad == NULL)
        return;
    if (lv_event_get_user_data(e) != NULL)
        s_gamepad->latch = !s_gamepad->latch;
    s_gamepad->directions = 0;
    s_gamepad->buttons = 0;
    gamepad_refresh(s_gamepad);
    remote_send_ok(gamepad_send(s_gamepad));
}

static void gamepad_tick(lv_timer_t *timer)
{
    gamepad_t *pad = lv_timer_get_user_data(timer);
    if (pad->version != hid_link_version())
    {
        pad->version = hid_link_version();
        pad->buttons = 0;
        pad->directions = 0;
        gamepad_refresh(pad);
    }
    if (s_gamepad == pad && app_current() == &remote_gamepad_app)
        gamepad_send(pad);
    else if (pad->buttons || pad->directions)
    {
        pad->buttons = 0;
        pad->directions = 0;
        gamepad_send(pad);
    }
}

static void gamepad_deleted(lv_event_t *e)
{
    gamepad_t *pad = lv_event_get_user_data(e);
    lv_timer_delete(pad->timer);
    if (s_gamepad == pad)
    {
        hid_link_gamepad(0, 0, 0);
        s_gamepad = NULL;
    }
    free(pad);
}

static void gamepad_clear(void)
{
    if (s_gamepad != NULL)
    {
        s_gamepad->buttons = 0;
        s_gamepad->directions = 0;
        gamepad_refresh(s_gamepad);
    }
    hid_link_release_all();
}

static void gamepad_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    panel_t *panel = panel_create(screen, "Gamepad");
    if (panel != NULL && remote_landscape())
        lv_obj_add_flag(panel->status, LV_OBJ_FLAG_HIDDEN); /* its line holds L/R/Select/Start */
    gamepad_t *pad = calloc(1, sizeof(*pad));
    if (pad == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    s_gamepad = pad;
    static const char *const names[] = {
        LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "A", "B", "X", "Y", "L", "R", "Select", "Start"};
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool landscape = remote_landscape();
    /* Sizes follow the short side; in landscape L/R/Select/Start share one row. */
    int32_t unit = LV_MIN(w, h), size = unit * 13 / 100;
    ui_key_rect_t head = remote_header_band();
    int32_t mode_y = head.y + head.h + 2, mode_h = 38;
    int32_t cy = landscape ? (mode_y + mode_h + h * 80 / 100) / 2 : h * 46 / 100;
    pad->mode = button_at(screen, (ui_key_rect_t){w / 2 - unit * 24 / 100, mode_y, unit * 48 / 100, mode_h},
                          "Direzioni: premute", UI_COLOR_CARD, gamepad_center, LV_EVENT_CLICKED, pad);
    lv_obj_set_style_text_font(lv_obj_get_child(pad->mode, 0), UI_FONT_SMALL, 0);
    button_at(screen, (ui_key_rect_t){w * 29 / 100 - size / 2, cy - size / 2, size, size}, LV_SYMBOL_STOP,
              UI_COLOR_CARD, gamepad_center, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < 12; i++)
    {
        ui_key_rect_t rect;
        if (i < 4)
        {
            static const int8_t dx[] = {0, 0, -1, 1};
            static const int8_t dy[] = {-1, 1, 0, 0};
            rect = (ui_key_rect_t){w * 29 / 100 - size / 2 + dx[i] * size, cy - size / 2 + dy[i] * size, size, size};
        }
        else if (i < 8)
        {
            /* Familiar diamond: Y above, A below, X left, B right. */
            static const int8_t dx[] = {0, 1, -1, 0};
            static const int8_t dy[] = {1, 0, 0, -1};
            int32_t face = unit * 14 / 100, step = unit * 12 / 100;
            rect = (ui_key_rect_t){w * 70 / 100 - face / 2 + dx[i - 4] * step, cy - face / 2 + dy[i - 4] * step, face,
                                   face};
        }
        else if (landscape)
        {
            ui_key_rect_t band = ui_keyboard_band(w, h, false, h * 83 / 100, h - 4 - h * 83 / 100, 8);
            rect = (ui_key_rect_t){band.x + (i - 8) * band.w / 4, band.y, band.w / 4, band.h};
        }
        else
        {
            int32_t y = h * (66 + 11 * ((i - 8) / 2)) / 100;
            ui_key_rect_t band = ui_keyboard_band(w, h, board_info()->round, y, h * 10 / 100, 8);
            rect = (ui_key_rect_t){band.x + ((i - 8) % 2) * band.w / 2, band.y, band.w / 2, band.h};
        }
        lv_obj_t *button = button_at(screen, rect, names[i], i < 4 || i >= 8 ? UI_COLOR_CARD_HI : s_pad_colors[i - 4],
                                     gamepad_button, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        pad->keys[i] = button;
        if (i == 4 || i == 7)
        {
            lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_black(), 0);
        }
        if (i >= 4 && i < 8)
            lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_transform_scale(button, 235, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(button, (rect.w - 6) / 2, 0);
        lv_obj_set_style_transform_pivot_y(button, (rect.h - 6) / 2, 0);
        lv_obj_add_event_cb(button, gamepad_button, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, gamepad_button, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
    }
    gamepad_refresh(pad);
    pad->version = hid_link_version();
    pad->timer = lv_timer_create(gamepad_tick, 100, pad);
    lv_obj_add_event_cb(screen, gamepad_deleted, LV_EVENT_DELETE, pad);
}

static bool mouse_back(void)
{
    mouse_clear();
    return false;
}
static bool gamepad_back(void)
{
    gamepad_clear();
    return false;
}

#define MODE_FLAGS (APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE)
const app_t remote_mouse_app = {.id = "remote.mouse",
                                .name = "Mouse",
                                .flags = MODE_FLAGS,
                                .create = mouse_create,
                                .back = mouse_back,
                                .pause = mouse_clear};
const app_t remote_media_app = {.id = "remote.media", .name = "Media", .flags = MODE_FLAGS, .create = media_create};
const app_t remote_presenter_app = {
    .id = "remote.presenter", .name = "Presentazioni", .flags = MODE_FLAGS, .create = presenter_create};
const app_t remote_meeting_app = {.id = "remote.meeting",
                                  .name = "Riunioni",
                                  .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                  .create = meeting_create};
const app_t remote_gamepad_app = {.id = "remote.gamepad",
                                  .name = "Gamepad",
                                  .flags = MODE_FLAGS,
                                  .create = gamepad_create,
                                  .back = gamepad_back,
                                  .pause = gamepad_clear};
const app_t remote_shortcuts_app = {
    .id = "remote.shortcuts", .name = "Scorciatoie", .flags = MODE_FLAGS, .create = shortcuts_create};

void remote_modes_register(void)
{
    app_register(&remote_mouse_app);
    app_register(&s_mouse_more_app);
    app_register(&remote_media_app);
    app_register(&remote_presenter_app);
    app_register(&remote_meeting_app);
    app_register(&remote_gamepad_app);
    app_register(&remote_shortcuts_app);
    for (unsigned i = 0; i < 4; i++)
        app_register(&s_shortcut_apps[i]);
}
