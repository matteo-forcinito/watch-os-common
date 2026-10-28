/*
 * Studio: drum pads, instruments and motion (unchanged behaviour), restyled
 * on the NEON NOIR tokens. Controls keep the 360-unit canvas of layout.h.
 * The VOL key opens the shared MASTER sheet.
 */
#include "engine.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "layout.h"
#include "solco_ui.h"
#include "ui/ui.h"
#include "ui_kit.h"
#include <stdio.h>

static int s_width, s_height;
static bool s_landscape;
static solco_layout_t s_layout;
static lv_obj_t *s_studio_page, *s_studio_name, *s_studio_info, *s_studio_play, *s_studio_rec;
static lv_obj_t *s_pad[8], *s_steps[16], *s_patch, *s_link_info, *s_motion_label, *s_kit;
static lv_obj_t *s_studio_tools, *s_studio_menu;
static int s_studio_view = -1, s_instrument;
static int s_last_step = -2;
static lv_obj_t *s_home;
static void studio_build(void);

static int x(int value) { return value * s_width / 360; }
static int y(int value) { return value * s_height / 360; }

static lv_obj_t *label(lv_obj_t *parent, const char *text, int left, int top, int width, uint32_t color)
{
    lv_obj_t *obj = sk_label(parent, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, color,
                             x(left), y(top), x(width), LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(obj, text);
    return obj;
}

static void studio_action(lv_event_t *event);

/* Outline control: hairline border, colour flash on press. Returns its label. */
static lv_obj_t *control(lv_obj_t *parent, const char *text, int left, int top, int width,
                         lv_event_cb_t callback, int code)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x(left), y(top));
    lv_obj_set_size(obj, x(width), y(42));
    lv_obj_set_style_radius(obj, TK_R_XS, 0);
    lv_obj_set_style_border_width(obj, TK_LW_HAIR, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(TK_LINE_HI), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(TK_SURFACE_1), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(TK_CYAN_G2), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(TK_CYAN), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(obj, y(4));
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *text_obj = lv_label_create(obj);
    lv_label_set_text(text_obj, text);
    lv_obj_set_style_text_color(text_obj, lv_color_hex(TK_TEXT), 0);
    lv_obj_set_style_text_font(text_obj, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_obj_center(text_obj);
    return text_obj;
}

static void studio_volume(lv_event_t *event)
{
    (void)event;
    solco_master_toggle();
}

static void studio_action(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    studio_status_t state;
    solco_studio_status(&state);
    if (code >= 100 && code < 108)
        solco_studio_command(STUDIO_HIT, (s_studio_view == 2 ? 8 + s_instrument * 8 : 0) + code - 100, 110);
    else if (code == 10)
        solco_studio_command(STUDIO_PLAY, 0, 0);
    else if (code == 11)
        solco_studio_command(STUDIO_RECORD, 0, 0);
    else if (code == 12)
        solco_studio_command(STUDIO_CLEAR, 0, 0);
    else if (code == 13)
    {
        s_instrument = (s_instrument + 1) % 3;
        solco_studio_open(2);
    }
    else if (code == 14)
        solco_studio_command(STUDIO_TEMPO, state.bpm - 5, 0);
    else if (code == 15)
        solco_studio_command(STUDIO_TEMPO, state.bpm + 5, 0);
    else if (code == 18)
        lv_obj_set_hidden(s_studio_tools, !lv_obj_is_hidden(s_studio_tools));
    else if (code == 19)
        solco_motion_calibrate();
    else if (code == 17)
        solco_studio_command(STUDIO_KIT, state.kit ? 0 : 1, 0);
    else if (code == 16)
        solco_motion_mode((solco_motion_get() + 1) % 3);
    else
        solco_studio_open(code);
}

int solco_studio_view(void) { return s_studio_view; }
bool solco_studio_tools_open(void) { return s_studio_tools && !lv_obj_is_hidden(s_studio_tools); }

void solco_studio_open(int view)
{
    if (view >= 0 && !s_studio_page)
        studio_build(); /* created on first open */
    if (!s_studio_page)
    {
        s_studio_view = -1;
        solco_ui_studio_closed();
        return;
    }
    s_studio_view = view;
    if (s_studio_tools)
        lv_obj_set_hidden(s_studio_tools, true);
    solco_studio_command(STUDIO_ENABLE, view == 1 || view == 2, 0);
    lv_obj_set_hidden(s_studio_page, view < 0);
    if (view < 0)
    {
        solco_ui_studio_closed();
        return;
    }
    solco_ui_studio_opened();
    lv_obj_move_foreground(s_studio_page);
    s_last_step = -2;
    const char *names[] = {"STUDIO //", "DRUMPAD //", "STRUMENTI //", "MOVIMENTO //"};
    lv_label_set_text(s_studio_name, names[view]);
    for (int i = 0; i < 8; i++)
        lv_obj_set_hidden(lv_obj_get_parent(s_pad[i]), view != 1 && view != 2);
    for (int i = 0; i < 16; i++)
        lv_obj_set_hidden(s_steps[i], view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_play), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_rec), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_kit), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_patch), view != 2);
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_menu), view != 1 && view != 2);
    static const char *patches[] = {"E-PIANO   >", "BASS   >", "BELL   >"};
    lv_label_set_text(s_patch, patches[s_instrument]);
    static const char *drums[] = {"KICK", "SNARE", "CLAP", "HI-HAT", "OPEN HAT", "TOM", "SHAKER", "RIM"};
    static const char *notes[] = {"DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO +"};
    for (int i = 0; i < 8; i++)
        lv_label_set_text(s_pad[i], view == 2 ? notes[i] : drums[i]);
    /* Other panels are children tagged by their view through user data. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_studio_page); i++)
    {
        lv_obj_t *child = lv_obj_get_child(s_studio_page, i);
        intptr_t tag = (intptr_t)lv_obj_get_user_data(child);
        if (tag >= 1000)
            lv_obj_set_hidden(child, view != tag - 1000);
    }
}

void solco_studio_refresh(void)
{
    if (!s_studio_page || s_studio_view < 0)
        return;
    studio_status_t state;
    solco_studio_status(&state);
    char text[48];
    snprintf(text, sizeof(text), "%d BPM  //  %d VOCI", state.bpm, state.voices);
    sk_set_text(s_studio_info, text);
    sk_set_text(s_kit, state.kit ? "KIT B" : "KIT A");
    sk_set_text(s_studio_play, state.playing ? "STOP" : "PLAY");
    sk_set_text(s_studio_rec, state.recording ? "REC ON" : "REC");
    lv_obj_t *rec = lv_obj_get_parent(s_studio_rec);
    lv_obj_set_style_bg_color(rec, lv_color_hex(state.recording ? TK_RED_G2 : TK_SURFACE_1), 0);
    lv_obj_set_style_border_color(rec, lv_color_hex(state.recording ? TK_RED : TK_LINE_HI), 0);
    int current = state.playing ? state.step : -1;
    if (current != s_last_step)
    {
        for (int i = 0; i < 16; i++)
            if (i == current || i == s_last_step || s_last_step == -2)
                lv_obj_set_style_bg_color(s_steps[i], lv_color_hex(i == current ? TK_CYAN : TK_LINE_HI), 0);
        s_last_step = current;
    }
    sk_set_text(s_link_info, "BATTERIA: IMPARA COLPO\nFermo, poi un colpo in avanti.\nCentro: snare. "
                             "Inclina: kit.\nRileva la frenata del colpo.");
    const char *motion[] = {"MOVIMENTO: OFF", "MARACAS", "BATTERIA"};
    int learned, pad;
    solco_motion_state(&learned, &pad);
    if (imu_supported() && solco_motion_get() == 2)
    {
        static const char *drums[] = {"KICK", "SNARE", "CLAP", "HI-HAT", "OPEN HAT", "TOM", "SHAKER", "RIM"};
        snprintf(text, sizeof(text), learned ? "BATTERIA: %s" : "FAI IL COLPO DI PROVA", drums[pad & 7]);
        sk_set_text(s_motion_label, text);
    }
    else
        sk_set_text(s_motion_label, imu_supported() ? motion[solco_motion_get()] : "NESSUNA IMU");
}

static void studio_tag(lv_obj_t *text, int view)
{
    lv_obj_set_user_data(lv_obj_get_parent(text), (void *)(intptr_t)(1000 + view));
}

void solco_studio_create(lv_obj_t *home)
{
    s_home = home;
    s_studio_view = -1;
}

static void studio_build(void)
{
    lv_obj_t *home = s_home;
    s_width = lv_obj_get_width(home);
    s_height = lv_obj_get_height(home);
    bool round = board_info()->round;
    s_layout = solco_layout_for(s_width, s_height, round);
    s_landscape = s_layout.landscape;
    s_studio_view = -1;
    s_studio_page = sk_panel(home, 0, 0, s_width, s_height, true);
    lv_obj_add_flag(s_studio_page, LV_OBJ_FLAG_CLICKABLE);
    s_studio_name = sk_hud(s_studio_page, "STUDIO //", TK_VIOLET, x(112), y(round ? 36 : 17), x(136),
                           LV_TEXT_ALIGN_CENTER, false);
    control(s_studio_page, LV_SYMBOL_LEFT, round ? 60 : 8, round ? 25 : 5, 42, studio_action, -1);
    control(s_studio_page, "VOL", round ? 246 : 296, round ? 25 : 5, 56, studio_volume, 0);
    studio_tag(control(s_studio_page, "MIX / VINYL", 65, 87, 230, studio_action, -1), 0);
    studio_tag(control(s_studio_page, "DRUMPAD / BEAT", 45, 137, 270, studio_action, 1), 0);
    studio_tag(control(s_studio_page, "STRUMENTI", 45, 187, 270, studio_action, 2), 0);
    studio_tag(control(s_studio_page, "MOVIMENTO", 65, 237, 230, studio_action, 3), 0);
    s_studio_play = control(s_studio_page, "PLAY", round ? 82 : 12, round ? 301 : 312, round ? 60 : 102,
                            studio_action, 10);
    s_studio_rec = control(s_studio_page, "REC", round ? 150 : 126, round ? 301 : 312, round ? 60 : 102,
                           studio_action, 11);
    s_studio_menu = control(s_studio_page, "...", round ? 218 : 240, round ? 301 : 312, round ? 60 : 108,
                            studio_action, 18);
    s_patch = control(s_studio_page, "E-PIANO >", round ? 82 : 12, round ? 301 : 312, round ? 128 : 216,
                      studio_action, 13);
    for (int i = 0; i < 8; i++)
    {
        solco_rect_t box = s_layout.pad[i];
        uint32_t neon = (i % 2) ? TK_MAGENTA : TK_CYAN;
        sk_tone_t t = sk_tone(neon);
        s_pad[i] = control(s_studio_page, "PAD", box.x, box.y, box.w, studio_action, 100 + i);
        lv_obj_t *pad = lv_obj_get_parent(s_pad[i]);
        lv_obj_set_height(pad, y(box.h));
        lv_obj_set_style_bg_color(pad, lv_color_hex(t.g3), 0);
        lv_obj_set_style_border_color(pad, lv_color_hex(t.g1), 0);
        lv_obj_set_style_bg_color(pad, lv_color_hex(t.g1), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(pad, lv_color_hex(t.neon), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(s_pad[i], lv_color_hex(t.neon), 0);
        lv_obj_remove_event_cb(pad, studio_action);
        lv_obj_add_event_cb(pad, studio_action, LV_EVENT_PRESSED, (void *)(intptr_t)(100 + i));
    }
    for (int i = 0; i < 16; i++)
    {
        s_steps[i] = lv_obj_create(s_studio_page);
        lv_obj_remove_style_all(s_steps[i]);
        lv_obj_set_pos(s_steps[i], x((round ? 52 : 12) + i * (round ? 16 : 21)), y(round ? 69 : 48));
        lv_obj_set_size(s_steps[i], x(round ? 12 : 17), y(3));
        lv_obj_set_style_bg_opa(s_steps[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_steps[i], lv_color_hex(TK_LINE_HI), 0);
        lv_obj_remove_flag(s_steps[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_studio_tools = sk_panel(s_studio_page, 0, 0, s_width, s_height, true);
    lv_obj_add_flag(s_studio_tools, LV_OBJ_FLAG_CLICKABLE);
    control(s_studio_tools, LV_SYMBOL_LEFT " SUONA", 90, 30, 180, studio_action, 18);
    s_studio_info = label(s_studio_tools, "120 BPM", 90, 87, 210, TK_TEXT);
    control(s_studio_tools, "BPM -", 48, 118, 128, studio_action, 14);
    control(s_studio_tools, "BPM +", 184, 118, 128, studio_action, 15);
    s_kit = control(s_studio_tools, "KIT A", 48, 172, 128, studio_action, 17);
    control(s_studio_tools, "CLR BEAT", 184, 172, 128, studio_action, 12);
    label(s_studio_tools, "REC sovraincide  /  CLR cancella", 48, 238, 280, TK_TEXT_2);
    lv_obj_set_hidden(s_studio_tools, true);
    s_link_info = label(s_studio_page, "", 48, 116, 270, TK_TEXT_2);
    lv_obj_set_user_data(s_link_info, (void *)1003);
    lv_obj_set_height(s_link_info, y(round ? 122 : 87));
    lv_label_set_long_mode(s_link_info, LV_LABEL_LONG_WRAP);
    s_motion_label = control(s_studio_page, "MOVIMENTO: OFF", 60, 206, 240, studio_action, 16);
    studio_tag(s_motion_label, 3);
    control(s_studio_page, "IMPARA COLPO", 60, 258, 240, studio_action, 19);
    lv_obj_set_user_data(lv_obj_get_child(s_studio_page, -1), (void *)1003);
    lv_obj_move_foreground(s_studio_tools);
    lv_obj_set_hidden(s_studio_page, true);
}
