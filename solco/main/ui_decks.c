/*
 * Solco live views: DECKS (home), FOCUS, JOG and LIBRARY.
 *
 * DECKS mirrors the two decks around the crossfader, so both transport rows
 * sit next to the thumb and the waveforms frame the screen:
 *
 *     [A] title ............ 01:23      header
 *     |||||||||||||▌||||||||||||||      waveform (history | playhead | preview)
 *     [ PLAY ][CUE][LOOP IN][1/2][x2]   transport
 *     A +0.00%    ▮▮▮▮▮▯▯    +0.00% B   pitch / master peak
 *     A ----------[ | ]---------- B     crossfader
 *     [ PLAY ][CUE][LOOP IN][1/2][x2]
 *     |||||||||||||▌||||||||||||||
 *     [B] title ............ 00:42
 *
 * The volume lives in the MASTER sheet (BOOT click), never on the decks.
 * Every live element changes only when its value does; the two waveforms
 * are the only continuous animation (<= 25 % of the screen).
 */
#include "core/app.h"
#include "core/lv_port.h"
#include "engine.h"
#include "esp_heap_caps.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "solco_ui.h"
#include "ui/ui.h"
#include "ui_kit.h"
#include "vinyl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int x, y, w, h;
} rc_t;

typedef struct
{
    rc_t head[2], wave[2], strip[2], pitch[2], meter, cross, status, c_master, c_studio;
    rc_t f_head, f_info, f_wave, f_pitch, f_loop, f_trans, f_nav;
    rc_t l_head, l_close, l_list;
    rc_t j_disc, j_back, j_play, j_cue;
} geo_t;

static int s_w, s_h;
static bool s_round, s_landscape;
static geo_t g;
static lv_obj_t *s_root;
static int s_view = SOLCO_VIEW_DECKS, s_deck;
static solco_status_t s_st, s_prev;
static bool s_first = true;
static bool s_wheel_vinyl = true;

/* DECKS */
static lv_obj_t *s_decks;
static lv_obj_t *d_chip[2], *d_title[2], *d_time[2], *d_pitch[2], *d_status;
static sk_wave_t *d_wave[2];
static sk_strip_t *d_strip[2];
static sk_fader_t *d_cross;
static sk_meter_t *d_meter;

/* FOCUS */
static lv_obj_t *s_focus;
static lv_obj_t *f_chip, *f_title, *f_time, *f_info;
static sk_wave_t *f_wave[2]; /* one image per deck: switching never mixes histories */
static sk_fader_t *f_pitch;
static sk_strip_t *f_loop, *f_trans, *f_nav;
static bool f_touch;
static int f_touch_x;
static bool s_rev_hold;
static int32_t s_rev_ppm;
static uint32_t s_info_alert_until;
static char s_info_alert[48];

/* JOG */
static lv_obj_t *s_jog, *j_disc, *j_label, *j_marker;
static sk_strip_t *j_back, *j_play, *j_cue;
static lv_image_dsc_t j_grooves;
static float j_angle, j_touch_angle;
static bool j_drag, j_touch_valid;

/* LIBRARY */
static lv_obj_t *s_lib, *l_head, *l_list;
static sk_strip_t *l_close;
static int s_lib_deck, s_lib_return = SOLCO_VIEW_DECKS;

/* volume diagnostics */
static const char *s_volume_source = "boot";
static uint32_t s_volume_changes;

static void show(int view, int deck);
static void open_library(int deck);

/* ------------------------------------------------------------------ geometry */

static rc_t R(int x, int y, int w, int h, int base_w, int base_h)
{
    return (rc_t){x * s_w / base_w, y * s_h / base_h, w * s_w / base_w, h * s_h / base_h};
}

static void geometry(void)
{
    if (s_landscape)
    {
#define L(x, y, w, h) R(x, y, w, h, 448, 368)
        g.head[0] = L(32, 4, 384, 30);
        g.wave[0] = L(8, 38, 432, 48);
        g.strip[0] = L(8, 90, 432, 50);
        g.pitch[0] = L(16, 147, 150, 18);
        g.meter = L(184, 152, 80, 8);
        g.pitch[1] = L(282, 147, 150, 18);
        g.cross = L(56, 168, 336, 40);
        g.c_master = L(4, 166, 48, 44);
        g.c_studio = L(396, 166, 48, 44);
        g.status = L(64, 209, 320, 18);
        g.strip[1] = L(8, 228, 432, 50);
        g.wave[1] = L(8, 282, 432, 48);
        g.head[1] = L(32, 334, 384, 30);
        g.f_head = L(32, 4, 384, 30);
        g.f_info = L(32, 37, 384, 18);
        g.f_wave = L(8, 66, 432, 80); /* 80 px: scrolling area kept under 21 % of the screen */
        g.f_pitch = L(8, 160, 432, 44);
        g.f_loop = L(8, 210, 432, 48);
        g.f_trans = L(8, 264, 432, 48);
        g.f_nav = L(56, 318, 336, 44);
        g.l_head = L(40, 12, 260, 20);
        g.l_close = L(312, 4, 120, 40);
        g.l_list = L(16, 50, 416, 314);
        g.j_disc = L(64, 24, 320, 320);
        g.j_back = L(4, 96, 56, 56);
        g.j_cue = L(4, 216, 56, 56);
        g.j_play = L(388, 96, 56, 56);
#undef L
    }
    else
    {
        /* Round (and square) panels: base 466, content kept inside the circle. */
#define C(x, y, w, h) R(x, y, w, h, 466, 466)
        g.head[0] = C(90, 50, 286, 30);
        g.wave[0] = C(56, 84, 354, 48);
        g.strip[0] = C(28, 136, 410, 48);
        g.pitch[0] = C(32, 190, 140, 18);
        g.meter = C(193, 195, 80, 8);
        g.pitch[1] = C(294, 190, 140, 18);
        g.cross = C(78, 212, 310, 44);
        g.c_master = C(22, 212, 52, 44);
        g.c_studio = C(392, 212, 52, 44);
        g.status = C(103, 259, 260, 18);
        g.strip[1] = C(28, 282, 410, 48);
        g.wave[1] = C(56, 334, 354, 48);
        g.head[1] = C(90, 386, 286, 30);
        g.f_head = C(90, 50, 286, 30);
        g.f_info = C(70, 83, 326, 18);
        g.f_wave = C(40, 104, 386, 80);
        g.f_pitch = C(24, 190, 418, 44);
        g.f_loop = C(20, 240, 426, 48);
        g.f_trans = C(28, 294, 410, 48);
        g.f_nav = C(76, 348, 314, 44);
        g.l_head = C(103, 40, 260, 20);
        g.l_close = C(163, 64, 140, 40);
        g.l_list = C(50, 112, 366, 316);
        g.j_disc = C(23, 23, 420, 420);
        g.j_back = g.j_play = g.j_cue = (rc_t){0, 0, 0, 0};
#undef C
    }
}

/* ------------------------------------------------------------------ helpers */

static solco_track_info_t s_info[2];
static unsigned s_info_ticks;

/* Track info changes only on load/analysis: poll it on change and once a second. */
static void refresh_info(void)
{
    bool poll = ++s_info_ticks % 25 == 0;
    for (int i = 0; i < 2; ++i)
        if (poll || s_first || s_st.track[i] != s_prev.track[i] || s_st.length[i] != s_prev.length[i])
            if (!solco_track_info(i, &s_info[i]))
                s_info[i].bpm_q16 = 0;
}

/* Effective tempo in tenths of BPM, 0 while unknown. */
static int bpm10(int deck)
{
    if (s_info[deck].bpm_q16 <= 0)
        return 0;
    int64_t speed = 1000000 + s_st.rate_ppm[deck];
    return (int)((int64_t)s_info[deck].bpm_q16 * 10 * speed / 1000000 / 65536);
}

/* Loop length in beats (x100) when the tempo is known, else 0. */
static int loop_beats100(int deck)
{
    if (s_info[deck].bpm_q16 <= 0 || s_st.loop_out[deck] <= s_st.loop_in[deck])
        return 0;
    int64_t len = s_st.loop_out[deck] - s_st.loop_in[deck];
    return (int)(len * s_info[deck].bpm_q16 * 100 / (65536LL * 44100 * 60));
}

static void fmt_pitch(char *out, size_t size, int deck, bool with_letter)
{
    int32_t ppm = s_st.rate_ppm[deck];
    bool reverse = s_st.rate[deck] < 0;
    char letter[4] = "";
    if (with_letter)
        snprintf(letter, sizeof(letter), "%c ", 'A' + deck);
    char tempo[24] = "";
    int bpm = bpm10(deck);
    if (bpm)
        snprintf(tempo, sizeof(tempo), "%d.%d ", bpm / 10, bpm % 10);
    snprintf(out, size, "%s%s%s%c%ld.%02ld%%%s", letter, tempo, reverse ? "REV " : "", ppm < 0 ? '-' : '+',
             (long)(labs(ppm) / 10000), (long)(labs(ppm) % 10000 / 100),
             s_st.keylock[deck] ? (bpm ? " K" : " KEY") : "");
}

static void fmt_time(char *out, size_t size, int deck)
{
    if (s_st.preparing[deck])
        snprintf(out, size, "%d%%", s_st.prepare_percent[deck]);
    else if (!s_st.ready[deck])
        snprintf(out, size, "--:--");
    else
    {
        /* Remaining time when the length is known (CDJ default), else elapsed. */
        uint32_t length = s_st.length[deck];
        bool remain = length && s_st.frames[deck] <= length;
        unsigned seconds = (remain ? length - s_st.frames[deck] + 44099 : s_st.frames[deck]) / 44100;
        snprintf(out, size, "%s%02u:%02u", remain ? "-" : "", seconds / 60, seconds % 60);
    }
}

static int wave_mode(int deck)
{
    if (s_st.preparing[deck])
        return SK_WAVE_LOADING;
    if (!s_st.ready[deck])
        return SK_WAVE_EMPTY;
    return s_st.scratching[deck] ? SK_WAVE_TOUCH : SK_WAVE_PLAY;
}

static void render_wave(sk_wave_t *wave, int deck)
{
    if (!wave)
        return;
    sk_wave_feed(wave, s_st.wave[deck], (const uint8_t(*)[3])s_st.wave_bands[deck], s_st.frames[deck]);
    sk_wave_state_t st = {.mode = wave_mode(deck), .percent = s_st.prepare_percent[deck],
                          .loop_on = s_st.looping[deck], .marked = s_st.loop_marked[deck],
                          .loop_in = s_st.loop_in[deck], .loop_out = s_st.loop_out[deck],
                          .cues = s_st.hotcue_set[deck]};
    memcpy(st.cue, s_st.hotcue_frame[deck], sizeof(st.cue));
    sk_wave_render(wave, s_st.frames[deck], &st);
}

static lv_obj_t *chip(lv_obj_t *parent, int deck, int x, int y, int w, int h)
{
    sk_tone_t t = sk_tone(sk_deck_color(deck));
    lv_obj_t *obj = sk_label(parent, &lv_font_montserrat_16, t.neon, x, y, w, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(obj, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(t.g2), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(t.neon), 0);
    lv_obj_set_style_border_width(obj, TK_LW_HAIR, 0);
    lv_obj_set_style_radius(obj, TK_R_XS, 0);
    lv_obj_set_style_pad_top(obj, (h - lv_font_get_line_height(&lv_font_montserrat_16)) / 2, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(t.g1), LV_STATE_PRESSED);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, TK_SP_2);
    return obj;
}

static void set_title(lv_obj_t *label, int deck)
{
    if (!label)
        return;
    bool empty = !s_st.title[deck][0];
    char text[80];
    snprintf(text, sizeof(text), LV_SYMBOL_DIRECTORY " %s", empty ? "CARICA BRANO" : s_st.title[deck]);
    sk_set_text(label, text);
    sk_set_color(label, empty ? TK_TEXT_3 : TK_TEXT);
}

static void info_alert(const char *text)
{
    snprintf(s_info_alert, sizeof(s_info_alert), "%s", text);
    s_info_alert_until = lv_tick_get() + 1800;
}

static void loop_action(int deck, int action, sk_strip_t *strip, int cell)
{
    if (!solco_loop(deck, action))
    {
        sk_strip_flash(strip, cell);
        info_alert(action == 1 ? "OUT PRIMA DI IN: RIPETI" : "LOOP NON VALIDO");
    }
}

/* ------------------------------------------------------------------ speed */

/* Old "VELOCITA" panel: fixed speeds both ways, one tap away from DECKS and FOCUS. */
static const int32_t k_speeds[8] = {-131072, -98304, -65536, -32768, 32768, 65536, 98304, 131072};
static const char *k_speed_text[8] = {LV_SYMBOL_LEFT "2", LV_SYMBOL_LEFT "1.5", LV_SYMBOL_LEFT "1",
                                      LV_SYMBOL_LEFT ".5", ".5" LV_SYMBOL_RIGHT, "1" LV_SYMBOL_RIGHT,
                                      "1.5" LV_SYMBOL_RIGHT, "2" LV_SYMBOL_RIGHT};
static sk_strip_t *d_speed[2], *f_speed;

static void fmt_speed(char *out, size_t size, int deck)
{
    int32_t rate = s_st.rate[deck];
    int r10 = (int)(((int64_t)(rate < 0 ? -rate : rate) * 10 + 32768) / 65536);
    if (rate < 0)
        snprintf(out, size, LV_SYMBOL_LEFT "%d.%d", r10 / 10, r10 % 10);
    else
        snprintf(out, size, "%d.%d" LV_SYMBOL_RIGHT, r10 / 10, r10 % 10);
}

static void update_speed(sk_strip_t *s, int deck)
{
    for (int i = 0; i < 8; ++i)
    {
        int32_t diff = s_st.rate[deck] - k_speeds[i];
        bool on = diff > -1311 && diff < 1311;
        sk_strip_set(s, i, k_speed_text[i], k_speeds[i] < 0 ? SK_FX : SK_CUE, on ? SK_ON : SK_IDLE);
    }
}

static void speed_open(int deck, bool open);

static void speed_cb(sk_strip_t *strip, int cell, bool pressed)
{
    if (!pressed)
        return;
    int deck = strip->tag & 1;
    solco_rate(deck, k_speeds[cell]);
    speed_open(strip->tag, false);
}

static sk_strip_t *speed_create(lv_obj_t *parent, rc_t r, int tag)
{
    sk_strip_t *s = sk_strip_create(parent, r.x, r.y, r.w, r.h, 8, NULL, speed_cb, tag);
    s->font = &lv_font_montserrat_14;
    lv_obj_set_hidden(s->obj, true);
    return s;
}

/* ------------------------------------------------------------------ DECKS */

static const uint8_t k_deck_weights[6] = {26, 17, 21, 11, 11, 14};

static void deck_strip_cb(sk_strip_t *strip, int cell, bool pressed)
{
    int deck = strip->tag;
    if (!pressed)
        return; /* live actions fire on press */
    switch (cell)
    {
    case 0: solco_toggle(deck); break;
    case 1: solco_cue(deck); break;
    case 2:
        if (s_st.looping[deck])
            loop_action(deck, 2, strip, cell);
        else if (bpm10(deck))
        {
            if (!solco_loop_beats(deck, 4))
                sk_strip_flash(strip, cell);
        }
        else if (s_st.loop_marked[deck] && s_st.loop_out[deck] <= s_st.loop_in[deck])
            loop_action(deck, 1, strip, cell);
        else
            loop_action(deck, 0, strip, cell);
        break;
    case 3: loop_action(deck, 3, strip, cell); break;
    case 4: loop_action(deck, 4, strip, cell); break;
    case 5: speed_open(deck, true); break;
    }
}

static void update_deck_strip(sk_strip_t *s, int deck)
{
    uint32_t neon = sk_deck_color(deck);
    bool ready = s_st.ready[deck];
    sk_strip_set(s, 0, s_st.playing[deck] ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, neon,
                 !ready ? SK_OFF : s_st.playing[deck] ? SK_ON : SK_IDLE);
    sk_strip_set(s, 1, "CUE", SK_CUE, ready ? SK_ARMED : SK_OFF);
    bool has = s_st.loop_out[deck] > s_st.loop_in[deck];
    if (s_st.looping[deck])
        sk_strip_set(s, 2, "EXIT", SK_LOOP, SK_ON);
    else if (s_st.loop_marked[deck] && !has)
        sk_strip_set(s, 2, "LOOP OUT", SK_LOOP, SK_ARMED);
    else
        sk_strip_set(s, 2, bpm10(deck) ? "LOOP 4" : "LOOP IN", SK_LOOP, ready ? SK_IDLE : SK_OFF);
    sk_strip_set(s, 3, "1/2", SK_LOOP, has ? SK_IDLE : SK_OFF);
    sk_strip_set(s, 4, "X2", SK_LOOP, has ? SK_IDLE : SK_OFF);
    char speed[16];
    fmt_speed(speed, sizeof(speed), deck);
    sk_strip_set(s, 5, speed, s_st.rate[deck] < 0 ? SK_FX : SK_CUE, s_st.rate[deck] == 65536 ? SK_IDLE : SK_ON);
    sk_strip_tick(s);
}

static void cross_cb(sk_fader_t *fader, int value)
{
    (void)fader;
    solco_cross(value);
}

static void open_focus_event(lv_event_t *e)
{
    show(SOLCO_VIEW_FOCUS, (int)(intptr_t)lv_event_get_user_data(e));
}

static void library_event(lv_event_t *e)
{
    open_library((int)(intptr_t)lv_event_get_user_data(e));
}

static sk_strip_t *d_master, *d_studio;

static void center_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)cell;
    if (pressed)
        return;
    if (strip->tag == 0)
        solco_master_toggle();
    else
        solco_ui_show(SOLCO_VIEW_STUDIO, 0);
}

static void create_decks(void)
{
    s_decks = sk_panel(s_root, 0, 0, s_w, s_h, false);
    for (int i = 0; i < 2; ++i)
    {
        rc_t h = g.head[i];
        int chip_w = 44 * s_w / (s_landscape ? 448 : 466);
        int time_w = 84 * s_w / (s_landscape ? 448 : 466);
        d_chip[i] = chip(s_decks, i, h.x, h.y + 2, chip_w, h.h - 4);
        lv_label_set_text_fmt(d_chip[i], "%c " LV_SYMBOL_RIGHT, 'A' + i);
        lv_obj_set_style_text_font(d_chip[i], &lv_font_montserrat_14, 0);
        lv_obj_add_event_cb(d_chip[i], open_focus_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d_title[i] = sk_label(s_decks, &lv_font_montserrat_16, TK_TEXT, h.x + chip_w + TK_SP_2,
                              h.y + (h.h - 19) / 2, h.w - chip_w - time_w - TK_SP_3, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_flag(d_title[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(d_title[i], TK_SP_2);
        lv_obj_add_event_cb(d_title[i], library_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d_time[i] = sk_label(s_decks, &lv_font_montserrat_24, sk_deck_color(i), h.x + h.w - time_w,
                             h.y + (h.h - 28) / 2, time_w, LV_TEXT_ALIGN_RIGHT);
        rc_t w = g.wave[i];
        /* Paged: the playhead sweeps a still page, only crossed columns redraw. */
        d_wave[i] = sk_wave_create(s_decks, w.x, w.y, w.w, w.h, sk_deck_color(i), true, 2 * SK_WAVE_BIN);
        if (d_wave[i])
        {
            lv_obj_add_flag(d_wave[i]->img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(d_wave[i]->img, open_focus_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        rc_t s = g.strip[i];
        d_strip[i] = sk_strip_create(s_decks, s.x, s.y, s.w, s.h, 6, k_deck_weights, deck_strip_cb, i);
        d_strip[i]->font = &lv_font_montserrat_16;
        d_speed[i] = speed_create(s_decks, s, i);
        rc_t p = g.pitch[i];
        d_pitch[i] = sk_hud(s_decks, "", TK_TEXT_2, p.x, p.y, p.w,
                            i ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, true);
    }
    d_meter = sk_meter_create(s_decks, g.meter.x, g.meter.y, g.meter.w, g.meter.h);
    d_cross = sk_fader_create(s_decks, g.cross.x, g.cross.y, g.cross.w, g.cross.h, SK_FADER_CROSS, 0,
                              1000, 500, TK_TEXT, cross_cb, 0);
    d_status = sk_hud(s_decks, "SOLCO //", TK_TEXT_3, g.status.x, g.status.y, g.status.w,
                      LV_TEXT_ALIGN_CENTER, true);
    /* One tap to the MASTER sheet (volume) and to STUDIO, as the old mixer had. */
    d_master = sk_strip_create(s_decks, g.c_master.x, g.c_master.y, g.c_master.w, g.c_master.h, 1, NULL,
                               center_cb, 0);
    d_studio = sk_strip_create(s_decks, g.c_studio.x, g.c_studio.y, g.c_studio.w, g.c_studio.h, 1, NULL,
                               center_cb, 1);
    sk_strip_set(d_master, 0, LV_SYMBOL_VOLUME_MAX, TK_CYAN, SK_IDLE);
    sk_strip_set(d_studio, 0, LV_SYMBOL_AUDIO, SK_FX, SK_ARMED);
}

static void refresh_decks(void)
{
    char text[80];
    for (int i = 0; i < 2; ++i)
    {
        if (s_first || strcmp(s_st.title[i], s_prev.title[i]))
        {
            set_title(d_title[i], i);
            if (d_wave[i] && !s_first)
                sk_wave_reset(d_wave[i]);
        }
        fmt_time(text, sizeof(text), i);
        sk_set_text(d_time[i], text);
        sk_set_color(d_time[i], s_st.preparing[i] ? TK_AMBER : s_st.ready[i] ? sk_deck_color(i) : TK_TEXT_3);
        fmt_pitch(text, sizeof(text), i, true);
        sk_set_text(d_pitch[i], text);
        sk_set_color(d_pitch[i], s_st.rate_ppm[i] || s_st.rate[i] < 0 ? TK_AMBER : TK_TEXT_2);
        if (s_st.preparing[i])
        {
            snprintf(text, sizeof(text), "%c MP3 %d%%", 'A' + i, s_st.prepare_percent[i]);
            sk_set_text(d_pitch[i], text);
            sk_set_color(d_pitch[i], TK_AMBER);
        }
        render_wave(d_wave[i], i);
        update_deck_strip(d_strip[i], i);
        if (!lv_obj_is_hidden(d_speed[i]->obj))
            update_speed(d_speed[i], i);
    }
    sk_fader_set(d_cross, s_st.cross);
    sk_meter_set(d_meter, s_st.peak);
    if ((int32_t)(s_info_alert_until - lv_tick_get()) > 0)
    {
        sk_set_text(d_status, s_info_alert);
        sk_set_color(d_status, TK_RED);
    }
    else if (s_st.error[0])
    {
        sk_set_text(d_status, s_st.error);
        sk_set_color(d_status, TK_RED);
    }
    else if (s_st.underruns)
    {
        snprintf(text, sizeof(text), "BUFFER %lu", (unsigned long)s_st.underruns);
        sk_set_text(d_status, text);
        sk_set_color(d_status, TK_AMBER);
    }
    else
    {
        int b = (s_st.cross + 5) / 10;
        const char *kit = solco_kit_tag();
        snprintf(text, sizeof(text), "A %d:%d B // %s // %s", 100 - b, b, kit[0] ? kit : "44.1K",
                 s_st.volume ? "LIVE" : "VOL 0");
        sk_set_text(d_status, text);
        sk_set_color(d_status, s_st.volume ? TK_TEXT_3 : TK_AMBER);
    }
}

/* ------------------------------------------------------------------ FOCUS */

static const uint8_t k_loop_weights[5] = {20, 20, 16, 28, 16};
static const uint8_t k_beat_weights[6] = {16, 17, 17, 17, 17, 16};
static const uint8_t k_trans_weights[6] = {24, 16, 15, 15, 15, 15};
static const uint8_t k_nav_weights[6] = {20, 15, 14, 14, 21, 16};
static const int k_beats[4] = {1, 2, 4, 8};
static lv_timer_t *s_cue_timer;
static int s_cue_slot = -1;
/* Optimistic hot cue state: a cue just stored counts as set before the engine
 * status shows it, so a quick second tap jumps instead of moving it. */
static uint8_t s_cue_local[2], s_cue_cleared[2];
static uint32_t s_cue_start;

static int64_t ppm_to_rate(int32_t ppm, bool reverse)
{
    int64_t rate = SOLCO_SAMPLE_ONE + SOLCO_SAMPLE_ONE / 1000000 * ppm + SOLCO_SAMPLE_ONE % 1000000 * ppm / 1000000;
    return reverse ? -rate : rate;
}

static void pitch_cb(sk_fader_t *fader, int value)
{
    (void)fader;
    solco_rate_fine(s_deck, ppm_to_rate(value, s_st.rate[s_deck] < 0));
}

static void focus_loop_cb(sk_strip_t *strip, int cell, bool pressed)
{
    if (!pressed)
        return;
    bool beats = bpm10(s_deck) > 0;
    int last = strip->n - 1;
    if (s_st.looping[s_deck] && (cell == 0 || cell == (beats ? last : 1)))
    {
        /* Active loop: the outer cells shift the loop by its length (M10b),
         * applied at the next loop edge so a forward shift never jumps. */
        if (!solco_loop_shift(s_deck, cell == 0 ? -1 : 1, false, false))
        {
            sk_strip_flash(strip, cell);
            info_alert("SHIFT FUORI BRANO");
        }
        return;
    }
    if (beats)
    {
        if (cell == 0 || cell == last)
            loop_action(s_deck, cell == 0 ? 0 : 1, strip, cell); /* manual IN / OUT */
        else if (s_st.looping[s_deck] && loop_beats100(s_deck) / 100 == k_beats[cell - 1])
            loop_action(s_deck, 2, strip, cell); /* lit size again = exit */
        else if (!solco_loop_beats(s_deck, k_beats[cell - 1]))
            sk_strip_flash(strip, cell);
        return;
    }
    static const int actions[5] = {0, 1, 3, 2, 4};
    loop_action(s_deck, actions[cell], strip, cell);
}

static void cue_hold(lv_timer_t *timer)
{
    (void)timer;
    uint32_t held = lv_tick_get() - s_cue_start;
    sk_strip_progress(f_trans, 2 + s_cue_slot, (int)(held * 255 / SK_HOLD_MS));
    if (held >= SK_HOLD_MS)
    {
        solco_hotcue(s_deck, s_cue_slot, 2); /* hold 1 s on a set cue = clear */
        s_cue_local[s_deck] &= (uint8_t)~(1u << s_cue_slot);
        s_cue_cleared[s_deck] |= (uint8_t)(1u << s_cue_slot);
        sk_strip_progress(f_trans, 2 + s_cue_slot, 0);
        lv_timer_delete(s_cue_timer);
        s_cue_timer = NULL;
        s_cue_slot = -1;
    }
}

static void cue_release(void)
{
    if (s_cue_timer)
        lv_timer_delete(s_cue_timer);
    if (s_cue_slot >= 0 && f_trans)
        sk_strip_progress(f_trans, 2 + s_cue_slot, 0);
    s_cue_timer = NULL;
    s_cue_slot = -1;
}

static void focus_trans_cb(sk_strip_t *strip, int cell, bool pressed)
{
    if (!pressed)
    {
        cue_release();
        return;
    }
    if (cell == 0)
        solco_toggle(s_deck);
    else if (cell == 1)
        solco_cue(s_deck);
    else
    {
        int slot = cell - 2;
        uint8_t bit = (uint8_t)(1u << slot);
        bool set = ((s_st.hotcue_set[s_deck] & ~s_cue_cleared[s_deck]) | s_cue_local[s_deck]) & bit;
        /* Empty: store the playhead. Set: jump now (press = lowest latency). */
        if (!solco_hotcue(s_deck, slot, set ? 1 : 0))
            sk_strip_flash(strip, cell);
        else if (!set)
        {
            s_cue_local[s_deck] |= bit;
            s_cue_cleared[s_deck] &= (uint8_t)~bit;
        }
        else
        {
            s_cue_slot = slot;
            s_cue_start = lv_tick_get();
            if (!s_cue_timer)
                s_cue_timer = lv_timer_create(cue_hold, SK_TICK_MS * 2, NULL);
        }
    }
}

static void focus_nav_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    if (cell == 2)
    {
        /* REV: hold to play backwards at the current pitch, release to return. */
        if (pressed && s_st.ready[s_deck])
        {
            s_rev_hold = true;
            s_rev_ppm = s_st.rate_ppm[s_deck];
            solco_rate_fine(s_deck, ppm_to_rate(s_rev_ppm, true));
        }
        else if (!pressed && s_rev_hold)
        {
            s_rev_hold = false;
            solco_rate_fine(s_deck, ppm_to_rate(s_rev_ppm, false));
        }
        return;
    }
    if (cell == 1)
    {
        if (pressed)
            solco_keylock(s_deck, !s_st.keylock[s_deck]);
        return;
    }
    if (cell == 3)
    {
        if (pressed)
            speed_open(2 + s_deck, lv_obj_is_hidden(f_speed->obj));
        return;
    }
    if (pressed)
        return; /* navigation fires on release */
    if (cell == 0)
        show(SOLCO_VIEW_DECKS, s_deck);
    else if (cell == 4)
        show(SOLCO_VIEW_JOG, s_deck);
    else
        show(SOLCO_VIEW_FOCUS, !s_deck);
}

/* Touch strip on the big waveform: hold = hand on the record, drag = scrub. */
static void focus_wave_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *input = lv_indev_active();
    lv_point_t p = {0, 0};
    if (input)
        lv_indev_get_point(input, &p);
    if (code == LV_EVENT_PRESSED && s_st.ready[s_deck])
    {
        f_touch = true;
        f_touch_x = p.x;
        solco_scratch(s_deck, 0, 0);
    }
    else if (code == LV_EVENT_PRESSING && f_touch)
    {
        int dx = p.x - f_touch_x;
        if (dx)
        {
            f_touch_x = p.x;
            solco_scratch(s_deck, 1, -dx * (SK_WAVE_BIN / SK_WAVE_COL));
        }
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && f_touch)
    {
        f_touch = false;
        solco_scratch(s_deck, 2, 0);
    }
}

static void focus_chip_event(lv_event_t *e)
{
    (void)e;
    show(SOLCO_VIEW_DECKS, s_deck);
}

static void focus_title_event(lv_event_t *e)
{
    (void)e;
    open_library(s_deck);
}

static void create_focus(void)
{
    s_focus = sk_panel(s_root, 0, 0, s_w, s_h, false);
    rc_t h = g.f_head;
    int chip_w = 52 * s_w / (s_landscape ? 448 : 466);
    int time_w = 84 * s_w / (s_landscape ? 448 : 466);
    f_chip = chip(s_focus, 0, h.x, h.y + 2, chip_w, h.h - 4);
    lv_obj_set_style_text_font(f_chip, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(f_chip, focus_chip_event, LV_EVENT_CLICKED, NULL);
    f_title = sk_label(s_focus, &lv_font_montserrat_16, TK_TEXT, h.x + chip_w + TK_SP_2, h.y + (h.h - 19) / 2,
                       h.w - chip_w - time_w - TK_SP_3, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(f_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(f_title, TK_SP_2);
    lv_obj_add_event_cb(f_title, focus_title_event, LV_EVENT_CLICKED, NULL);
    f_time = sk_label(s_focus, &lv_font_montserrat_24, TK_CYAN, h.x + h.w - time_w, h.y + (h.h - 28) / 2,
                      time_w, LV_TEXT_ALIGN_RIGHT);
    f_info = sk_hud(s_focus, "", TK_TEXT_2, g.f_info.x, g.f_info.y, g.f_info.w,
                    s_round ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, true);
    for (int i = 0; i < 2; ++i)
    {
        f_wave[i] = sk_wave_create(s_focus, g.f_wave.x, g.f_wave.y, g.f_wave.w, g.f_wave.h, sk_deck_color(i),
                                   false, SK_WAVE_BIN);
        if (f_wave[i])
        {
            lv_obj_add_flag(f_wave[i]->img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(f_wave[i]->img, LV_OBJ_FLAG_GESTURE_BUBBLE);
            lv_obj_add_event_cb(f_wave[i]->img, focus_wave_event, LV_EVENT_ALL, NULL);
        }
    }
    f_pitch = sk_fader_create(s_focus, g.f_pitch.x, g.f_pitch.y, g.f_pitch.w, g.f_pitch.h, SK_FADER_PITCH,
                              -SK_PITCH_PPM, SK_PITCH_PPM, 0, TK_CYAN, pitch_cb, 0);
    f_loop = sk_strip_create(s_focus, g.f_loop.x, g.f_loop.y, g.f_loop.w, g.f_loop.h, 5, k_loop_weights,
                             focus_loop_cb, 0);
    f_trans = sk_strip_create(s_focus, g.f_trans.x, g.f_trans.y, g.f_trans.w, g.f_trans.h, 6, k_trans_weights,
                              focus_trans_cb, 0);
    f_speed = speed_create(s_focus, g.f_pitch, 2 + 0);
    f_nav = sk_strip_create(s_focus, g.f_nav.x, g.f_nav.y, g.f_nav.w, g.f_nav.h, 6, k_nav_weights,
                            focus_nav_cb, 0);
    f_nav->font = &lv_font_montserrat_14;
}

static void focus_apply_deck(void)
{
    sk_tone_t t = sk_tone(sk_deck_color(s_deck));
    lv_label_set_text_fmt(f_chip, LV_SYMBOL_LEFT " %c", 'A' + s_deck);
    lv_obj_set_style_text_color(f_chip, lv_color_hex(t.neon), 0);
    lv_obj_set_style_border_color(f_chip, lv_color_hex(t.neon), 0);
    lv_obj_set_style_bg_color(f_chip, lv_color_hex(t.g2), 0);
    lv_obj_set_style_bg_color(f_chip, lv_color_hex(t.g1), LV_STATE_PRESSED);
    for (int i = 0; i < 2; ++i)
        if (f_wave[i])
            lv_obj_set_hidden(f_wave[i]->img, i != s_deck);
    f_pitch->neon = t.neon;
    lv_obj_invalidate(f_pitch->obj);
    char text[20];
    snprintf(text, sizeof(text), LV_SYMBOL_SHUFFLE " %c", 'A' + !s_deck);
    sk_strip_set(f_nav, 0, LV_SYMBOL_LEFT " DECKS", TK_TEXT, SK_IDLE);
    sk_strip_set(f_nav, 4, "SCRATCH", t.neon, SK_ARMED);
    sk_strip_set(f_nav, 5, text, sk_deck_color(!s_deck), SK_IDLE);
    speed_open(2 + s_deck, false);
}

static void refresh_focus(void)
{
    int d = s_deck;
    char text[96];
    uint32_t neon = sk_deck_color(d);
    set_title(f_title, d);
    fmt_time(text, sizeof(text), d);
    sk_set_text(f_time, text);
    sk_set_color(f_time, s_st.preparing[d] ? TK_AMBER : s_st.ready[d] ? neon : TK_TEXT_3);
    if ((int32_t)(s_info_alert_until - lv_tick_get()) > 0)
    {
        sk_set_text(f_info, s_info_alert);
        sk_set_color(f_info, TK_RED);
    }
    else if (s_st.error[0])
    {
        sk_set_text(f_info, s_st.error);
        sk_set_color(f_info, TK_RED);
    }
    else
    {
        char pitch[48], loop[40] = "";
        fmt_pitch(pitch, sizeof(pitch), d, false);
        int beats100 = loop_beats100(d);
        if (beats100)
            snprintf(loop, sizeof(loop), "  //  LOOP %d.%02d B", beats100 / 100, beats100 % 100);
        else if (s_st.loop_out[d] > s_st.loop_in[d])
            snprintf(loop, sizeof(loop), "  //  LOOP %.2fS", (s_st.loop_out[d] - s_st.loop_in[d]) / 44100.f);
        else if (s_st.loop_marked[d])
            snprintf(loop, sizeof(loop), "  //  IN OK");
        int32_t rate = s_st.rate[d];
        int r10 = (int)(((int64_t)(rate < 0 ? -rate : rate) * 10 + 32768) / 65536);
        snprintf(text, sizeof(text), "%s %d.%dX // %s%s%s%s", rate < 0 ? "INDIETRO" : "AVANTI", r10 / 10, r10 % 10,
                 bpm10(d) ? "BPM " : "PITCH ", pitch, s_st.keylock[d] ? " // TONO FISSO" : "", loop);
        sk_set_text(f_info, text);
        sk_set_color(f_info, s_st.looping[d] ? SK_LOOP : TK_TEXT_2);
    }
    render_wave(f_wave[d], d);
    sk_fader_set(f_pitch, s_st.rate_ppm[d] < -SK_PITCH_PPM ? -SK_PITCH_PPM
                          : s_st.rate_ppm[d] > SK_PITCH_PPM ? SK_PITCH_PPM
                                                            : s_st.rate_ppm[d]);
    bool ready = s_st.ready[d], has = s_st.loop_out[d] > s_st.loop_in[d];
    bool waiting = s_st.loop_marked[d] && !has, beats = bpm10(d) > 0;
    sk_strip_layout(f_loop, beats ? 6 : 5, beats ? k_beat_weights : k_loop_weights);
    int last = beats ? 5 : 1;
    if (s_st.looping[d])
    {
        /* Shift armed: lit until the playhead reaches the loop edge. */
        int shift = s_st.shift_pending[d] ? SK_ON : SK_ARMED;
        sk_strip_set(f_loop, 0, LV_SYMBOL_PREV, SK_LOOP, shift);
        sk_strip_set(f_loop, last, LV_SYMBOL_NEXT, SK_LOOP, shift);
    }
    else
    {
        sk_strip_set(f_loop, 0, "IN", SK_LOOP, !ready ? SK_OFF : waiting ? SK_ARMED : SK_IDLE);
        sk_strip_set(f_loop, last, "OUT", SK_LOOP, waiting ? SK_ARMED : SK_OFF);
    }
    if (beats)
    {
        int lit = s_st.looping[d] ? loop_beats100(d) : 0;
        for (int i = 0; i < 4; ++i)
        {
            char size[4];
            snprintf(size, sizeof(size), "%d", k_beats[i]);
            bool on = lit && abs(lit - k_beats[i] * 100) <= 2;
            sk_strip_set(f_loop, 1 + i, size, SK_LOOP, !ready ? SK_OFF : on ? SK_ON : SK_IDLE);
        }
    }
    else
    {
        sk_strip_set(f_loop, 2, "1/2", SK_LOOP, has ? SK_IDLE : SK_OFF);
        sk_strip_set(f_loop, 3, s_st.looping[d] ? "EXIT" : has ? "RELOOP" : "LOOP", SK_LOOP,
                     s_st.looping[d] ? SK_ON : has ? SK_ARMED : SK_OFF);
        sk_strip_set(f_loop, 4, "X2", SK_LOOP, has ? SK_IDLE : SK_OFF);
    }
    sk_strip_set(f_trans, 0, s_st.playing[d] ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, neon,
                 !ready ? SK_OFF : s_st.playing[d] ? SK_ON : SK_IDLE);
    sk_strip_set(f_trans, 1, "CUE", SK_CUE, ready ? SK_ARMED : SK_OFF);
    for (int i = 0; i < SOLCO_HOTCUES && i < 4; ++i)
    {
        char slot[2] = {(char)('1' + i), 0};
        uint8_t bit = (uint8_t)(1u << i);
        if (s_st.hotcue_set[d] & bit)
            s_cue_local[d] &= (uint8_t)~bit;
        else
            s_cue_cleared[d] &= (uint8_t)~bit;
        bool set = ((s_st.hotcue_set[d] & ~s_cue_cleared[d]) | s_cue_local[d]) & bit;
        sk_strip_set(f_trans, 2 + i, slot, SK_CUE, !ready ? SK_OFF : set ? SK_ON : SK_IDLE);
    }
    sk_strip_set(f_nav, 1, "TONO", neon, s_st.keylock[d] ? SK_ON : SK_IDLE);
    char speed[16];
    fmt_speed(speed, sizeof(speed), d);
    sk_strip_set(f_nav, 3, speed, s_st.rate[d] < 0 ? SK_FX : SK_CUE,
                 !lv_obj_is_hidden(f_speed->obj) || s_st.rate[d] != 65536 ? SK_ON : SK_IDLE);
    if (!lv_obj_is_hidden(f_speed->obj))
    {
        f_speed->tag = 2 + d;
        update_speed(f_speed, d);
    }
    sk_strip_set(f_nav, 2, "REV", SK_FX, !ready ? SK_OFF : s_rev_hold ? SK_ON : SK_IDLE);
    sk_strip_tick(f_trans);
    sk_strip_tick(f_loop);
}

static void speed_open(int tag, bool open)
{
    if (tag >= 2)
    {
        /* FOCUS: the speed row takes the pitch fader's place while open. */
        if (!f_speed)
            return;
        lv_obj_set_hidden(f_speed->obj, !open);
        lv_obj_set_hidden(f_pitch->obj, open);
        if (open)
        {
            f_speed->tag = tag;
            update_speed(f_speed, tag & 1);
        }
        return;
    }
    lv_obj_set_hidden(d_speed[tag]->obj, !open);
    lv_obj_set_hidden(d_strip[tag]->obj, open);
    if (open)
        update_speed(d_speed[tag], tag);
}

/* ------------------------------------------------------------------ JOG */

static void jog_marker_place(void)
{
    if (!j_marker)
        return;
    int d = lv_obj_get_width(j_disc), m = lv_obj_get_width(j_marker);
    float r = d * 0.43f;
    lv_obj_set_pos(j_marker, (int)(d / 2 + cosf(j_angle) * r) - m / 2, (int)(d / 2 + sinf(j_angle) * r) - m / 2);
}

static void jog_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (j_drag)
            solco_scratch(s_deck, 2, 0);
        j_drag = j_touch_valid = false;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)
        return;
    lv_indev_t *input = lv_indev_active();
    if (!input)
        return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    lv_area_t area;
    lv_obj_get_coords(j_disc, &area);
    float dx = point.x - (area.x1 + area.x2) * 0.5f, dy = point.y - (area.y1 + area.y2) * 0.5f;
    float dead = lv_area_get_width(&area) * .17f;
    if (dx * dx + dy * dy < dead * dead)
    {
        j_touch_valid = false;
        return; /* never unwrap an angle across the spindle */
    }
    float angle = atan2f(dy, dx);
    if (code == LV_EVENT_PRESSED || !j_touch_valid)
    {
        j_touch_valid = j_drag = true;
        j_touch_angle = angle;
        solco_scratch(s_deck, 0, 0);
    }
    else if (j_drag)
    {
        float delta = angle - j_touch_angle;
        if (delta > 3.14159265f)
            delta -= 6.2831853f;
        if (delta < -3.14159265f)
            delta += 6.2831853f;
        j_touch_angle = angle;
        /* 33 1/3 RPM platter: one revolution = 1.8 s of source audio. */
        solco_scratch(s_deck, 1, (int32_t)(delta * (44100 * 1.8f / 6.2831853f)));
    }
}

static void create_grooves(int diameter)
{
    uint16_t *pixels = heap_caps_malloc((size_t)diameter * diameter * 2, MALLOC_CAP_SPIRAM);
    if (!pixels)
        return; /* the platter still works without the printed grooves */
    float center = (diameter - 1) * .5f;
    uint16_t base = sk_px(TK_SURFACE_1), groove = sk_px(TK_LINE), band = sk_px(TK_LINE_HI);
    uint16_t rim = sk_px(TK_TEXT_3);
    for (int row = 0; row < diameter; ++row)
        for (int col = 0; col < diameter; ++col)
        {
            float dx = col - center, dy = row - center;
            float radius = sqrtf(dx * dx + dy * dy);
            uint16_t c = radius > center ? 0 : base;
            if (fabsf(radius - (center - 1)) < 1)
                c = rim;
            for (int ring = 0; ring < 14 && radius < center - 3; ++ring)
            {
                float r = diameter * (93 - ring * 4) / 200.f;
                if (fabsf(radius - r) < (ring % 4 ? .55f : 1.f))
                {
                    c = ring % 4 ? groove : band;
                    break;
                }
            }
            pixels[row * diameter + col] = c;
        }
    j_grooves.header.magic = LV_IMAGE_HEADER_MAGIC;
    j_grooves.header.cf = LV_COLOR_FORMAT_RGB565;
    j_grooves.header.w = j_grooves.header.h = diameter;
    j_grooves.header.stride = diameter * 2;
    j_grooves.data_size = diameter * diameter * 2;
    j_grooves.data = (const uint8_t *)pixels;
    lv_obj_t *image = lv_image_create(j_disc);
    lv_image_set_src(image, &j_grooves);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
}

static void jog_back_event(lv_event_t *e)
{
    (void)e;
    show(SOLCO_VIEW_FOCUS, s_deck);
}

static void jog_cell_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)cell;
    if (strip->tag == 0 && !pressed)
        show(SOLCO_VIEW_FOCUS, s_deck);
    else if (strip->tag == 1 && pressed)
        solco_toggle(s_deck);
    else if (strip->tag == 2 && pressed)
        solco_cue(s_deck);
}

static void create_jog(void)
{
    s_jog = sk_panel(s_root, 0, 0, s_w, s_h, false);
    int d = g.j_disc.w;
    j_disc = lv_obj_create(s_jog);
    lv_obj_remove_style_all(j_disc);
    lv_obj_set_pos(j_disc, g.j_disc.x, g.j_disc.y);
    lv_obj_set_size(j_disc, d, d);
    lv_obj_set_style_radius(j_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(j_disc, lv_color_hex(TK_SURFACE_1), 0);
    lv_obj_set_style_bg_opa(j_disc, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(j_disc, false);
    lv_obj_add_flag(j_disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(j_disc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(j_disc, jog_touch, LV_EVENT_ALL, NULL);
    create_grooves(d);
    j_label = lv_label_create(j_disc);
    int label = d * 30 / 100;
    lv_obj_set_size(j_label, label, label);
    lv_obj_center(j_label);
    lv_obj_set_style_radius(j_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(j_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(j_label, TK_LW, 0);
    lv_obj_set_style_text_align(j_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(j_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(j_label, label / 2 - 24, 0);
    lv_obj_add_flag(j_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(j_label, jog_back_event, LV_EVENT_CLICKED, NULL);
    j_marker = lv_obj_create(j_disc);
    lv_obj_remove_style_all(j_marker);
    lv_obj_set_size(j_marker, 14, 14);
    lv_obj_set_style_radius(j_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(j_marker, LV_OPA_COVER, 0);
    lv_obj_remove_flag(j_marker, LV_OBJ_FLAG_CLICKABLE);
    if (g.j_back.w)
    {
        j_back = sk_strip_create(s_jog, g.j_back.x, g.j_back.y, g.j_back.w, g.j_back.h, 1, NULL, jog_cell_cb, 0);
        j_play = sk_strip_create(s_jog, g.j_play.x, g.j_play.y, g.j_play.w, g.j_play.h, 1, NULL, jog_cell_cb, 1);
        j_cue = sk_strip_create(s_jog, g.j_cue.x, g.j_cue.y, g.j_cue.w, g.j_cue.h, 1, NULL, jog_cell_cb, 2);
        sk_strip_set(j_back, 0, LV_SYMBOL_LEFT, TK_TEXT, SK_IDLE);
    }
}

static void jog_apply_deck(void)
{
    sk_tone_t t = sk_tone(sk_deck_color(s_deck));
    lv_label_set_text_fmt(j_label, LV_SYMBOL_LEFT " %c\nSCRATCH", 'A' + s_deck);
    lv_obj_set_style_bg_color(j_label, lv_color_hex(t.g2), 0);
    lv_obj_set_style_border_color(j_label, lv_color_hex(t.neon), 0);
    lv_obj_set_style_text_color(j_label, lv_color_hex(t.neon), 0);
    lv_obj_set_style_bg_color(j_marker, lv_color_hex(t.neon), 0);
}

static void refresh_jog(void)
{
    int d = s_deck;
    float angle = s_st.platter_phase[d] * (6.2831853f * 10 / (44100 * 18));
    if (angle != j_angle || s_first)
    {
        j_angle = angle;
        jog_marker_place();
    }
    if (j_play)
    {
        sk_strip_set(j_play, 0, s_st.playing[d] ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, sk_deck_color(d),
                     !s_st.ready[d] ? SK_OFF : s_st.playing[d] ? SK_ON : SK_IDLE);
        sk_strip_set(j_cue, 0, "CUE", SK_CUE, s_st.ready[d] ? SK_ARMED : SK_OFF);
    }
}

/* ------------------------------------------------------------------ LIBRARY */

static void lib_close_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    (void)cell;
    if (!pressed)
        show(s_lib_return, s_deck);
}

static void lib_choose(lv_event_t *e)
{
    solco_select(s_lib_deck, (int)(intptr_t)lv_event_get_user_data(e));
    if (d_wave[s_lib_deck])
        sk_wave_reset(d_wave[s_lib_deck]);
    if (f_wave[s_lib_deck])
        sk_wave_reset(f_wave[s_lib_deck]);
    show(s_lib_return, s_deck);
}

static void create_library(void)
{
    s_lib = sk_panel(s_root, 0, 0, s_w, s_h, false);
    l_head = sk_hud(s_lib, "", TK_CYAN, g.l_head.x, g.l_head.y, g.l_head.w,
                    s_round ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, false);
    l_close = sk_strip_create(s_lib, g.l_close.x, g.l_close.y, g.l_close.w, g.l_close.h, 1, NULL, lib_close_cb, 0);
    l_close->font = &lv_font_montserrat_14;
    sk_strip_set(l_close, 0, "CHIUDI", TK_TEXT, SK_IDLE);
    l_list = lv_obj_create(s_lib);
    lv_obj_remove_style_all(l_list);
    lv_obj_set_pos(l_list, g.l_list.x, g.l_list.y);
    lv_obj_set_size(l_list, g.l_list.w, g.l_list.h);
    lv_obj_set_flex_flow(l_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(l_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_bottom(l_list, s_round ? 96 : TK_SP_2, 0);
}

static void open_library(int deck)
{
    if (s_st.playing[deck] || s_st.scratching[deck])
    {
        /* Never swap an audible deck: tell where the problem is. */
        info_alert("FERMA IL DECK PER CARICARE");
        if (s_view == SOLCO_VIEW_DECKS)
            sk_strip_flash(d_strip[deck], 0);
        else if (s_view == SOLCO_VIEW_FOCUS)
            sk_strip_flash(f_trans, 0);
        return;
    }
    if (!s_lib)
        create_library();
    s_lib_deck = deck;
    s_lib_return = s_view == SOLCO_VIEW_LIBRARY ? s_lib_return : s_view;
    sk_tone_t t = sk_tone(sk_deck_color(deck));
    lv_label_set_text_fmt(l_head, "LOAD // DECK %c // WAV MP3", 'A' + deck);
    lv_obj_set_style_text_color(l_head, lv_color_hex(t.neon), 0);
    lv_obj_clean(l_list);
    int count = solco_library_count();
    for (int i = 0; i < count; ++i)
    {
        char title[80];
        solco_library_title(i, title, sizeof(title));
        bool loaded = i == s_st.track[deck];
        lv_obj_t *row = lv_obj_create(l_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), TK_TOUCH);
        lv_obj_set_scrollable(row, false);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, TK_LW_HAIR, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(TK_LINE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(TK_SURFACE_2), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, lib_choose, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *index = sk_label(row, &lv_font_montserrat_12, loaded ? t.neon : TK_TEXT_3, TK_SP_1, 17, 32,
                                   LV_TEXT_ALIGN_LEFT);
        lv_label_set_text_fmt(index, "%02d", i + 1);
        lv_obj_t *name = sk_label(row, &lv_font_montserrat_16, loaded ? t.neon : TK_TEXT, 40, 14,
                                  g.l_list.w - 48, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(name, title);
    }
    show(SOLCO_VIEW_LIBRARY, s_deck);
}

/* ------------------------------------------------------------------ views */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj && lv_obj_is_hidden(obj) != hidden)
        lv_obj_set_hidden(obj, hidden);
}

static void release_touch(void)
{
    cue_release();
    if (f_touch || j_drag)
        solco_scratch(s_deck, 2, 0);
    f_touch = j_drag = j_touch_valid = false;
    if (s_rev_hold)
    {
        s_rev_hold = false;
        solco_rate_fine(s_deck, ppm_to_rate(s_rev_ppm, false));
    }
}

static void show(int view, int deck)
{
    release_touch();
    if (view == SOLCO_VIEW_FOCUS && !s_focus)
        create_focus();
    if (view == SOLCO_VIEW_JOG && !s_jog)
        create_jog();
    if (view == SOLCO_VIEW_STUDIO)
    {
        solco_studio_open(0);
        return;
    }
    if (s_view == SOLCO_VIEW_STUDIO && solco_studio_view() >= 0)
        solco_studio_open(-1); /* closes the studio page, comes back here */
    s_view = view;
    s_deck = deck & 1;
    set_hidden(s_decks, view != SOLCO_VIEW_DECKS);
    set_hidden(s_focus, view != SOLCO_VIEW_FOCUS);
    set_hidden(s_jog, view != SOLCO_VIEW_JOG);
    set_hidden(s_lib, view != SOLCO_VIEW_LIBRARY);
    if (view == SOLCO_VIEW_FOCUS)
        focus_apply_deck();
    if (view == SOLCO_VIEW_JOG)
        jog_apply_deck();
    s_first = true;
}

void solco_ui_show(int view, int deck)
{
    solco_master_close();
    show(view, deck);
}

/* Studio closes itself through solco_studio_open(-1): keep the state machine in sync. */
void solco_ui_studio_closed(void)
{
    if (s_view == SOLCO_VIEW_STUDIO)
        s_view = SOLCO_VIEW_DECKS;
    set_hidden(s_decks, s_view != SOLCO_VIEW_DECKS);
    s_first = true;
}

void solco_ui_studio_opened(void)
{
    release_touch();
    s_view = SOLCO_VIEW_STUDIO;
    set_hidden(s_decks, true);
    set_hidden(s_focus, true);
    set_hidden(s_jog, true);
    set_hidden(s_lib, true);
}

static void back(void)
{
    if (solco_master_open())
        solco_master_close();
    else if (s_view == SOLCO_VIEW_LIBRARY)
        show(s_lib_return, s_deck);
    else if (s_view == SOLCO_VIEW_JOG)
        show(SOLCO_VIEW_FOCUS, s_deck);
    else if (s_view == SOLCO_VIEW_FOCUS)
        show(SOLCO_VIEW_DECKS, s_deck);
    else if (s_view == SOLCO_VIEW_STUDIO)
        solco_studio_open(-1);
}

bool solco_ui_home_back(void)
{
    back(); /* leaving Solco is a deliberate hold in MASTER, never a stray back */
    return true;
}

void solco_ui_button(button_id_t button, button_event_t event)
{
    if (button == BUTTON_BOOT && event == BUTTON_CLICK)
    {
        release_touch();
        solco_master_toggle();
    }
    else if (button == BUTTON_BOOT && event == BUTTON_LONG_PRESS)
        back();
    else
        app_manager_button(button, event);
}

static void knob_event(lv_event_t *e)
{
    lv_port_knob_t *knob = lv_event_get_param(e);
    if (solco_master_open() || s_view == SOLCO_VIEW_STUDIO)
        solco_master_knob(knob->delta);
    else if ((s_view == SOLCO_VIEW_FOCUS || s_view == SOLCO_VIEW_JOG) && s_wheel_vinyl)
        solco_jog(s_deck, knob->delta);
    else if (s_view == SOLCO_VIEW_DECKS)
    {
        int value = s_st.cross + knob->delta * 20;
        solco_cross(value < 0 ? 0 : value > 1000 ? 1000 : value);
    }
    else
        return; /* library: the list scrolls */
    knob->handled = true;
}

void solco_ui_set_volume(int value, const char *source)
{
    s_volume_source = source;
    ++s_volume_changes;
    solco_volume(value < 0 ? 0 : value > 100 ? 100 : value);
}

const char *solco_ui_volume_source(uint32_t *changes)
{
    *changes = s_volume_changes;
    return s_volume_source;
}

bool solco_ui_wheel_vinyl(void) { return s_wheel_vinyl; }
lv_obj_t *solco_ui_root(void) { return s_root; }

void solco_ui_info(solco_ui_info_t *info)
{
    *info = (solco_ui_info_t){.width = s_w, .height = s_h, .landscape = s_landscape, .view = s_view,
                              .deck = s_deck, .studio = solco_studio_view(), .master = solco_master_open(),
                              .studio_tools = solco_studio_tools_open()};
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    solco_status(&s_st);
    refresh_info();
    solco_kit_tick();
    sk_wave_tick();
    solco_studio_refresh();
    if (solco_master_open())
        solco_master_refresh();
    else if (s_view == SOLCO_VIEW_DECKS)
        refresh_decks();
    else if (s_view == SOLCO_VIEW_FOCUS)
        refresh_focus();
    else if (s_view == SOLCO_VIEW_JOG)
        refresh_jog();
    s_prev = s_st;
    s_first = false;
}

void solco_ui_create(void)
{
    sk_init();
    s_w = lv_display_get_horizontal_resolution(lv_port_display());
    s_h = lv_display_get_vertical_resolution(lv_port_display());
    s_round = board_info()->round;
    s_landscape = !s_round && s_w > s_h;
    geometry();
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, s_w, s_h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(TK_VOID), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_root, lv_color_hex(TK_TEXT), 0);
    lv_obj_set_scrollable(s_root, false);
    solco_status(&s_st);
    create_decks();
    solco_studio_create(s_root);
    solco_master_create(s_root);
    lv_obj_add_event_cb(s_root, knob_event, lv_port_knob_event(), NULL);
    app_manager_set_home(s_root, solco_ui_home_back);
    lv_timer_create(tick, SK_TICK_MS, NULL);
    s_first = true;
}
