/*
 * Deck UI.
 *
 *   KEYS     the profile's keycaps (3D caps that sink when pressed and glow in
 *            their colour when the action leaves). Header: profile and link;
 *            tap it for the menu. Bottom: the dial - the knob on boards that
 *            have one, a strip to drag sideways on the others.
 *   MENU     profiles, connection (USB / Bluetooth), configure from the PC, exit
 *   CONFIG   configuration mode: address and code for the editor page
 */
#include "deck.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_heap_caps.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hid_link/hid_link.h"
#include "sdkconfig.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

enum { KEYS, MENU, CONFIG };

#define C_BG 0x090B10
#define C_CAP 0x1B1F2A
#define C_CAP_PRESSED 0x232838
#define C_EDGE 0x2A3040
#define C_SHADOW 0x05060A
#define C_INK 0xEEF1F7
#define C_MIST 0x8B93A7
#define DIAL_STEP_PX 22

typedef struct
{
    lv_obj_t *cap;
    int index;
    lv_timer_t *glow;
} cap_t;

typedef struct
{
    lv_obj_t *screen, *content, *status, *dial_label;
    lv_timer_t *timer;
    int view;
    cap_t caps[DECK_KEYS];
    int32_t drag_x;
    hid_link_state_t shown_state;
} deck_ui_t;

static deck_ui_t *s_ui;
static void render(deck_ui_t *ui);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    lv_obj_set_clickable(l, false);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static int current_profile(void)
{
    int index = deck_settings()->profile;
    return index < deck_profile_count() ? index : 0;
}

static void link_text(char *out, size_t size, hid_link_state_t *state)
{
    *state = hid_link_state();
    bool usb = hid_link_transport() == HID_LINK_USB;
    if (*state == HID_LINK_CONNECTED)
        snprintf(out, size, "%s  collegato al PC", usb ? LV_SYMBOL_USB : LV_SYMBOL_BLUETOOTH);
    else if (usb)
        snprintf(out, size, LV_SYMBOL_USB "  collega il cavo al PC");
    else
        snprintf(out, size, LV_SYMBOL_BLUETOOTH "  associa \"%s Deck\" dal PC", board_info()->short_name);
}

/* ------------------------------------------------------------ keys */

static void glow_end(lv_timer_t *timer)
{
    cap_t *cap = lv_timer_get_user_data(timer);
    lv_obj_set_style_border_color(cap->cap, ui_color(C_EDGE), 0);
    lv_obj_set_style_border_width(cap->cap, 1, 0);
    cap->glow = NULL;
    lv_timer_delete(timer);
}

static void cap_event(lv_event_t *e)
{
    cap_t *cap = lv_event_get_user_data(e);
    const deck_key_t *key = &deck_profile(current_profile())->keys[cap->index];
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
    {
        char text[200];
        snprintf(text, sizeof(text), "%s: %s", key->label, key->action);
        ui_toast(text);
        lv_indev_wait_release(lv_indev_active()); /* no click after the long press */
        return;
    }
    if (!deck_connected())
    {
        ui_toast(hid_link_transport() == HID_LINK_USB ? "Collega il cavo USB al PC" : "Associa Deck dal Bluetooth del PC");
        return;
    }
    if (!deck_run(key->action))
    {
        ui_toast("Un attimo...");
        return;
    }
    /* The cap glows in its colour while the action leaves. */
    lv_obj_set_style_border_color(cap->cap, ui_color(key->color), 0);
    lv_obj_set_style_border_width(cap->cap, 2, 0);
    if (cap->glow)
        lv_timer_reset(cap->glow);
    else
        cap->glow = lv_timer_create(glow_end, 220, cap);
}

static void cap_deleted(lv_event_t *e)
{
    cap_t *cap = lv_event_get_user_data(e);
    if (cap->glow)
    {
        lv_timer_delete(cap->glow);
        cap->glow = NULL;
    }
}

static lv_obj_t *keycap(lv_obj_t *parent, cap_t *cap, const deck_key_t *key, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_color(c, ui_color(C_CAP), 0);
    lv_obj_set_style_bg_grad_color(c, ui_color(0x161922), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, ui_color(C_EDGE), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    /* The key's body: a hard shadow below that shrinks when pressed. */
    lv_obj_set_style_shadow_color(c, ui_color(C_SHADOW), 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_shadow_offset_y(c, 5, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(c, 3, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_offset_y(c, 2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(c, ui_color(C_CAP_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);
    const char *glyph = deck_glyph(key->glyph);
    bool symbol = glyph != key->glyph;
    lv_obj_t *g = label(c, symbol ? UI_FONT_ICON : ui_font_title, key->color, glyph);
    (void)g;
    lv_obj_t *l = label(c, UI_FONT_SMALL, C_MIST, key->label);
    lv_obj_set_width(l, w - 12);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(UI_FONT_SMALL));
    cap->cap = c;
    lv_obj_add_event_cb(c, cap_event, LV_EVENT_CLICKED, cap);
    lv_obj_add_event_cb(c, cap_event, LV_EVENT_LONG_PRESSED, cap);
    lv_obj_add_event_cb(c, cap_deleted, LV_EVENT_DELETE, cap);
    lv_port_set_focusable(c);
    return c;
}

static void open_menu(lv_event_t *e)
{
    (void)e;
    s_ui->view = MENU;
    render(s_ui);
}

/* Touch dial: every DIAL_STEP_PX of sideways drag is one detent. */
static void strip_event(lv_event_t *e)
{
    deck_ui_t *ui = s_ui;
    lv_indev_t *input = lv_indev_active();
    if (input == NULL)
        return;
    lv_point_t p;
    lv_indev_get_point(input, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
    {
        ui->drag_x = p.x;
        return;
    }
    int32_t moved = p.x - ui->drag_x;
    int steps = (int)(moved / DIAL_STEP_PX);
    if (steps != 0)
    {
        ui->drag_x += steps * DIAL_STEP_PX;
        if (deck_connected())
            deck_dial((deck_dial_t)deck_profile(current_profile())->dial, steps);
    }
}

static void knob_event(lv_event_t *e)
{
    lv_port_knob_t *k = lv_event_get_param(e);
    if (s_ui->view != KEYS)
        return;
    if (deck_connected())
        deck_dial((deck_dial_t)deck_profile(current_profile())->dial, k->delta);
    k->handled = true;
}

static void keys(deck_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height;
    const deck_profile_t *profile = deck_profile(current_profile());
    lv_obj_t *c = ui->content;

    /* Header: profile name and link, a tap opens the menu. */
    lv_obj_t *header = plain(c);
    lv_obj_set_size(header, b->round ? w * 62 / 100 : w - 24, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(header, 4, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, b->round ? h * 8 / 100 : 6);
    lv_obj_set_clickable(header, true);
    lv_obj_set_ext_click_area(header, 10);
    lv_obj_add_event_cb(header, open_menu, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(header);
    char title[48];
    snprintf(title, sizeof(title), "%s  " LV_SYMBOL_DOWN, profile->name);
    label(header, ui_font_title, C_INK, title);
    ui->status = label(header, UI_FONT_SMALL, C_MIST, "");

    /* Keycaps: 3 columns; 4 rows on tall screens, a scrolling grid on round ones. */
    int columns = 3, gap = 10;
    int grid_w = b->round ? w * 74 / 100 : w - 24;
    int cap_w = (grid_w - gap * (columns - 1)) / columns;
    int top = b->round ? h * 22 / 100 : 72;
    int bottom = b->round ? h * 20 / 100 : 58;
    int rows_visible = b->round ? 3 : 4;
    int cap_h = (h - top - bottom - gap * (rows_visible - 1)) / rows_visible;
    if (cap_h > cap_w)
        cap_h = cap_w;
    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, grid_w + 8, h - top - bottom);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_set_style_pad_row(grid, gap + 2, 0);
    lv_obj_set_style_pad_left(grid, 4, 0);
    lv_obj_set_style_pad_bottom(grid, 8, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < profile->count; i++)
    {
        ui->caps[i].index = i;
        ui->caps[i].glow = NULL;
        keycap(grid, &ui->caps[i], &profile->keys[i], cap_w, cap_h);
    }

    /* Dial. */
    lv_obj_t *strip = plain(c);
    lv_obj_set_size(strip, b->round ? w * 52 / 100 : w - 24, 40);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 8 / 100 : -8);
    lv_obj_set_style_radius(strip, 20, 0);
    lv_obj_set_style_bg_color(strip, ui_color(0x12151D), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(strip, ui_color(C_EDGE), 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    char text[48];
    if (b->has_knob)
        snprintf(text, sizeof(text), "Manopola: %s", deck_dial_name((deck_dial_t)profile->dial));
    else
    {
        snprintf(text, sizeof(text), LV_SYMBOL_LEFT "   %s   " LV_SYMBOL_RIGHT, deck_dial_name((deck_dial_t)profile->dial));
        lv_obj_set_clickable(strip, true);
        lv_obj_add_event_cb(strip, strip_event, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(strip, strip_event, LV_EVENT_PRESSING, NULL);
    }
    ui->dial_label = label(strip, UI_FONT_SMALL, C_MIST, text);
    lv_obj_center(ui->dial_label);
    ui->shown_state = (hid_link_state_t)-1;
}

/* ------------------------------------------------------------ menu */

static void choose_profile(lv_event_t *e)
{
    deck_settings_t s = *deck_settings();
    s.profile = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    deck_settings_save(&s);
    s_ui->view = KEYS;
    render(s_ui);
}

static void restart_into(bool confirmed, void *user_data)
{
    if (!confirmed)
        return;
    deck_settings_t s = *deck_settings();
    intptr_t what = (intptr_t)user_data;
    if (what == 1)
        s.config_mode = 1;
    else
        s.transport = (uint8_t)(what == 2 ? HID_LINK_USB : HID_LINK_BLE);
    deck_settings_save(&s);
    extapp_restart_self();
}

static void menu_action(lv_event_t *e)
{
    intptr_t what = (intptr_t)lv_event_get_user_data(e);
    if (what == 1)
        ui_confirm("Configura dal PC", "Deck si riavvia in modalità configurazione: apri l'indirizzo che compare sul PC.",
                   "Riavvia", 0x4FB3FF, restart_into, (void *)1);
    else if (what == 2 || what == 3)
        ui_confirm(what == 2 ? "Collegamento USB" : "Collegamento Bluetooth", "Deck si riavvia con il nuovo collegamento.",
                   "Riavvia", 0x4FB3FF, restart_into, (void *)what);
    else if (what == 4)
    {
        hid_link_forget_hosts();
        ui_toast("Associazioni Bluetooth cancellate");
    }
    else if (what == 5)
        extapp_return_to_launcher();
}

static void menu(deck_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Deck");
    ui_section(page, "PROFILI");
    for (int i = 0; i < deck_profile_count(); i++)
    {
        const deck_profile_t *p = deck_profile(i);
        char value[24];
        snprintf(value, sizeof(value), "%d tasti", p->count);
        ui_row(page, i == current_profile() ? LV_SYMBOL_OK : (p->custom ? LV_SYMBOL_EDIT : LV_SYMBOL_KEYBOARD),
               i == current_profile() ? 0x5BD6A0 : (p->custom ? 0xF7C948 : 0x4FB3FF), p->name, value, choose_profile,
               (void *)(intptr_t)i);
    }
    ui_section(page, "COLLEGAMENTO");
    bool usb = hid_link_transport() == HID_LINK_USB;
    if (board_info()->has_usb_otg)
        ui_row(page, LV_SYMBOL_USB, 0x9C8CFF, "USB", usb ? "attivo" : NULL, menu_action, (void *)2);
    ui_row(page, LV_SYMBOL_BLUETOOTH, 0x4FB3FF, "Bluetooth", usb ? NULL : "attivo", menu_action, (void *)3);
    if (!usb)
        ui_row(page, LV_SYMBOL_TRASH, 0xFF6B6B, "Dimentica i PC associati", NULL, menu_action, (void *)4);
    ui_section(page, "PERSONALIZZA");
    ui_row(page, LV_SYMBOL_EDIT, 0xF7C948, "Configura dal PC", "tasti e profili", menu_action, (void *)1);
    ui_text(page, "Dal PC crei profili tuoi: etichetta, simbolo, colore e azione di ogni tasto, anche registrando la combinazione dalla tastiera.", true);
    ui_button(page, "Esci", 0x232A3A, menu_action, (void *)5);
}

/* ------------------------------------------------------------ configuration mode */

static void config_done(lv_event_t *e)
{
    (void)e;
    deck_settings_t s = *deck_settings();
    s.config_mode = 0;
    deck_settings_save(&s);
    extapp_restart_self();
}

static void config(deck_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Configura Deck");
    ui_text(page, "Sul PC, nella stessa rete Wi-Fi, apri nel browser http://", true);
    char text[64];
    snprintf(text, sizeof(text), "%s", deck_web_address()); /* the address alone fits one line */
    lv_obj_t *address = ui_text(page, text, false);
    lv_obj_set_style_text_font(address, ui_font_headline, 0);
    lv_obj_set_style_text_color(address, ui_color(0x4FB3FF), 0);
    ui_text(page, "Codice:", true);
    const char *code = deck_web_code();
    snprintf(text, sizeof(text), "%.3s %.3s", code, code + 3);
    lv_obj_t *code_label = ui_text(page, text, false);
    lv_obj_set_style_text_font(code_label, ui_font_big, 0);
    ui->status = ui_text(page, "In attesa dell'editor...", true);
    ui_button(page, "Torna ai tasti", 0x4FB3FF, config_done, NULL);
}

/* ------------------------------------------------------------ frame */

static void render(deck_ui_t *ui)
{
    if (ui->content)
        lv_obj_delete(ui->content);
    memset(ui->caps, 0, sizeof(ui->caps));
    ui->status = ui->dial_label = NULL;
    ui->content = plain(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    if (ui->view == KEYS)
        keys(ui);
    else if (ui->view == MENU)
        menu(ui);
    else
        config(ui);
}

static void tick(lv_timer_t *timer)
{
    deck_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->status == NULL)
        return;
    if (ui->view == CONFIG)
    {
        if (deck_web_saved())
            lv_label_set_text(ui->status, "Salvato. Tocca \"Torna ai tasti\" per usarli.");
        return;
    }
    hid_link_state_t state;
    char text[64];
    link_text(text, sizeof(text), &state);
    if (state == ui->shown_state)
        return;
    ui->shown_state = state;
    lv_label_set_text(ui->status, text);
    lv_obj_set_style_text_color(ui->status, ui_color(state == HID_LINK_CONNECTED ? 0x5BD6A0 : 0xF7C948), 0);
}

bool deck_ui_back(void)
{
    if (s_ui->view == MENU)
    {
        s_ui->view = KEYS;
        render(s_ui);
    }
    else
        extapp_return_to_launcher();
    return true;
}

void deck_ui_create(lv_obj_t *screen, bool config_mode)
{
    deck_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    ui->view = config_mode ? CONFIG : KEYS;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    lv_obj_add_event_cb(screen, knob_event, lv_port_knob_event(), NULL);
    render(ui);
    ui->timer = lv_timer_create(tick, 300, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA only: "view keys|menu|config", "profile n". */
#include "core/devcon.h"
#include <stdlib.h>

bool devcon_app_command(char *line)
{
    if (s_ui == NULL)
        return false;
    bool handled = true;
    lv_port_lock();
    if (strcmp(line, "view keys") == 0)
        s_ui->view = KEYS;
    else if (strcmp(line, "view menu") == 0)
        s_ui->view = MENU;
    else if (strcmp(line, "config") == 0)
    {
        lv_port_unlock();
        printf("@OK config\n");
        restart_into(true, (void *)1);
        return true;
    }
    else if (strncmp(line, "profile ", 8) == 0)
    {
        deck_settings_t s = *deck_settings();
        s.profile = (uint8_t)atoi(line + 8);
        deck_settings_save(&s);
        s_ui->view = KEYS;
    }
    else
        handled = false;
    if (handled)
        render(s_ui);
    lv_port_unlock();
    if (handled)
        printf("@OK %s\n", line);
    return handled;
}
#endif
