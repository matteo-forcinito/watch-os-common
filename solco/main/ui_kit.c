/* NEON NOIR building blocks for Solco: few objects, small invalidations. */
#include "ui_kit.h"
#include "esp_heap_caps.h"
#include "lvgl_private.h" /* lv_image_cache_drop */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_swap = false;
static lv_color_format_t s_cf = LV_COLOR_FORMAT_RGB565;

void sk_init(void)
{
    /* LVGL renders in RGB565 and swaps bytes only at flush: plain RGB565 images
     * blit directly from their buffer (a swapped source would be converted, whole
     * image, every time it changes). */
    s_swap = false;
    s_cf = LV_COLOR_FORMAT_RGB565;
}

uint16_t sk_px(uint32_t rgb)
{
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F));
    return s_swap ? (uint16_t)((v >> 8) | (v << 8)) : v;
}

sk_tone_t sk_tone(uint32_t neon)
{
    switch (neon)
    {
    case TK_MAGENTA: return (sk_tone_t){TK_MAGENTA, TK_MAGENTA_G1, TK_MAGENTA_G2, TK_MAGENTA_G3};
    case TK_AMBER: return (sk_tone_t){TK_AMBER, TK_AMBER_G1, TK_AMBER_G2, TK_AMBER_G3};
    case TK_LIME: return (sk_tone_t){TK_LIME, TK_LIME_G1, TK_LIME_G2, TK_LIME_G3};
    case TK_VIOLET: return (sk_tone_t){TK_VIOLET, TK_VIOLET_G1, TK_VIOLET_G2, TK_VIOLET_G3};
    case TK_RED: return (sk_tone_t){TK_RED, TK_RED_G1, TK_RED_G2, TK_RED_G3};
    case TK_TEXT: return (sk_tone_t){TK_TEXT, TK_LINE_HI, TK_SURFACE_2, TK_SURFACE_1};
    default: return (sk_tone_t){TK_CYAN, TK_CYAN_G1, TK_CYAN_G2, TK_CYAN_G3};
    }
}

/* ------------------------------------------------------------------ basics */

lv_obj_t *sk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w,
                   lv_text_align_t align)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, "");
    lv_obj_set_style_text_font(obj, font ? font : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, align, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    return obj;
}

lv_obj_t *sk_hud(lv_obj_t *parent, const char *text, uint32_t color, int x, int y, int w,
                 lv_text_align_t align, bool micro)
{
    lv_obj_t *obj = sk_label(parent, micro ? &lv_font_montserrat_12 : &lv_font_montserrat_14, color,
                             x, y, w, align);
    lv_obj_set_style_text_letter_space(obj, micro ? 2 : 3, 0);
    lv_label_set_text(obj, text);
    return obj;
}

lv_obj_t *sk_panel(lv_obj_t *parent, int x, int y, int w, int h, bool opaque)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_scrollable(obj, false);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    if (opaque)
    {
        lv_obj_set_style_bg_color(obj, lv_color_hex(TK_VOID), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    return obj;
}

void sk_set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text))
        lv_label_set_text(label, text);
}

void sk_set_color(lv_obj_t *obj, uint32_t color)
{
    lv_color_t value = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), value))
        lv_obj_set_style_text_color(obj, value, 0);
}

static uint32_t now_ms(void) { return lv_tick_get(); }

static void touch_point(lv_obj_t *obj, lv_point_t *point, lv_area_t *area)
{
    lv_indev_t *input = lv_indev_active();
    point->x = point->y = 0;
    if (input)
        lv_indev_get_point(input, point);
    lv_obj_get_coords(obj, area);
}

/* ------------------------------------------------------------------ strip */

static void cell_area(const sk_strip_t *s, const lv_area_t *a, int i, lv_area_t *out)
{
    int total = 0, before = 0;
    for (int j = 0; j < s->n; ++j)
    {
        total += s->weight[j];
        if (j < i)
            before += s->weight[j];
    }
    int width = lv_area_get_width(a) - SK_CELL_GAP * (s->n - 1);
    out->x1 = a->x1 + width * before / total + SK_CELL_GAP * i;
    out->x2 = a->x1 + width * (before + s->weight[i]) / total + SK_CELL_GAP * i - 1;
    out->y1 = a->y1;
    out->y2 = a->y2;
}

static void strip_invalidate(sk_strip_t *s, int cell)
{
    lv_area_t a, c;
    lv_obj_get_coords(s->obj, &a);
    cell_area(s, &a, cell, &c);
    lv_obj_invalidate_area(s->obj, &c);
}

static void strip_draw(sk_strip_t *s, lv_layer_t *layer)
{
    lv_area_t a;
    lv_obj_get_coords(s->obj, &a);
    for (int i = 0; i < s->n; ++i)
    {
        lv_area_t c;
        cell_area(s, &a, i, &c);
        sk_tone_t t = sk_tone(s->neon[i]);
        int state = s->state[i];
        uint32_t bg = 0, border = TK_LINE_HI, text = TK_TEXT;
        int border_w = TK_LW_HAIR;
        bool fill = false;
        switch (state)
        {
        case SK_ON: fill = true; bg = t.g2; border = t.neon; text = t.neon; border_w = TK_LW; break;
        case SK_ARMED: border = t.neon; text = t.neon; break;
        case SK_OFF: border = TK_LINE; text = TK_TEXT_3; break;
        case SK_ERR: fill = true; bg = TK_RED_G2; border = TK_RED; text = TK_RED; border_w = TK_LW; break;
        default: break;
        }
        if (s->pressed == i && state != SK_OFF)
        {
            fill = true;
            bg = state == SK_IDLE ? TK_SURFACE_2 : t.g1;
            border = t.neon;
        }
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = TK_R_XS;
        r.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
        r.bg_color = lv_color_hex(bg);
        r.border_width = border_w;
        r.border_color = lv_color_hex(border);
        r.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &r, &c);
        if (s->progress[i])
        {
            lv_area_t p = c;
            p.x1 += TK_LW;
            p.y1 += TK_LW;
            p.y2 -= TK_LW;
            p.x2 = p.x1 + (lv_area_get_width(&c) - 2 * TK_LW) * s->progress[i] / 255;
            lv_draw_rect_dsc_t f;
            lv_draw_rect_dsc_init(&f);
            f.bg_color = lv_color_hex(t.g1);
            f.radius = 0;
            lv_draw_rect(layer, &f, &p);
        }
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.text = s->text[i];
        l.font = s->font;
        l.color = lv_color_hex(text);
        l.align = LV_TEXT_ALIGN_CENTER;
        l.letter_space = 1;
        lv_area_t ta = c;
        int lh = lv_font_get_line_height(s->font);
        ta.y1 = (c.y1 + c.y2 + 1 - lh) / 2;
        ta.y2 = ta.y1 + lh - 1;
        lv_draw_label(layer, &l, &ta);
    }
}

static void strip_event(lv_event_t *e)
{
    sk_strip_t *s = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN)
    {
        strip_draw(s, lv_event_get_layer(e));
        return;
    }
    if (code == LV_EVENT_PRESSED)
    {
        lv_point_t p;
        lv_area_t a;
        touch_point(s->obj, &p, &a);
        for (int i = 0; i < s->n; ++i)
        {
            lv_area_t c;
            cell_area(s, &a, i, &c);
            if (i == s->n - 1)
                c.x2 = a.x2;
            if (p.x <= c.x2 + SK_CELL_GAP / 2 || i == s->n - 1)
            {
                if (s->state[i] == SK_OFF)
                    return;
                s->pressed = (int8_t)i;
                strip_invalidate(s, i);
                if (s->cb)
                    s->cb(s, i, true);
                return;
            }
        }
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s->pressed >= 0)
    {
        int cell = s->pressed;
        s->pressed = -1;
        strip_invalidate(s, cell);
        if (s->cb)
            s->cb(s, cell, false);
    }
}

sk_strip_t *sk_strip_create(lv_obj_t *parent, int x, int y, int w, int h, int n,
                            const uint8_t *weights, sk_strip_cb_t cb, int tag)
{
    sk_strip_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->n = n > SK_STRIP_MAX ? SK_STRIP_MAX : n;
    s->cb = cb;
    s->tag = tag;
    s->pressed = -1;
    s->font = &lv_font_montserrat_16;
    for (int i = 0; i < s->n; ++i)
    {
        s->weight[i] = weights ? weights[i] : 1;
        s->neon[i] = TK_CYAN;
    }
    s->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s->obj);
    lv_obj_set_pos(s->obj, x, y);
    lv_obj_set_size(s->obj, w, h);
    lv_obj_set_scrollable(s->obj, false);
    lv_obj_add_flag(s->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(s->obj, TK_SP_1);
    lv_obj_add_event_cb(s->obj, strip_event, LV_EVENT_ALL, s);
    return s;
}

void sk_strip_set(sk_strip_t *s, int cell, const char *text, uint32_t neon, int state)
{
    if (!s || cell < 0 || cell >= s->n)
        return;
    if ((int32_t)(s->flash_until[cell] - now_ms()) > 0)
        state = SK_ERR;
    bool changed = s->neon[cell] != neon || s->state[cell] != state;
    if (text && strncmp(s->text[cell], text, sizeof(s->text[cell]) - 1))
    {
        snprintf(s->text[cell], sizeof(s->text[cell]), "%s", text);
        changed = true;
    }
    s->neon[cell] = neon;
    s->state[cell] = (uint8_t)state;
    if (changed)
        strip_invalidate(s, cell);
}

void sk_strip_progress(sk_strip_t *s, int cell, int progress)
{
    progress = progress < 0 ? 0 : progress > 255 ? 255 : progress;
    if (s && s->progress[cell] != progress)
    {
        s->progress[cell] = (uint8_t)progress;
        strip_invalidate(s, cell);
    }
}

void sk_strip_layout(sk_strip_t *s, int n, const uint8_t *weights)
{
    n = n > SK_STRIP_MAX ? SK_STRIP_MAX : n;
    bool changed = n != s->n;
    for (int i = 0; i < n; ++i)
    {
        changed |= s->weight[i] != (weights ? weights[i] : 1);
        s->weight[i] = weights ? weights[i] : 1;
    }
    if (!changed)
        return;
    s->n = n;
    s->pressed = -1;
    lv_obj_invalidate(s->obj);
}

void sk_strip_flash(sk_strip_t *s, int cell)
{
    s->flash_until[cell] = now_ms() + SK_FLASH_MS;
    s->state[cell] = SK_ERR;
    strip_invalidate(s, cell);
}

void sk_strip_tick(sk_strip_t *s)
{
    for (int i = 0; s && i < s->n; ++i)
    {
        if (s->state[i] == SK_ERR && s->flash_until[i] && (int32_t)(s->flash_until[i] - now_ms()) <= 0)
        {
            s->flash_until[i] = 0;
            s->state[i] = SK_IDLE;
            strip_invalidate(s, i);
        }
    }
}

/* ------------------------------------------------------------------ waveform */

static int s_flip_budget = 1;
void sk_wave_tick(void) { s_flip_budget = 1; }

sk_wave_t *sk_wave_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t neon, bool paged,
                          uint32_t frames_per_col)
{
    sk_wave_t *wave = heap_caps_calloc(1, sizeof(*wave), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wave)
        wave = calloc(1, sizeof(*wave));
    if (!wave)
        return NULL;
    wave->w = w;
    wave->h = h;
    wave->paged = paged;
    wave->fpc = frames_per_col < SK_WAVE_BIN ? SK_WAVE_BIN : frames_per_col / SK_WAVE_BIN * SK_WAVE_BIN;
    wave->tone = sk_tone(neon);
    wave->norm = 8192;
    wave->bnorm = 64;
    wave->px = heap_caps_calloc((size_t)w * h, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    wave->img = lv_image_create(parent);
    lv_obj_set_pos(wave->img, x, y);
    lv_obj_set_size(wave->img, w, h);
    if (wave->px)
    {
        wave->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        wave->dsc.header.cf = s_cf;
        wave->dsc.header.w = w;
        wave->dsc.header.h = h;
        wave->dsc.header.stride = w * 2;
        wave->dsc.data_size = (uint32_t)w * h * 2;
        wave->dsc.data = (const uint8_t *)wave->px;
        lv_image_set_src(wave->img, &wave->dsc);
    }
    wave->dirty = true;
    return wave;
}

void sk_wave_reset(sk_wave_t *wave)
{
    if (!wave)
        return;
    memset(wave->tag, 0, sizeof(wave->tag));
    wave->norm = 8192;
    wave->bnorm = 64;
    wave->dirty = true;
}

void sk_wave_feed(sk_wave_t *wave, const uint16_t peaks[64], const uint8_t bands[64][3], uint32_t frame)
{
    uint32_t base = frame / SK_WAVE_BIN;
    for (uint32_t j = 0; j < 64; ++j)
    {
        if (!peaks[j])
            continue;
        uint32_t bin = base + j, slot = bin % SK_WAVE_HIST;
        bool same = wave->tag[slot] == bin + 1 && wave->hist[slot] == peaks[j] &&
                    (!bands || !memcmp(wave->band[slot], bands[j], 3));
        if (same)
            continue;
        wave->tag[slot] = bin + 1;
        wave->hist[slot] = peaks[j];
        if (bands)
            memcpy(wave->band[slot], bands[j], 3);
        else
            memset(wave->band[slot], 0, 3);
        if (!wave->data || bin < wave->data_lo)
            wave->data_lo = bin;
        if (!wave->data || bin > wave->data_hi)
            wave->data_hi = bin;
        wave->data = true;
    }
}

/* Drawing is clipped to the column span being refreshed. */
static int s_clip0, s_clip1;

static void fill(sk_wave_t *wave, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < s_clip0)
        x0 = s_clip0;
    if (x1 > s_clip1)
        x1 = s_clip1;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= wave->h)
        y1 = wave->h - 1;
    for (int y = y0; y <= y1; ++y)
    {
        uint16_t *row = wave->px + y * wave->w;
        for (int x = x0; x <= x1; ++x)
            row[x] = c;
    }
}

/* Peak and bands of one column (max of its engine bins); false if unknown. */
static bool column(const sk_wave_t *wave, int64_t col, uint16_t *peak, uint8_t band[3])
{
    if (col < 0)
        return false;
    uint32_t first = (uint32_t)(col * (wave->fpc / SK_WAVE_BIN));
    bool known = false;
    *peak = 0;
    band[0] = band[1] = band[2] = 0;
    for (uint32_t b = first; b < first + wave->fpc / SK_WAVE_BIN; ++b)
    {
        uint32_t slot = b % SK_WAVE_HIST;
        if (wave->tag[slot] != b + 1)
            continue;
        known = true;
        if (wave->hist[slot] > *peak)
            *peak = wave->hist[slot];
        for (int k = 0; k < 3; ++k)
            if (wave->band[slot][k] > band[k])
                band[k] = wave->band[slot][k];
    }
    return known;
}

static int64_t frame_px(const sk_wave_t *wave, uint32_t frame) { return frame / (wave->fpc / SK_WAVE_COL); }

/* Redraw screen columns x0..x1 (inclusive) of the current view. */
static void draw_span(sk_wave_t *wave, int x0, int x1, const sk_wave_state_t *st, int play_x)
{
    int w = wave->w, h = wave->h, mid = h / 2;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 >= w ? w - 1 : x1;
    if (x0 > x1)
        return;
    s_clip0 = x0;
    s_clip1 = x1;
    fill(wave, x0, 0, x1, h - 1, 0); /* TK_VOID is 0 in both byte orders */
    sk_tone_t t = wave->tone;
    if (st->mode == SK_WAVE_LOADING || st->mode == SK_WAVE_EMPTY)
    {
        bool empty = st->mode == SK_WAVE_EMPTY;
        uint16_t dim = sk_px(empty ? TK_TEXT_3 : TK_LINE);
        for (int x = 0; x < w; x += empty ? 8 : 1)
            fill(wave, x, mid, x + (empty ? 3 : 0), mid, dim);
        if (!empty)
        {
            int edge = w * (st->percent < 0 ? 0 : st->percent > 100 ? 100 : st->percent) / 100;
            fill(wave, 0, mid - 1, edge - 1, mid + 1, sk_px(t.g1));
            fill(wave, edge - 3, mid - 4, edge, mid + 4, sk_px(t.neon));
        }
        return;
    }
    bool has_loop = st->loop_out > st->loop_in;
    fill(wave, x0, mid, x1, mid, sk_px(TK_LINE));
    if (has_loop)
    {
        int lx0 = (int)(frame_px(wave, st->loop_in) - wave->origin);
        int lx1 = (int)(frame_px(wave, st->loop_out) - wave->origin);
        if (lx1 >= x0 && lx0 <= x1)
            fill(wave, lx0, 0, lx1, h - 1, sk_px(st->loop_on ? TK_LIME_G3 : TK_SURFACE_1));
    }
    uint16_t c_low = sk_px(t.g1), c_mid = sk_px(t.neon), c_high = sk_px(TK_TEXT);
    uint16_t p_low = sk_px(t.g2), p_mid = sk_px(t.g1), p_high = sk_px(TK_TEXT_3);
    int64_t first = (wave->origin + x0) / SK_WAVE_COL, last = (wave->origin + x1) / SK_WAVE_COL;
    for (int64_t col = first; col <= last; ++col)
    {
        uint16_t peak;
        uint8_t band[3];
        if (!column(wave, col, &peak, band))
            continue;
        int x = (int)(col * SK_WAVE_COL - wave->origin);
        bool past = x + SK_WAVE_COL <= play_x;
        int a = (int)(peak * (uint32_t)(mid - 1) / wave->norm);
        a = a < 1 ? 1 : a > mid - 1 ? mid - 1 : a;
        if (band[0] | band[1] | band[2])
        {
            /* 3 bands stacked over each other: body, core, tips. */
            int hl = band[0] * (mid - 1) / (int)wave->bnorm, hm = band[1] * (mid - 1) / (int)wave->bnorm;
            int hh = band[2] * (mid - 1) / (int)wave->bnorm;
            hl = hl > mid - 1 ? mid - 1 : hl;
            hm = hm > mid - 1 ? mid - 1 : hm;
            hh = hh > mid - 1 ? mid - 1 : hh;
            fill(wave, x, mid - hl, x + SK_WAVE_COL - 2, mid + hl, past ? p_low : c_low);
            fill(wave, x, mid - hm, x + SK_WAVE_COL - 2, mid + hm, past ? p_mid : c_mid);
            if (hh)
                fill(wave, x, mid - hh, x + SK_WAVE_COL - 2, mid + hh, past ? p_high : c_high);
        }
        else
        {
            int c = a * 5 / 8;
            fill(wave, x, mid - a, x + SK_WAVE_COL - 2, mid + a, past ? p_low : c_low);
            fill(wave, x, mid - c, x + SK_WAVE_COL - 2, mid + c, past ? p_mid : c_mid);
        }
    }
    if (has_loop || st->marked)
    {
        uint16_t edge = sk_px(st->loop_on ? TK_LIME : TK_LIME_G1);
        int lx0 = (int)(frame_px(wave, st->loop_in) - wave->origin);
        fill(wave, lx0, 0, lx0, h - 1, edge);
        if (has_loop)
        {
            int lx1 = (int)(frame_px(wave, st->loop_out) - wave->origin);
            fill(wave, lx1, 0, lx1, h - 1, edge);
        }
    }
    uint16_t cue = sk_px(SK_CUE);
    for (int i = 0; i < 4; ++i)
    {
        if (!(st->cues & (1u << i)))
            continue;
        int cx = (int)(frame_px(wave, st->cue[i]) - wave->origin);
        for (int r = 0; r < 4; ++r)
            fill(wave, cx - 3 + r, r, cx + 3 - r, r, cue);
        fill(wave, cx, 0, cx, h - 1, sk_px(TK_AMBER_G1));
    }
    uint16_t head = sk_px(st->mode == SK_WAVE_TOUCH ? TK_AMBER : TK_TEXT);
    fill(wave, play_x - 1, 0, play_x, h - 1, head);
    for (int i = 0; i < 3; ++i)
    {
        fill(wave, play_x - 3 + i, i, play_x + 2 - i, i, head);
        fill(wave, play_x - 3 + i, h - 1 - i, play_x + 2 - i, h - 1 - i, head);
    }
}

static void invalidate_span(sk_wave_t *wave, int x0, int x1)
{
    lv_area_t a;
    lv_obj_get_coords(wave->img, &a);
    lv_area_t span = {a.x1 + (x0 < 0 ? 0 : x0), a.y1, a.x1 + (x1 >= wave->w ? wave->w - 1 : x1), a.y2};
    if (span.x1 <= span.x2)
        lv_obj_invalidate_area(wave->img, &span);
}

/* Visible maximum, for the slow-release auto gain. */
static void measure(sk_wave_t *wave, uint32_t *peak_max, uint32_t *band_max)
{
    *peak_max = 1;
    *band_max = 1;
    int64_t first = wave->origin / SK_WAVE_COL, last = (wave->origin + wave->w) / SK_WAVE_COL;
    for (int64_t col = first; col <= last; ++col)
    {
        uint16_t peak;
        uint8_t band[3];
        if (!column(wave, col, &peak, band))
            continue;
        *peak_max = peak > *peak_max ? peak : *peak_max;
        for (int k = 0; k < 3; ++k)
            *band_max = band[k] > *band_max ? band[k] : *band_max;
    }
}

static void set_gain(sk_wave_t *wave, bool release)
{
    uint32_t pm, bm;
    measure(wave, &pm, &bm);
    uint32_t norm = release ? wave->norm - wave->norm / 32 : wave->norm;
    wave->norm = pm > norm ? pm : norm < 4096 ? 4096 : norm;
    uint32_t bnorm = release ? wave->bnorm - wave->bnorm / 32 : wave->bnorm;
    wave->bnorm = bm > bnorm ? bm : bnorm < 32 ? 32 : bnorm;
}

void sk_wave_render(sk_wave_t *wave, uint32_t frame, const sk_wave_state_t *st)
{
    if (!wave || !wave->px)
        return;
    int64_t play_abs = frame_px(wave, frame);
    bool has_loop = st->loop_out > st->loop_in;
    uint8_t loop_key = has_loop ? 1 + st->loop_on : 0;
    bool same_state = wave->drawn && !wave->dirty && wave->key_mode == st->mode &&
                      wave->key_percent == st->percent && wave->key_loop == loop_key &&
                      wave->key_marked == st->marked && wave->key_in == st->loop_in &&
                      wave->key_out == st->loop_out && wave->key_cues == st->cues &&
                      !memcmp(wave->key_cue, st->cue, sizeof(wave->key_cue));
    bool full = !same_state;
    int play_x;
    if (!wave->paged)
    {
        play_x = wave->w * SK_WAVE_PAST_PCT / 100;
        if (same_state && (uint32_t)play_abs == wave->key_frame && !wave->data)
            return;
        wave->origin = play_abs - play_x;
        set_gain(wave, true);
        full = true;
    }
    else
    {
        play_x = (int)(play_abs - wave->origin);
        bool outside = play_x < 0 || play_x > wave->w * 88 / 100;
        if (outside && !full && s_flip_budget <= 0 && play_x >= 0 && play_x < wave->w - 4)
            outside = false; /* another waveform flipped this tick: flip on the next one */
        if (outside || full)
        {
            full = true;
            s_flip_budget--;
            wave->origin = (play_abs - wave->w * 12 / 100) / SK_WAVE_COL * SK_WAVE_COL;
            play_x = (int)(play_abs - wave->origin);
            wave->norm = 4096;
            wave->bnorm = 32;
            set_gain(wave, false);
        }
    }
    if (st->mode == SK_WAVE_LOADING || st->mode == SK_WAVE_EMPTY)
        full = true;
    wave->drawn = true;
    wave->dirty = false;
    wave->key_frame = (uint32_t)play_abs;
    wave->key_mode = (uint8_t)st->mode;
    wave->key_percent = (uint8_t)st->percent;
    wave->key_loop = loop_key;
    wave->key_marked = st->marked;
    wave->key_in = st->loop_in;
    wave->key_out = st->loop_out;
    wave->key_cues = st->cues;
    memcpy(wave->key_cue, st->cue, sizeof(wave->key_cue));
    if (full)
    {
        wave->data = false;
        wave->play_x = play_x;
        draw_span(wave, 0, wave->w - 1, st, play_x);
        lv_image_cache_drop(&wave->dsc);
        lv_obj_invalidate(wave->img);
        return;
    }
    /* Paged, same page: the columns the playhead crossed, then the new preview. */
    bool drew = false;
    if (play_x != wave->play_x)
    {
        int a = (play_x < wave->play_x ? play_x : wave->play_x) - 4;
        int b = (play_x > wave->play_x ? play_x : wave->play_x) + 4;
        a = (int)(((wave->origin + a) / SK_WAVE_COL * SK_WAVE_COL) - wave->origin);
        b = (int)(((wave->origin + b) / SK_WAVE_COL * SK_WAVE_COL) - wave->origin) + SK_WAVE_COL - 1;
        draw_span(wave, a, b, st, play_x);
        invalidate_span(wave, a, b);
        drew = true;
    }
    if (wave->data)
    {
        uint32_t per = wave->fpc / SK_WAVE_BIN;
        uint32_t pm, bm;
        int a = (int)((int64_t)(wave->data_lo / per) * SK_WAVE_COL - wave->origin);
        int b = (int)((int64_t)(wave->data_hi / per) * SK_WAVE_COL - wave->origin) + SK_WAVE_COL - 1;
        wave->data = false;
        measure(wave, &pm, &bm);
        if (pm > wave->norm + wave->norm / 4 || bm > wave->bnorm + wave->bnorm / 4)
            wave->dirty = true; /* much louder material: rescale on the next flip-free tick */
        if (b >= 0 && a < wave->w)
        {
            draw_span(wave, a, b, st, play_x);
            invalidate_span(wave, a, b);
            drew = true;
        }
    }
    wave->play_x = play_x;
    if (drew)
        lv_image_cache_drop(&wave->dsc);
}

/* ------------------------------------------------------------------ fader */

static int fader_thumb_w(const sk_fader_t *f) { return f->kind == SK_FADER_CROSS ? 28 : 24; }
static int fader_inset(const sk_fader_t *f) { return f->kind == SK_FADER_CROSS ? 24 : 0; }

static int fader_x(const sk_fader_t *f, const lv_area_t *a, int value)
{
    int span = lv_area_get_width(a) - 2 * fader_inset(f) - fader_thumb_w(f);
    return a->x1 + fader_inset(f) + (int)((int64_t)(value - f->min) * span / (f->max - f->min));
}

static void fader_draw(sk_fader_t *f, lv_layer_t *layer)
{
    lv_area_t a;
    lv_obj_get_coords(f->obj, &a);
    int mid = (a.y1 + a.y2) / 2, tw = fader_thumb_w(f);
    int x = fader_x(f, &a, f->value);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(TK_LINE_HI);
    lv_area_t track = {a.x1 + fader_inset(f) + tw / 2, mid - 1, a.x2 - fader_inset(f) - tw / 2, mid};
    lv_draw_rect(layer, &r, &track);
    sk_tone_t t = sk_tone(f->neon);
    if (f->kind != SK_FADER_CROSS)
    {
        int from = f->kind == SK_FADER_PITCH ? fader_x(f, &a, f->home) + tw / 2 : track.x1;
        int to = x + tw / 2;
        lv_area_t bar = {from < to ? from : to, mid - 2, from < to ? to : from, mid + 1};
        r.bg_color = lv_color_hex(f->kind == SK_FADER_PITCH ? TK_AMBER_G1 : t.g1);
        lv_draw_rect(layer, &r, &bar);
    }
    if (f->kind != SK_FADER_LEVEL)
    {
        int cx = fader_x(f, &a, f->home) + tw / 2;
        lv_area_t tick = {cx, a.y1 + 6, cx, a.y2 - 6};
        r.bg_color = lv_color_hex(TK_TEXT_3);
        lv_draw_rect(layer, &r, &tick);
    }
    else
    {
        for (int i = 0; i <= 10; ++i)
        {
            int tx = track.x1 + (track.x2 - track.x1) * i / 10;
            lv_area_t tick = {tx, mid + 6, tx, mid + (i % 5 ? 8 : 11)};
            r.bg_color = lv_color_hex(TK_LINE_HI);
            lv_draw_rect(layer, &r, &tick);
        }
    }
    if (f->kind == SK_FADER_CROSS)
    {
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.font = &lv_font_montserrat_16;
        int lh = lv_font_get_line_height(l.font);
        lv_area_t la = {a.x1, mid - lh / 2, a.x1 + 18, mid - lh / 2 + lh - 1};
        l.text = "A";
        l.color = lv_color_hex(SK_DECK_A);
        lv_draw_label(layer, &l, &la);
        la.x1 = a.x2 - 14;
        la.x2 = a.x2;
        l.text = "B";
        l.color = lv_color_hex(SK_DECK_B);
        lv_draw_label(layer, &l, &la);
    }
    lv_area_t thumb = {x, a.y1 + 4, x + tw - 1, a.y2 - 4};
    lv_draw_rect_dsc_init(&r);
    r.radius = TK_R_XS;
    r.bg_color = lv_color_hex(f->dragging ? t.g1 : TK_SURFACE_2);
    r.border_width = TK_LW;
    r.border_color = lv_color_hex(t.neon);
    lv_draw_rect(layer, &r, &thumb);
    lv_area_t line = {x + tw / 2 - 1, thumb.y1 + 6, x + tw / 2, thumb.y2 - 6};
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(t.neon);
    lv_draw_rect(layer, &r, &line);
}

static void fader_apply(sk_fader_t *f, int value)
{
    value = value < f->min ? f->min : value > f->max ? f->max : value;
    if (f->kind != SK_FADER_LEVEL && abs(value - f->home) < (f->max - f->min) / 80)
        value = f->home; /* centre detent */
    if (value == f->value)
        return;
    f->value = value;
    lv_obj_invalidate(f->obj);
    if (f->cb)
        f->cb(f, value);
}

static void fader_event(lv_event_t *e)
{
    sk_fader_t *f = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN)
    {
        fader_draw(f, lv_event_get_layer(e));
        return;
    }
    lv_point_t p;
    lv_area_t a;
    if (code == LV_EVENT_PRESSED)
    {
        touch_point(f->obj, &p, &a);
        f->dragging = true;
        f->press_x = p.x;
        f->press_value = f->value;
        lv_obj_invalidate(f->obj);
        if (f->kind != SK_FADER_LEVEL && now_ms() - f->last_release < SK_DOUBLE_TAP_MS)
            fader_apply(f, f->home);
    }
    else if (code == LV_EVENT_PRESSING && f->dragging)
    {
        touch_point(f->obj, &p, &a);
        int span = lv_area_get_width(&a) - 2 * fader_inset(f) - fader_thumb_w(f);
        if (span > 0)
            fader_apply(f, f->press_value + (int)((int64_t)(p.x - f->press_x) * (f->max - f->min) / span));
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        f->dragging = false;
        f->last_release = now_ms();
        lv_obj_invalidate(f->obj);
    }
}

sk_fader_t *sk_fader_create(lv_obj_t *parent, int x, int y, int w, int h, int kind, int min,
                            int max, int home, uint32_t neon, sk_fader_cb_t cb, int tag)
{
    sk_fader_t *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    *f = (sk_fader_t){.cb = cb, .tag = tag, .kind = kind, .min = min, .max = max, .value = home,
                      .home = home, .neon = neon};
    f->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(f->obj);
    lv_obj_set_pos(f->obj, x, y);
    lv_obj_set_size(f->obj, w, h);
    lv_obj_set_scrollable(f->obj, false);
    lv_obj_add_flag(f->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f->obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(f->obj, TK_SP_1);
    lv_obj_add_event_cb(f->obj, fader_event, LV_EVENT_ALL, f);
    return f;
}

void sk_fader_set(sk_fader_t *f, int value)
{
    if (!f || f->dragging || value == f->value)
        return;
    f->value = value < f->min ? f->min : value > f->max ? f->max : value;
    lv_obj_invalidate(f->obj);
}

/* ------------------------------------------------------------------ meter */

#define SK_METER_SEGS 12

static void meter_draw(lv_event_t *e)
{
    sk_meter_t *m = lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(m->obj, &a);
    int w = lv_area_get_width(&a), gap = 2;
    int seg = (w - gap * (SK_METER_SEGS - 1)) / SK_METER_SEGS;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    for (int i = 0; i < SK_METER_SEGS; ++i)
    {
        uint32_t lit = i >= SK_METER_SEGS - 1 ? TK_RED : i >= SK_METER_SEGS - 3 ? TK_AMBER : TK_CYAN;
        r.bg_color = lv_color_hex(i < m->lit ? lit : TK_LINE);
        lv_area_t s = {a.x1 + i * (seg + gap), a.y1, a.x1 + i * (seg + gap) + seg - 1, a.y2};
        lv_draw_rect(lv_event_get_layer(e), &r, &s);
    }
}

sk_meter_t *sk_meter_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    sk_meter_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(m->obj);
    lv_obj_set_pos(m->obj, x, y);
    lv_obj_set_size(m->obj, w, h);
    lv_obj_remove_flag(m->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->obj, meter_draw, LV_EVENT_DRAW_MAIN, m);
    return m;
}

void sk_meter_set(sk_meter_t *m, uint16_t peak)
{
    if (!m)
        return;
    int lit = 0;
    if (peak)
    {
        float db = 20.f * log10f(peak / 32768.f); /* -48 .. 0 dBFS in 4 dB segments */
        lit = (int)((db + 48.f) / 4.f + 1.f);
        lit = lit < 0 ? 0 : lit > SK_METER_SEGS ? SK_METER_SEGS : lit;
    }
    if (lit != m->lit)
    {
        m->lit = lit;
        lv_obj_invalidate(m->obj);
    }
}
