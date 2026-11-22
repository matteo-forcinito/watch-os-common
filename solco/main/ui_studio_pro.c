/*
 * Studio Pro screens (SOLCO_STUDIO_PRO): CAMPIONATORE, SYNTH, SEQUENCER.
 * Designed on 480 x 320 (Solco is landscape on rectangular panels) and scaled.
 *
 *  CAMPIONATORE                          SYNTH                         SEQUENCER
 *  ┌< CAMPIONATORE // SLOT 3 · 1.2 s┐  ┌< SYNTH // [BASSO>] [-]C3[+]┐  ┌< SEQUENCER // [▶] [-]120[+]┐
 *  │ ▁▃▇█▇▅▃▂▁▁▃▅▇▅▃ |start  end|    │  │ ONDA FILTRO RISON INVIL ...│  │[BATT][CAMP][MELO] SWING CLR │
 *  │ REC PROVA TAGLIA NORM INV FADE  │  │ ATT DEC SUS RIL DETUNE ... │  │ KICK ■□□□■□□□■□□□■□□□       │
 *  │ PIT- PIT+ CALDO SALVA CARICA 8F │  │ ┌┬┬┬┬┬┬┬┬┬┬┬┬┐ keyboard   │  │ ...  8 rows x 16 steps      │
 *  │ [1][2][3][4][5][6][7][8] pads   │  │ └┴┴┴┴┴┴┴┴┴┴┴┴┘            │  │                             │
 *  └─────────────────────────────────┘  └────────────────────────────┘  └─────────────────────────────┘
 */
#include "engine.h"
#include "solco_ui.h"
#include "studio.h"
#include "studio_pro.h"
#include "ui/ui.h"
#include "ui_kit.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVE_COLS 116

enum
{
    SCREEN_SAMPLER,
    SCREEN_SYNTH,
    SCREEN_SEQ,
};

static lv_obj_t *s_parent;
static lv_obj_t *s_panel[3];
static int s_screen = -1;
static int s_w = 480, s_h = 320;

/* sampler */
static int s_slot;
static int s_trim_a, s_trim_b = 1000;   /* per mille */
static lv_obj_t *s_wave, *s_slot_info, *s_pads[PRO_SLOTS], *s_rec_overlay, *s_rec_label, *s_rec_level;
static uint16_t s_peaks[WAVE_COLS];
static bool s_has_wave;
static pro_capture_state_t s_cap_seen = PRO_CAPTURE_IDLE;

/* synth */
static lv_obj_t *s_preset_label, *s_octave_label, *s_wave_label;
static lv_obj_t *s_sliders[11];
static int s_preset;
static int s_octave = 4;   /* keyboard starts at C of this octave */
static int s_key_down = -1;

/* sequencer */
static int s_tab;   /* 0 drums (Studio beat), 1 samples, 2 melody */
static lv_obj_t *s_cells[8][PRO_STEPS], *s_row_names[8], *s_tabs[3], *s_play_label, *s_bpm_label, *s_swing_label;
static int s_seq_step = -2;
static uint32_t s_seq_revision;

static int sx(int v) { return v * s_w / 480; }
static int sy(int v) { return v * s_h / 320; }

/* ------------------------------------------------------------- widgets */

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, uint32_t neon,
                        lv_event_cb_t cb, intptr_t code)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, sx(x), sy(y));
    lv_obj_set_size(b, sx(w), sy(h));
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(TK_LINE_HI), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TK_SURFACE_1), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_hex(neon), lv_color_black(), 70), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(neon), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 3);

    if (cb != NULL)
    {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)code);
    }

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(neon == TK_LINE_HI ? TK_TEXT : neon), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int x, int y, int w, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, sx(x), sy(y));

    if (w > 0)
    {
        lv_obj_set_width(l, sx(w));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(l, lv_font_get_line_height(&lv_font_montserrat_12));
    }

    return l;
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    solco_pro_close();
}

static lv_obj_t *screen_panel(const char *title)
{
    lv_obj_t *p = sk_panel(s_parent, 0, 0, s_w, s_h, true);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    button(p, LV_SYMBOL_LEFT, 4, 3, 44, 34, TK_LINE_HI, back_cb, 0);
    sk_hud(p, title, TK_VIOLET, sx(56), sy(13), sx(170), LV_TEXT_ALIGN_LEFT, false);
    return p;
}

/* ============================================================ SAMPLER */

static void sampler_select(int slot);

static void wave_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int w = lv_area_get_width(&area), h = lv_area_get_height(&area);
    int mid = area.y1 + h / 2;

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = 1;

    if (!s_has_wave)
    {
        return;
    }

    int col_w = w / WAVE_COLS;

    for (int c = 0; c < WAVE_COLS; c++)
    {
        int per_mille = c * 1000 / WAVE_COLS;
        bool kept = per_mille >= s_trim_a && per_mille < s_trim_b;
        int half = s_peaks[c] * (h / 2 - 4) / 32767;
        half = half < 1 ? 1 : half;
        bar.bg_color = lv_color_hex(kept ? TK_CYAN : TK_LINE_HI);
        bar.bg_opa = kept ? LV_OPA_COVER : LV_OPA_60;
        lv_area_t a = {area.x1 + c * col_w + 1, mid - half, area.x1 + c * col_w + col_w - 1, mid + half};
        lv_draw_rect(layer, &bar, &a);
    }

    /* trim markers */
    lv_draw_rect_dsc_t mark;
    lv_draw_rect_dsc_init(&mark);
    mark.bg_opa = LV_OPA_COVER;
    int xa = area.x1 + w * s_trim_a / 1000, xb = area.x1 + w * s_trim_b / 1000 - 1;
    mark.bg_color = lv_color_hex(TK_LIME);
    lv_area_t ma = {xa, area.y1, xa + 2, area.y2};
    lv_draw_rect(layer, &mark, &ma);
    mark.bg_color = lv_color_hex(TK_MAGENTA);
    lv_area_t mb = {xb - 2, area.y1, xb, area.y2};
    lv_draw_rect(layer, &mark, &mb);
}

static void wave_touch(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int pm = (pt.x - area.x1) * 1000 / lv_area_get_width(&area);
    pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;

    /* move the nearer marker, keep at least 3 % between them */
    if (abs(pm - s_trim_a) <= abs(pm - s_trim_b))
    {
        s_trim_a = pm > s_trim_b - 30 ? s_trim_b - 30 : pm;
    }
    else
    {
        s_trim_b = pm < s_trim_a + 30 ? s_trim_a + 30 : pm;
    }

    lv_obj_invalidate(obj);
}

static void sampler_refresh_info(void)
{
    pro_slot_info_t info;
    studio_pro_slot_info(s_slot, &info);
    char text[96];

    if (info.used)
    {
        snprintf(text, sizeof(text), "SLOT %d  %s  %lu.%lu s  PITCH %+d", s_slot + 1, info.name,
                 (unsigned long)(info.frames / info.rate), (unsigned long)(info.frames * 10 / info.rate % 10),
                 info.pitch);
    }
    else
    {
        snprintf(text, sizeof(text), "SLOT %d VUOTO  //  REC per registrare", s_slot + 1);
    }

    sk_set_text(s_slot_info, text);

    for (int i = 0; i < PRO_SLOTS; i++)
    {
        pro_slot_info_t pi;
        studio_pro_slot_info(i, &pi);
        uint32_t neon = i == s_slot ? TK_LIME : pi.used ? ((i & 1) ? TK_MAGENTA : TK_CYAN) : TK_TEXT_3;
        lv_obj_set_style_border_color(s_pads[i], lv_color_hex(neon), 0);
        lv_obj_set_style_border_width(s_pads[i], i == s_slot ? 2 : 1, 0);
        lv_obj_set_style_bg_color(s_pads[i], lv_color_mix(lv_color_hex(neon), lv_color_black(), pi.used ? 40 : 10), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_pads[i], 0), lv_color_hex(pi.used ? TK_TEXT : TK_TEXT_3), 0);
    }
}

static void sampler_reload_wave(void)
{
    s_has_wave = studio_pro_slot_peaks(s_slot, s_peaks, WAVE_COLS);
    s_trim_a = 0;
    s_trim_b = 1000;
    lv_obj_invalidate(s_wave);
    sampler_refresh_info();
}

static void sampler_select(int slot)
{
    s_slot = slot;
    sampler_reload_wave();
}

enum
{
    ACT_REC,
    ACT_PLAY,
    ACT_TRIM,
    ACT_NORM,
    ACT_REV,
    ACT_FADE,
    ACT_PITCH_DOWN,
    ACT_PITCH_UP,
    ACT_WARM,
    ACT_SAVE,
    ACT_LOAD,
    ACT_CHOP,
    ACT_STOP_REC,
    ACT_CLEAR,
};

static void sampler_action(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    pro_slot_info_t info;
    studio_pro_slot_info(s_slot, &info);
    esp_err_t err = ESP_OK;

    switch (act)
    {
        case ACT_REC:
            err = studio_pro_record(s_slot);
            if (err == ESP_OK)
            {
                lv_obj_set_hidden(s_rec_overlay, false);
                lv_obj_move_foreground(s_rec_overlay);
            }
            else
            {
                ui_toast("Microfono non disponibile");
            }
            return;
        case ACT_STOP_REC:
            studio_pro_record_stop();
            return;
        case ACT_PLAY:
            studio_pro_hit(s_slot, 120);
            return;
        case ACT_TRIM:
            err = studio_pro_edit(s_slot, PRO_EDIT_TRIM, s_trim_a, s_trim_b);
            break;
        case ACT_NORM:
            err = studio_pro_edit(s_slot, PRO_EDIT_NORMALIZE, 0, 0);
            break;
        case ACT_REV:
            err = studio_pro_edit(s_slot, PRO_EDIT_REVERSE, 0, 0);
            break;
        case ACT_FADE:
            err = studio_pro_edit(s_slot, PRO_EDIT_FADE, 0, 0);
            break;
        case ACT_WARM:
            err = studio_pro_edit(s_slot, PRO_EDIT_LOWPASS, 0, 0);
            break;
        case ACT_PITCH_DOWN:
            studio_pro_set_pitch(s_slot, info.pitch - 1);
            studio_pro_hit(s_slot, 110);
            break;
        case ACT_PITCH_UP:
            studio_pro_set_pitch(s_slot, info.pitch + 1);
            studio_pro_hit(s_slot, 110);
            break;
        case ACT_CHOP:
            err = studio_pro_edit(s_slot, PRO_EDIT_CHOP, 0, 0);
            if (err == ESP_OK)
            {
                ui_toast("Diviso in 8 fette sui pad");
            }
            break;
        case ACT_SAVE:
            err = studio_pro_save(s_slot);
            ui_toast(err == ESP_OK ? "Salvato su microSD (solco/samples)" : "Salvataggio non riuscito");
            return;
        case ACT_LOAD:
            err = studio_pro_load(s_slot);
            ui_toast(err == ESP_OK ? "Campione caricato" : "Nessun slotN.wav in solco/samples");
            break;
        default:
            break;
    }

    if (err == ESP_ERR_NOT_FOUND && act != ACT_LOAD)
    {
        ui_toast("Slot vuoto: registra prima un suono");
    }

    sampler_reload_wave();
}

static void pad_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    studio_pro_hit(slot, 118);

    if (slot != s_slot)
    {
        sampler_select(slot);
    }
}

static void build_sampler(void)
{
    lv_obj_t *p = screen_panel("CAMPIONATORE");
    s_panel[SCREEN_SAMPLER] = p;
    s_slot_info = caption(p, "", 236, 15, 240, TK_TEXT_2);

    s_wave = lv_obj_create(p);
    lv_obj_remove_style_all(s_wave);
    lv_obj_set_pos(s_wave, sx(6), sy(42));
    lv_obj_set_size(s_wave, sx(468), sy(104));
    lv_obj_set_style_bg_opa(s_wave, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_wave, lv_color_hex(0x05080E), 0);
    lv_obj_set_style_border_width(s_wave, 1, 0);
    lv_obj_set_style_border_color(s_wave, lv_color_hex(TK_LINE), 0);
    lv_obj_set_style_radius(s_wave, 6, 0);
    lv_obj_add_flag(s_wave, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wave, wave_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_wave, wave_touch, LV_EVENT_PRESSING, NULL);

    static const struct
    {
        const char *text;
        int act;
        uint32_t neon;
    } row1[] = {{LV_SYMBOL_STOP " REC", ACT_REC, TK_RED},  {LV_SYMBOL_PLAY " PROVA", ACT_PLAY, TK_LIME},
                {"TAGLIA", ACT_TRIM, TK_CYAN},             {"NORMAL.", ACT_NORM, TK_CYAN},
                {"INVERTI", ACT_REV, TK_CYAN},             {"FADE", ACT_FADE, TK_CYAN}},
      row2[] = {{"PITCH -", ACT_PITCH_DOWN, TK_AMBER}, {"PITCH +", ACT_PITCH_UP, TK_AMBER},
                {"CALDO", ACT_WARM, TK_AMBER},         {"8 FETTE", ACT_CHOP, TK_MAGENTA},
                {"SALVA", ACT_SAVE, TK_VIOLET},        {"CARICA", ACT_LOAD, TK_VIOLET}};

    for (int i = 0; i < 6; i++)
    {
        button(p, row1[i].text, 6 + i * 79, 152, 74, 40, row1[i].neon, sampler_action, row1[i].act);
        button(p, row2[i].text, 6 + i * 79, 198, 74, 40, row2[i].neon, sampler_action, row2[i].act);
    }

    for (int i = 0; i < PRO_SLOTS; i++)
    {
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        s_pads[i] = button(p, num, 6 + i * 59, 246, 54, 68, TK_LINE_HI, NULL, 0);
        lv_obj_add_event_cb(s_pads[i], pad_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_set_style_text_font(lv_obj_get_child(s_pads[i], 0), &lv_font_montserrat_20, 0);
    }

    /* recording overlay */
    s_rec_overlay = sk_panel(p, 0, 0, s_w, s_h, true);
    lv_obj_add_flag(s_rec_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *dot = lv_obj_create(s_rec_overlay);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, sx(64), sx(64));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(TK_RED), 0);
    lv_obj_set_style_shadow_width(dot, 30, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(TK_RED), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, sy(40));
    s_rec_label = lv_label_create(s_rec_overlay);
    lv_obj_set_style_text_font(s_rec_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_rec_label, lv_color_hex(TK_TEXT), 0);
    lv_obj_align(s_rec_label, LV_ALIGN_TOP_MID, 0, sy(120));
    s_rec_level = lv_bar_create(s_rec_overlay);
    lv_obj_set_size(s_rec_level, sx(300), sy(12));
    lv_obj_align(s_rec_level, LV_ALIGN_TOP_MID, 0, sy(162));
    lv_bar_set_range(s_rec_level, 0, 100);
    lv_obj_set_style_bg_color(s_rec_level, lv_color_hex(TK_SURFACE_2), 0);
    lv_obj_set_style_bg_color(s_rec_level, lv_color_hex(TK_RED), LV_PART_INDICATOR);
    button(s_rec_overlay, LV_SYMBOL_STOP " STOP", 170, 200, 140, 52, TK_RED, sampler_action, ACT_STOP_REC);
    lv_obj_t *hint = caption(s_rec_overlay, "Avvicina il suono al microfono. Max 10 s, il silenzio iniziale si toglie da solo.",
                             40, 268, 400, TK_TEXT_2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(hint, LV_SIZE_CONTENT);
    lv_obj_set_hidden(s_rec_overlay, true);

    sampler_select(0);
}

static void sampler_tick(void)
{
    int elapsed = 0, level = 0;
    pro_capture_state_t st = studio_pro_capture_state(&elapsed, &level);

    if (st == PRO_CAPTURE_WAITING || st == PRO_CAPTURE_RECORDING)
    {
        char text[32];
        snprintf(text, sizeof(text), "REGISTRO  %d.%d s", elapsed / 1000, elapsed / 100 % 10);
        sk_set_text(s_rec_label, st == PRO_CAPTURE_WAITING ? "PREPARO..." : text);
        lv_bar_set_value(s_rec_level, level, LV_ANIM_OFF);
    }
    else if (st != s_cap_seen)
    {
        lv_obj_set_hidden(s_rec_overlay, true);

        if (st == PRO_CAPTURE_DONE)
        {
            sampler_reload_wave();
            studio_pro_hit(s_slot, 120);   /* hear it at once */
        }
        else if (st == PRO_CAPTURE_FAILED)
        {
            ui_toast("Nessun suono registrato");
        }
    }

    s_cap_seen = st;
}

/* ============================================================== SYNTH */

static const struct
{
    const char *name;
    size_t offset;
} s_params[11] = {
    {"FILTRO", offsetof(pro_synth_t, cutoff)},    {"RISON.", offsetof(pro_synth_t, resonance)},
    {"INV.FILT", offsetof(pro_synth_t, env_amount)}, {"ATTACCO", offsetof(pro_synth_t, attack)},
    {"DECAD.", offsetof(pro_synth_t, decay)},     {"SUSTAIN", offsetof(pro_synth_t, sustain)},
    {"RILASCIO", offsetof(pro_synth_t, release)}, {"DETUNE", offsetof(pro_synth_t, detune)},
    {"DRIVE", offsetof(pro_synth_t, drive)},      {"ECO", offsetof(pro_synth_t, echo)},
    {"VOLUME", offsetof(pro_synth_t, volume)},
};
static const char *const s_wave_names[PRO_WAVE_COUNT] = {"SEGA", "QUADRA", "TRIANG.", "SINUS.", "RUMORE"};

static void synth_sync_ui(void)
{
    pro_synth_t p;
    studio_pro_synth_get(&p);
    const uint8_t *raw = (const uint8_t *)&p;

    for (int i = 0; i < 11; i++)
    {
        lv_slider_set_value(s_sliders[i], raw[s_params[i].offset], LV_ANIM_OFF);
    }

    char text[24];
    snprintf(text, sizeof(text), "%s >", studio_pro_preset_name(s_preset));
    sk_set_text(s_preset_label, text);
    sk_set_text(s_wave_label, s_wave_names[p.wave % PRO_WAVE_COUNT]);
    snprintf(text, sizeof(text), "DO%d", s_octave);
    sk_set_text(s_octave_label, text);
}

static void slider_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    pro_synth_t p;
    studio_pro_synth_get(&p);
    ((uint8_t *)&p)[s_params[index].offset] = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    studio_pro_synth_set(&p);
}

enum
{
    SYN_PRESET,
    SYN_WAVE,
    SYN_OCT_DOWN,
    SYN_OCT_UP,
};

static void synth_action(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);

    if (act == SYN_PRESET)
    {
        s_preset = (s_preset + 1) % PRO_PRESETS;
        studio_pro_preset(s_preset);
    }
    else if (act == SYN_WAVE)
    {
        pro_synth_t p;
        studio_pro_synth_get(&p);
        p.wave = (uint8_t)((p.wave + 1) % PRO_WAVE_COUNT);
        studio_pro_synth_set(&p);
    }
    else if (act == SYN_OCT_DOWN && s_octave > 1)
    {
        s_octave--;
    }
    else if (act == SYN_OCT_UP && s_octave < 6)
    {
        s_octave++;
    }

    synth_sync_ui();
}

static void key_cb(lv_event_t *e)
{
    int semitone = (int)(intptr_t)lv_event_get_user_data(e);
    int note = 12 * (s_octave + 1) + semitone;
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED)
    {
        if (s_key_down >= 0)
        {
            studio_pro_note_off(s_key_down);
        }

        s_key_down = note;
        studio_pro_note_on(note, 110);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        studio_pro_note_off(note);

        if (s_key_down == note)
        {
            s_key_down = -1;
        }
    }
}

static lv_obj_t *key(lv_obj_t *parent, int x, int y, int w, int h, bool black, int semitone)
{
    lv_obj_t *k = lv_obj_create(parent);
    lv_obj_remove_style_all(k);
    lv_obj_set_pos(k, x, y);
    lv_obj_set_size(k, w, h);
    lv_obj_set_style_radius(k, 4, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(black ? 0x0B0F18 : 0xD9E4F2), 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(black ? TK_MAGENTA : TK_CYAN), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(k, 1, 0);
    lv_obj_set_style_border_color(k, lv_color_hex(black ? TK_LINE_HI : 0x7D8AA0), 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_PRESSED, (void *)(intptr_t)semitone);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_RELEASED, (void *)(intptr_t)semitone);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)semitone);
    return k;
}

static void build_synth(void)
{
    lv_obj_t *p = screen_panel("SYNTH //");
    s_panel[SCREEN_SYNTH] = p;
    lv_obj_t *preset = button(p, "BASSO >", 150, 3, 124, 34, TK_MAGENTA, synth_action, SYN_PRESET);
    s_preset_label = lv_obj_get_child(preset, 0);
    lv_obj_t *wave = button(p, "SEGA", 280, 3, 80, 34, TK_CYAN, synth_action, SYN_WAVE);
    s_wave_label = lv_obj_get_child(wave, 0);
    button(p, "-", 366, 3, 34, 34, TK_LINE_HI, synth_action, SYN_OCT_DOWN);
    s_octave_label = caption(p, "DO4", 404, 14, 38, TK_AMBER);
    button(p, "+", 442, 3, 34, 34, TK_LINE_HI, synth_action, SYN_OCT_UP);

    /* 11 parameters on two rows */
    for (int i = 0; i < 11; i++)
    {
        int col = i % 6, row = i / 6;
        int x = 6 + col * 79, y = 44 + row * 58;
        caption(p, s_params[i].name, x + 2, y, 74, TK_TEXT_2);
        lv_obj_t *s = lv_slider_create(p);
        lv_obj_set_pos(s, sx(x + 4), sy(y + 24));
        lv_obj_set_size(s, sx(66), sy(10));
        lv_slider_set_range(s, 0, 100);
        lv_obj_set_style_bg_color(s, lv_color_hex(TK_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s, lv_color_hex(row ? TK_MAGENTA : TK_CYAN), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(s, 4, LV_PART_KNOB);
        lv_obj_set_ext_click_area(s, sy(12));
        lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_sliders[i] = s;
    }

    caption(p, "TASTIERA", 6 + 5 * 79 + 2, 102, 74, TK_TEXT_3);

    /* keyboard: 14 white keys, black keys on top */
    int top = sy(164), height = s_h - top - sy(4);
    int white_w = s_w / 14;
    static const int whites[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int blacks[5] = {1, 3, 6, 8, 10};
    static const int black_after[5] = {0, 1, 3, 4, 5};   /* white key index the black key follows */

    for (int o = 0; o < 2; o++)
    {
        for (int i = 0; i < 7; i++)
        {
            key(p, (o * 7 + i) * white_w + 1, top, white_w - 2, height, false, o * 12 + whites[i]);
        }
    }

    for (int o = 0; o < 2; o++)
    {
        for (int i = 0; i < 5; i++)
        {
            int x = (o * 7 + black_after[i] + 1) * white_w - white_w * 3 / 10;
            key(p, x, top, white_w * 6 / 10, height * 58 / 100, true, o * 12 + blacks[i]);
        }
    }

    synth_sync_ui();
}

/* ========================================================== SEQUENCER */

static const char *const s_drum_names[8] = {"KICK", "SNARE", "CLAP", "HAT", "OPEN", "TOM", "SHAKE", "RIM"};

static bool cell_on(int row, int step, const studio_status_t *st)
{
    if (s_tab == 0)
    {
        return (st->pattern.pattern[row] >> step) & 1;
    }

    if (s_tab == 1)
    {
        return (studio_pro_track(row) >> step) & 1;
    }

    return studio_pro_melody(step) == 7 - row;   /* top row = highest note */
}

static void seq_paint(void)
{
    studio_status_t st;
    solco_studio_status(&st);
    static const uint32_t tab_neon[3] = {TK_CYAN, TK_MAGENTA, TK_AMBER};
    uint32_t neon = tab_neon[s_tab];

    for (int r = 0; r < 8; r++)
    {
        for (int c = 0; c < PRO_STEPS; c++)
        {
            bool on = cell_on(r, c, &st);
            bool beat = (c / 4) % 2 == 0;
            lv_obj_set_style_bg_color(s_cells[r][c], on ? lv_color_hex(neon)
                                                        : lv_color_hex(beat ? 0x101828 : 0x0A101A), 0);
        }
    }

    for (int t = 0; t < 3; t++)
    {
        lv_obj_set_style_border_color(s_tabs[t], lv_color_hex(t == s_tab ? tab_neon[t] : TK_LINE_HI), 0);
        lv_obj_set_style_border_width(s_tabs[t], t == s_tab ? 2 : 1, 0);
    }

    for (int r = 0; r < 8; r++)
    {
        char name[16];

        if (s_tab == 0)
        {
            snprintf(name, sizeof(name), "%s", s_drum_names[r]);
        }
        else if (s_tab == 1)
        {
            pro_slot_info_t info;
            studio_pro_slot_info(r, &info);
            snprintf(name, sizeof(name), info.used ? "SMP %d" : "- %d -", r + 1);
        }
        else
        {
            static const char *const names[12] = {"DO", "DO#", "RE", "RE#", "MI", "FA", "FA#", "SOL", "SOL#", "LA", "LA#", "SI"};
            int note = studio_pro_row_note(7 - r);
            snprintf(name, sizeof(name), "%s%d", names[note % 12], note / 12 - 1);
        }

        sk_set_text(s_row_names[r], name);
    }

    char text[24];
    snprintf(text, sizeof(text), "SWING %d%%", studio_pro_swing());
    sk_set_text(s_swing_label, text);
    s_seq_revision = st.revision;
}

static void cell_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int row = code / PRO_STEPS, step = code % PRO_STEPS;

    if (s_tab == 0)
    {
        solco_studio_command(STUDIO_STEP, row, step + 1);
        solco_studio_command(STUDIO_HIT, row, 100);   /* hear the sound you placed */
    }
    else if (s_tab == 1)
    {
        studio_pro_toggle(row, step);

        if ((studio_pro_track(row) >> step) & 1)
        {
            studio_pro_hit(row, 100);
        }
    }
    else
    {
        int note_row = 7 - row;
        bool same = studio_pro_melody(step) == note_row;
        studio_pro_set_melody(step, same ? -1 : note_row);

        if (!same)
        {
            int note = studio_pro_row_note(note_row);
            studio_pro_note_on(note, 100);
            studio_pro_note_off(note);
        }
    }

    seq_paint();
}

enum
{
    SEQ_PLAY,
    SEQ_BPM_DOWN,
    SEQ_BPM_UP,
    SEQ_SWING,
    SEQ_CLEAR,
    SEQ_TAB0,
};

static void seq_action(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    studio_status_t st;
    solco_studio_status(&st);

    if (act == SEQ_PLAY)
    {
        solco_studio_command(STUDIO_PLAY, 0, 0);
    }
    else if (act == SEQ_BPM_DOWN)
    {
        solco_studio_command(STUDIO_TEMPO, st.bpm - 2, 0);
    }
    else if (act == SEQ_BPM_UP)
    {
        solco_studio_command(STUDIO_TEMPO, st.bpm + 2, 0);
    }
    else if (act == SEQ_SWING)
    {
        int next = studio_pro_swing() + 15;
        studio_pro_set_swing(next > 60 ? 0 : next);
    }
    else if (act == SEQ_CLEAR)
    {
        if (s_tab == 0)
        {
            solco_studio_command(STUDIO_CLEAR, 0, 0);
        }
        else
        {
            for (int r = 0; r < 8 && s_tab == 1; r++)
            {
                for (int c = 0; c < PRO_STEPS; c++)
                {
                    if ((studio_pro_track(r) >> c) & 1)
                    {
                        studio_pro_toggle(r, c);
                    }
                }
            }

            for (int c = 0; c < PRO_STEPS && s_tab == 2; c++)
            {
                studio_pro_set_melody(c, -1);
            }
        }
    }
    else if (act >= SEQ_TAB0 && act < SEQ_TAB0 + 3)
    {
        s_tab = act - SEQ_TAB0;
    }

    seq_paint();
}

static void build_seq(void)
{
    lv_obj_t *p = screen_panel("SEQUENCER //");
    s_panel[SCREEN_SEQ] = p;
    lv_obj_t *play = button(p, LV_SYMBOL_PLAY, 250, 3, 60, 34, TK_LIME, seq_action, SEQ_PLAY);
    s_play_label = lv_obj_get_child(play, 0);
    button(p, "-", 320, 3, 38, 34, TK_LINE_HI, seq_action, SEQ_BPM_DOWN);
    s_bpm_label = caption(p, "120 BPM", 362, 14, 72, TK_AMBER);
    button(p, "+", 438, 3, 38, 34, TK_LINE_HI, seq_action, SEQ_BPM_UP);

    static const char *const tabs[3] = {"BATTERIA", "CAMPIONI", "MELODIA"};

    for (int t = 0; t < 3; t++)
    {
        s_tabs[t] = button(p, tabs[t], 6 + t * 96, 41, 92, 30, t == 0 ? TK_CYAN : t == 1 ? TK_MAGENTA : TK_AMBER,
                           seq_action, SEQ_TAB0 + t);
    }

    lv_obj_t *swing = button(p, "SWING 0%", 300, 41, 96, 30, TK_VIOLET, seq_action, SEQ_SWING);
    s_swing_label = lv_obj_get_child(swing, 0);
    button(p, "CANCELLA", 400, 41, 76, 30, TK_RED, seq_action, SEQ_CLEAR);

    int top = 76, row_h = 30, left = 62, cell_w = (474 - left) / PRO_STEPS;

    for (int r = 0; r < 8; r++)
    {
        s_row_names[r] = caption(p, "", 4, top + r * row_h + 9, 56, TK_TEXT_2);

        for (int c = 0; c < PRO_STEPS; c++)
        {
            lv_obj_t *cell = lv_obj_create(p);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, sx(left + c * cell_w + 1), sy(top + r * row_h + 2));
            lv_obj_set_size(cell, sx(cell_w - 2), sy(row_h - 4));
            lv_obj_set_style_radius(cell, 3, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, lv_color_white(), 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(r * PRO_STEPS + c));
            s_cells[r][c] = cell;
        }
    }

    seq_paint();
}

static void seq_tick(void)
{
    studio_status_t st;
    solco_studio_status(&st);
    int step = st.playing ? st.step : -1;

    if (step != s_seq_step)
    {
        for (int r = 0; r < 8; r++)
        {
            if (s_seq_step >= 0 && s_seq_step < PRO_STEPS)
            {
                lv_obj_set_style_border_width(s_cells[r][s_seq_step], 0, 0);
            }

            if (step >= 0 && step < PRO_STEPS)
            {
                lv_obj_set_style_border_width(s_cells[r][step], 2, 0);
            }
        }

        s_seq_step = step;
    }

    char text[16];
    snprintf(text, sizeof(text), "%d BPM", st.bpm);
    sk_set_text(s_bpm_label, text);
    sk_set_text(s_play_label, st.playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);

    if (s_tab == 0 && st.revision != s_seq_revision)
    {
        seq_paint();   /* the beat changed elsewhere (drumpad REC) */
    }
}

/* ============================================================ public */

void solco_pro_open(lv_obj_t *parent, int screen)
{
    if (screen < 0 || screen > SCREEN_SEQ)
    {
        return;
    }

    if (s_parent != parent)
    {
        s_parent = parent;
        s_w = lv_obj_get_width(parent);
        s_h = lv_obj_get_height(parent);
    }

    if (s_panel[screen] == NULL)
    {
        if (screen == SCREEN_SAMPLER)
            build_sampler();
        else if (screen == SCREEN_SYNTH)
            build_synth();
        else
            build_seq();
    }

    for (int i = 0; i < 3; i++)
    {
        if (s_panel[i] != NULL)
        {
            lv_obj_set_hidden(s_panel[i], i != screen);
        }
    }

    lv_obj_move_foreground(s_panel[screen]);
    s_screen = screen;
    s_seq_step = -2;

    if (screen == SCREEN_SAMPLER)
        sampler_reload_wave();
    else if (screen == SCREEN_SEQ)
        seq_paint();
    else
        synth_sync_ui();
}

void solco_pro_close(void)
{
    if (s_screen >= 0 && s_panel[s_screen] != NULL)
    {
        lv_obj_set_hidden(s_panel[s_screen], true);
    }

    if (s_key_down >= 0)
    {
        studio_pro_note_off(s_key_down);
        s_key_down = -1;
    }

    s_screen = -1;
    studio_pro_save_project();   /* pattern and synth survive a restart */
}

bool solco_pro_visible(void)
{
    return s_screen >= 0;
}

void solco_pro_refresh(void)
{
    if (s_screen == SCREEN_SAMPLER)
        sampler_tick();
    else if (s_screen == SCREEN_SEQ)
        seq_tick();
}
