/*
 * Ombra UI. One screen, one page at a time:
 *
 *   FIELD   home: how many signals, the depth ruler (every device is a tick
 *           placed by distance, tall when strong, faded when far), shield state
 *   MENU    entries below
 *   NEARBY  cards nearest first, faded with distance (all, or trackers only)
 *   DETAIL  one device: distance, 60 s signal trace, facts, find / mine
 *   FIND    hot/cold: the whole screen warms up as you get closer
 *   SHIELD  anti-tracking: who is following you, threshold, trackers around
 *   MIX     composition by kind
 *
 * Widgets are created once per page and updated in place (1 Hz lists,
 * ~16 Hz find): no rebuild under the finger, no layout work per frame.
 */
#include "ombra.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIELD, MENU, NEARBY, DETAIL, FIND, SHIELD, MIX };

#define C_BG 0x07070C
#define C_CARD 0x121219
#define C_INK 0xF3EEE6
#define C_MIST 0x8A879C
#define C_DIM 0x3A3848
#define C_EMBER 0xFF6A3D
#define C_SAFE 0x5BD6A0
#define C_COLD 0x0B2A5B

#define TICKS 40
#define CARDS 24
#define SNAPSHOT 128

static const uint32_t s_kind_color[OMBRA_KIND_COUNT] = {
    0x6E6A80, C_EMBER, 0x7AA7FF, 0x9C8CFF, 0x4FD1C5, 0xF2C14E,
    0xE36FB0, 0xB7E36F, 0x5EC2F2, 0x8BD97A, 0xFF8FA3, 0xC9A27A,
};

typedef struct
{
    lv_obj_t *card, *bar, *title, *subtitle, *distance, *signal;
    uint32_t id;
} card_t;

typedef struct
{
    lv_obj_t *screen, *content, *page;
    lv_timer_t *timer;
    int view, filter, back_to;
    bool screen_kept;
    uint32_t selected, shown_alert, revision;
    int frames;
    /* field */
    lv_obj_t *count, *count_caption, *pill, *pill_dot, *strip, *kinds[3];
    lv_obj_t *ticks[TICKS];
    uint32_t tick_id[TICKS];
    int strip_w, strip_h;
    /* lists */
    card_t cards[CARDS];
    lv_obj_t *list_caption;
    /* detail */
    lv_obj_t *d_distance, *d_signal, *d_seen, *d_follow, *chart, *mine_button;
    lv_chart_series_t *series;
    /* find */
    lv_obj_t *f_word, *f_distance, *f_unit, *f_trend, *f_arc, *f_name, *f_sound;
    int32_t f_shown;      /* displayed proximity x1000 (eased) */
    int32_t f_trend_ref;  /* proximity 2 s ago */
    uint32_t f_trend_at;
    ombra_device_t *snapshot;
    ombra_status_t status;
} ombra_ui_t;

static ombra_ui_t *s_ui;
static void render(ombra_ui_t *ui);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------ text helpers */

static void distance_text(uint32_t cm, char *out, size_t size)
{
    if (cm < 100)
        snprintf(out, size, "%lu cm", (unsigned long)(cm < 10 ? 10 : cm));
    else if (cm < 1000)
        snprintf(out, size, "%lu,%lu m", (unsigned long)(cm / 100), (unsigned long)(cm % 100 / 10));
    else
        snprintf(out, size, "%lu m", (unsigned long)((cm + 50) / 100));
}

static void duration_text(uint32_t ms, char *out, size_t size)
{
    uint32_t s = ms / 1000;
    if (s < 60)
        snprintf(out, size, "%lu s", (unsigned long)s);
    else if (s < 3600)
        snprintf(out, size, "%lu min", (unsigned long)(s / 60));
    else
        snprintf(out, size, "%lu h %02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

static void mac_text(const uint8_t mac[6], char *out, size_t size)
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
}

static uint32_t kind_color(const ombra_device_t *d)
{
    return d->adv.kind < OMBRA_KIND_COUNT ? s_kind_color[d->adv.kind] : s_kind_color[0];
}

/* Shadow metaphor: near devices are solid, far ones fade (40 cm -> 100 %, 25 m -> 35 %). */
static lv_opa_t depth_opa(uint32_t cm)
{
    if (cm <= 40)
        return LV_OPA_COVER;
    if (cm >= 2500)
        return 90;
    /* Logarithmic: equal steps per doubling of distance. */
    int doublings = 0;
    for (uint32_t d = 40; d < cm; d *= 2)
        doublings++;
    return (lv_opa_t)(255 - doublings * 165 / 6);
}

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

static void go(ombra_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

/* ------------------------------------------------------------ field (home) */

static void open_menu(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED)
        go(s_ui, MENU);
    else if (code == LV_EVENT_GESTURE)
    {
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP)
        {
            lv_indev_wait_release(input);
            go(s_ui, MENU);
        }
    }
}

/* x on the depth ruler: logarithmic, 20 cm at the left edge, 32 m at the right. */
static int depth_x(int width, uint32_t cm)
{
    if (cm < 20)
        cm = 20;
    int steps = 0; /* tenths of a doubling above 20 cm */
    uint32_t base = 20;
    while (base * 2 <= cm && steps < 110)
    {
        base *= 2;
        steps += 10;
    }
    steps += (int)((cm - base) * 10 / base);
    if (steps > 106) /* 20 cm * 2^10.6 = 32 m */
        steps = 106;
    return steps * (width - 4) / 106;
}

static void field(ombra_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height;
    lv_obj_t *c = ui->content;
    lv_obj_set_style_bg_color(c, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);

    lv_obj_t *brand = label(c, UI_FONT_SMALL, C_MIST, "OMBRA");
    lv_obj_set_style_text_letter_space(brand, 6, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, h * (b->round ? 11 : 6) / 100);

    /* Shield pill: a dot and a short state. */
    ui->pill = plain(c);
    lv_obj_set_size(ui->pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui->pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui->pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(ui->pill, 12, 0);
    lv_obj_set_style_pad_ver(ui->pill, 5, 0);
    lv_obj_set_style_pad_column(ui->pill, 8, 0);
    lv_obj_set_style_radius(ui->pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui->pill, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(ui->pill, LV_OPA_COVER, 0);
    ui->pill_dot = plain(ui->pill);
    lv_obj_set_size(ui->pill_dot, 8, 8);
    lv_obj_set_style_radius(ui->pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ui->pill_dot, LV_OPA_COVER, 0);
    label(ui->pill, UI_FONT_SMALL, C_INK, "");
    lv_obj_align(ui->pill, LV_ALIGN_TOP_MID, 0, h * (b->round ? 18 : 12) / 100);

    bool large = h >= 360;
    const lv_font_t *count_font = large ? ui_font_clock : ui_font_big;
    int count_y = h * (b->round ? 25 : 22) / 100;
    ui->count = label(c, count_font, C_INK, "0");
    lv_obj_align(ui->count, LV_ALIGN_TOP_MID, 0, count_y);
    /* Aligned to the screen, not to the number: it stays centred whatever the count. */
    ui->count_caption = label(c, UI_FONT_SMALL, C_MIST, "segnali intorno a te");
    lv_obj_align(ui->count_caption, LV_ALIGN_TOP_MID, 0, count_y + lv_font_get_line_height(count_font));

    /* Depth ruler. */
    ui->strip_w = w * (b->round ? 70 : 84) / 100;
    ui->strip_h = large ? 64 : 44;
    ui->strip = plain(c);
    lv_obj_set_size(ui->strip, ui->strip_w, ui->strip_h + 22);
    lv_obj_align(ui->strip, LV_ALIGN_TOP_MID, 0, h * (b->round ? 58 : 58) / 100);
    lv_obj_t *base = plain(ui->strip);
    lv_obj_set_size(base, ui->strip_w, 1);
    lv_obj_set_pos(base, 0, ui->strip_h);
    lv_obj_set_style_bg_color(base, ui_color(C_DIM), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    static const struct
    {
        uint32_t cm;
        const char *text;
    } marks[] = {{20, "qui"}, {100, "1 m"}, {500, "5 m"}, {2000, "20 m"}};
    for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
    {
        int x = depth_x(ui->strip_w, marks[i].cm);
        lv_obj_t *notch = plain(ui->strip);
        lv_obj_set_size(notch, 1, 5);
        lv_obj_set_pos(notch, x, ui->strip_h);
        lv_obj_set_style_bg_color(notch, ui_color(C_DIM), 0);
        lv_obj_set_style_bg_opa(notch, LV_OPA_COVER, 0);
        lv_obj_t *t = label(ui->strip, UI_FONT_SMALL, C_DIM, marks[i].text);
        lv_obj_set_style_text_color(t, ui_color(0x5C5A6C), 0);
        lv_obj_update_layout(t);
        int tx = x - lv_obj_get_width(t) / 2;
        tx = tx < 0 ? 0 : tx > ui->strip_w - lv_obj_get_width(t) ? ui->strip_w - lv_obj_get_width(t) : tx;
        lv_obj_set_pos(t, tx, ui->strip_h + 5);
    }
    for (int i = 0; i < TICKS; i++)
    {
        lv_obj_t *t = plain(ui->strip);
        lv_obj_set_size(t, 3, 10);
        lv_obj_set_style_radius(t, 2, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_hidden(t, true);
        ui->ticks[i] = t;
        ui->tick_id[i] = 0;
    }

    /* The three most common kinds right now. */
    lv_obj_t *row = plain(c);
    lv_obj_set_size(row, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, h * (b->round ? 77 : 78) / 100);
    for (int i = 0; i < 3; i++)
        ui->kinds[i] = label(row, UI_FONT_SMALL, C_MIST, "");

    lv_obj_t *hint = label(c, UI_FONT_SMALL, 0x5C5A6C, "tocca per esplorare");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -h * (b->round ? 9 : 4) / 100);

    lv_obj_set_clickable(c, true);
    lv_obj_set_gesture_bubble(c, false);
    lv_obj_add_event_cb(c, open_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(c, open_menu, LV_EVENT_GESTURE, NULL);
    lv_port_set_focusable(c);
}

static void tick_move(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void field_update(ombra_ui_t *ui)
{
    char text[64];
    snprintf(text, sizeof(text), "%d", ui->status.total);
    lv_label_set_text(ui->count, text);
    lv_label_set_text(ui->count_caption, ui->status.scanning ? (ui->status.total == 1 ? "segnale intorno a te" : "segnali intorno a te")
                                                               : "Bluetooth in avvio...");
    lv_obj_t *pill_text = lv_obj_get_child(ui->pill, 1);
    uint32_t dot = ui->status.shield ? C_SAFE : C_DIM;
    if (ui->status.suspects)
    {
        snprintf(text, sizeof(text), ui->status.suspects == 1 ? "1 tracker ti segue" : "%d tracker ti seguono",
                 ui->status.suspects);
        dot = C_EMBER;
    }
    else if (ui->status.shield)
        snprintf(text, sizeof(text), "Scudo attivo");
    else
        snprintf(text, sizeof(text), "Scudo spento");
    lv_label_set_text(pill_text, text);
    lv_obj_set_style_text_color(pill_text, ui_color(ui->status.suspects ? C_EMBER : C_INK), 0);
    lv_obj_set_style_bg_color(ui->pill_dot, ui_color(dot), 0);
    lv_obj_set_style_text_color(ui->count, ui_color(ui->status.suspects ? C_EMBER : C_INK), 0);

    /* Top three kinds. */
    int order[OMBRA_KIND_COUNT];
    for (int i = 0; i < OMBRA_KIND_COUNT; i++)
        order[i] = i;
    for (int i = 0; i < OMBRA_KIND_COUNT; i++)
        for (int j = i + 1; j < OMBRA_KIND_COUNT; j++)
            if (ui->status.kinds[order[j]] > ui->status.kinds[order[i]])
            {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    for (int i = 0; i < 3; i++)
    {
        int k = order[i], n = ui->status.kinds[k];
        lv_obj_set_hidden(ui->kinds[i], n == 0);
        if (n == 0)
            continue;
        static const char *const plural[OMBRA_KIND_COUNT] = {
            "altri", "tracker", "telefoni", "computer", "orologi", "audio",
            "TV", "comandi", "beacon", "casa", "salute", "veicoli",
        };
        snprintf(text, sizeof(text), "%d %s", n, plural[k]);
        lv_label_set_text(ui->kinds[i], text);
        lv_obj_set_style_text_color(ui->kinds[i], ui_color(s_kind_color[k]), 0);
    }

    /* Ticks keep their device: they glide when it moves, never jump between devices. */
    int count = ombra_snapshot(ui->snapshot, SNAPSHOT, -1);
    if (count > TICKS)
        count = TICKS;
    bool used[TICKS] = {false};
    int slot_of[TICKS];
    for (int i = 0; i < count; i++)
    {
        slot_of[i] = -1;
        for (int s = 0; s < TICKS; s++)
            if (ui->tick_id[s] == ui->snapshot[i].id)
            {
                slot_of[i] = s;
                used[s] = true;
            }
    }
    for (int i = 0; i < count; i++)
        for (int s = 0; s < TICKS && slot_of[i] < 0; s++)
            if (!used[s])
            {
                slot_of[i] = s;
                used[s] = true;
                ui->tick_id[s] = 0; /* new: placed without gliding */
            }
    for (int s = 0; s < TICKS; s++)
        if (!used[s])
        {
            ui->tick_id[s] = 0;
            lv_obj_set_hidden(ui->ticks[s], true);
        }
    for (int i = 0; i < count; i++)
    {
        const ombra_device_t *d = &ui->snapshot[i];
        lv_obj_t *t = ui->ticks[slot_of[i]];
        int strength = d->rssi + 100; /* 0 .. 70 */
        strength = strength < 4 ? 4 : strength > 70 ? 70 : strength;
        int height = 6 + strength * (ui->strip_h - 6) / 70;
        int x = depth_x(ui->strip_w, d->distance_cm);
        bool tracker = d->adv.kind == OMBRA_KIND_TRACKER;
        lv_obj_set_size(t, tracker ? 5 : 3, height);
        lv_obj_set_y(t, ui->strip_h - height);
        lv_obj_set_style_bg_color(t, ui_color(d->suspect ? C_EMBER : kind_color(d)), 0);
        lv_obj_set_style_bg_opa(t, depth_opa(d->distance_cm), 0);
        if (ui->tick_id[slot_of[i]] == d->id && !lv_obj_is_hidden(t))
        {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, t);
            lv_anim_set_exec_cb(&a, tick_move);
            lv_anim_set_values(&a, lv_obj_get_x(t), x);
            lv_anim_set_duration(&a, 600);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
        else
            lv_obj_set_x(t, x);
        ui->tick_id[slot_of[i]] = d->id;
        lv_obj_set_hidden(t, false);
    }
}

/* ------------------------------------------------------------ lists */

static void open_device(lv_event_t *e)
{
    card_t *card = lv_event_get_user_data(e);
    if (card->id == 0)
        return;
    s_ui->selected = card->id;
    s_ui->back_to = s_ui->view;
    go(s_ui, DETAIL);
}

static void cards(ombra_ui_t *ui)
{
    for (int i = 0; i < CARDS; i++)
    {
        card_t *k = &ui->cards[i];
        k->card = lv_obj_create(ui->page);
        lv_obj_remove_style_all(k->card);
        lv_obj_set_size(k->card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(k->card, ui_color(C_CARD), 0);
        lv_obj_set_style_bg_opa(k->card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(k->card, ui_color(0x1E1D27), LV_STATE_PRESSED);
        lv_obj_set_style_radius(k->card, 16, 0);
        lv_obj_set_style_pad_all(k->card, 12, 0);
        lv_obj_set_style_pad_left(k->card, 18, 0);
        lv_obj_set_style_border_color(k->card, ui_color(C_EMBER), 0);
        lv_obj_set_scrollable(k->card, false);
        lv_obj_add_event_cb(k->card, open_device, LV_EVENT_CLICKED, k);
        lv_port_set_focusable(k->card);
        k->bar = plain(k->card);
        lv_obj_set_size(k->bar, 4, LV_PCT(100));
        lv_obj_set_style_radius(k->bar, 2, 0);
        lv_obj_set_style_bg_opa(k->bar, LV_OPA_COVER, 0);
        lv_obj_align(k->bar, LV_ALIGN_LEFT_MID, -10, 0);
        k->title = label(k->card, UI_FONT_BODY, C_INK, "");
        lv_label_set_long_mode(k->title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(k->title, LV_PCT(66), lv_font_get_line_height(UI_FONT_BODY));
        k->subtitle = label(k->card, UI_FONT_SMALL, C_MIST, "");
        lv_label_set_long_mode(k->subtitle, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(k->subtitle, LV_PCT(66), lv_font_get_line_height(UI_FONT_SMALL));
        lv_obj_align(k->subtitle, LV_ALIGN_TOP_LEFT, 0, 26);
        k->distance = label(k->card, UI_FONT_LARGE, C_INK, "");
        lv_obj_align(k->distance, LV_ALIGN_TOP_RIGHT, 0, -2);
        k->signal = label(k->card, UI_FONT_SMALL, C_MIST, "");
        lv_obj_align(k->signal, LV_ALIGN_TOP_RIGHT, 0, 26);
        lv_obj_set_hidden(k->card, true);
        k->id = 0;
    }
}

static void list_update(ombra_ui_t *ui)
{
    /* Never reorder under the finger. */
    if (lv_obj_is_scrolling(ui->page))
        return;
    for (int i = 0; i < CARDS; i++)
        if (lv_obj_has_state(ui->cards[i].card, LV_STATE_PRESSED))
            return;
    int count = ombra_snapshot(ui->snapshot, SNAPSHOT, ui->filter);
    char text[96], name[48];
    if (ui->list_caption)
    {
        if (count == 0)
            snprintf(text, sizeof(text), ui->filter == OMBRA_KIND_TRACKER ? "Nessun tracker nei dintorni." : "Nessun segnale per ora.");
        else
            snprintf(text, sizeof(text), "%d %s, dal più vicino", count, count == 1 ? "segnale" : "segnali");
        lv_label_set_text(ui->list_caption, text);
    }
    uint32_t now = now_ms();
    for (int i = 0; i < CARDS; i++)
    {
        card_t *k = &ui->cards[i];
        if (i >= count)
        {
            lv_obj_set_hidden(k->card, true);
            k->id = 0;
            continue;
        }
        const ombra_device_t *d = &ui->snapshot[i];
        k->id = d->id;
        ombra_display_name(d, name, sizeof(name));
        lv_label_set_text(k->title, name);
        const char *company = classify_company(d->adv.company);
        if (d->suspect)
        {
            char follow[16];
            duration_text(d->follow_ms, follow, sizeof(follow));
            snprintf(text, sizeof(text), "Ti segue da %s", follow);
        }
        else if (d->adv.kind == OMBRA_KIND_TRACKER)
            snprintf(text, sizeof(text), "%s%s", d->adv.separated ? "Lontano dal proprietario" : "Vicino al proprietario",
                     d->mine ? " \u00B7 mio" : "");
        else
        {
            char seen[16];
            duration_text(now - d->first_ms, seen, sizeof(seen));
            snprintf(text, sizeof(text), "%s%s%s \u00B7 %s", classify_kind_name((ombra_kind_t)d->adv.kind),
                     company ? " \u00B7 " : "", company ? company : "", seen);
        }
        lv_label_set_text(k->subtitle, text);
        lv_obj_set_style_text_color(k->subtitle, ui_color(d->suspect ? C_EMBER : C_MIST), 0);
        distance_text(d->distance_cm, text, sizeof(text));
        lv_label_set_text(k->distance, text);
        snprintf(text, sizeof(text), "%d dBm", d->rssi);
        lv_label_set_text(k->signal, text);
        lv_obj_set_style_bg_color(k->bar, ui_color(d->suspect ? C_EMBER : kind_color(d)), 0);
        lv_obj_set_style_border_width(k->card, d->suspect ? 2 : 0, 0);
        lv_obj_set_style_opa(k->card, now - d->last_ms > 8000 ? 70 : depth_opa(d->distance_cm), 0);
        lv_obj_set_hidden(k->card, false);
    }
}

/* ------------------------------------------------------------ detail */

static void detail_action(lv_event_t *e)
{
    ombra_ui_t *ui = s_ui;
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    ombra_device_t d;
    if (!ombra_get(ui->selected, &d))
    {
        ui_toast("Dispositivo non più nei dintorni");
        return;
    }
    if (what == 1)
        go(ui, FIND);
    else
    {
        ombra_set_mine(d.id, !d.mine);
        ui_toast(d.mine ? "Non più segnato come tuo" : "Segnato come tuo: niente allarmi");
        lv_label_set_text(lv_obj_get_child(ui->mine_button, 0), d.mine ? "È mio" : "Non è mio");
    }
}

static void fact(lv_obj_t *page, const char *name, const char *value)
{
    lv_obj_t *row = plain(page);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *n = label(row, UI_FONT_SMALL, C_MIST, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = label(row, UI_FONT_SMALL, C_INK, value);
    lv_obj_set_width(v, LV_PCT(62));
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
}

static void detail(ombra_ui_t *ui)
{
    ombra_device_t d;
    lv_obj_t *page = ui->page;
    if (!ombra_get(ui->selected, &d))
    {
        ui_title(page, "Sparito");
        ui_text(page, "Questo dispositivo non trasmette più da un po'.", true);
        return;
    }
    char text[200], name[48];
    uint32_t color = d.suspect ? C_EMBER : kind_color(&d);
    snprintf(text, sizeof(text), "%s", d.adv.kind == OMBRA_KIND_TRACKER ? "TRACKER" : classify_kind_name((ombra_kind_t)d.adv.kind));
    for (char *p = text; *p; p++)
        *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    lv_obj_t *kind = label(page, UI_FONT_SMALL, color, text);
    lv_obj_set_style_text_letter_space(kind, 3, 0);
    ombra_display_name(&d, name, sizeof(name));
    ui_title(page, name);

    ui->d_distance = label(page, ui_font_big, C_INK, "");
    ui->d_signal = label(page, UI_FONT_SMALL, C_MIST, "");

    ui->chart = lv_chart_create(page);
    lv_obj_set_size(ui->chart, LV_PCT(100), 74);
    lv_chart_set_type(ui->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui->chart, OMBRA_HISTORY);
    lv_chart_set_axis_range(ui->chart, LV_CHART_AXIS_PRIMARY_Y, -100, -30);
    lv_chart_set_div_line_count(ui->chart, 3, 0);
    lv_obj_set_style_bg_opa(ui->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->chart, 0, 0);
    lv_obj_set_style_line_color(ui->chart, ui_color(0x1C1B24), LV_PART_MAIN);
    lv_obj_set_style_size(ui->chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ui->chart, 2, LV_PART_ITEMS);
    ui->series = lv_chart_add_series(ui->chart, ui_color(color), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t *caption = label(page, UI_FONT_SMALL, 0x5C5A6C, "segnale, ultimi 60 secondi");
    (void)caption;

    lv_obj_t *buttons = plain(page);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(buttons, 8, 0);
    ui_button(buttons, "Trova - caldo / freddo", C_EMBER, detail_action, (void *)1);
    ui->mine_button = ui_button(buttons, d.mine ? "Non è mio" : "È mio", 0x2A2936, detail_action, (void *)2);

    const char *company = classify_company(d.adv.company);
    if (company)
        fact(page, "Produttore", company);
    if (d.adv.detail[0])
        fact(page, "Riconosciuto", d.adv.detail);
    if (d.adv.name[0] && d.adv.tracker != OMBRA_TRACKER_NONE)
        fact(page, "Nome", d.adv.name);
    mac_text(d.mac, text, sizeof(text));
    fact(page, "Indirizzo", text);
    fact(page, "Tipo indirizzo", ombra_addr_text((ombra_addr_t)d.addr_type));
    lv_obj_t *seen = plain(page);
    lv_obj_set_size(seen, LV_PCT(100), LV_SIZE_CONTENT);
    ui->d_seen = label(seen, UI_FONT_SMALL, C_MIST, "");
    lv_obj_set_width(ui->d_seen, LV_PCT(100));
    if (d.adv.kind == OMBRA_KIND_TRACKER)
    {
        static const char *const battery[] = {"piena", "media", "bassa", "quasi scarica"};
        if (d.adv.battery >= 0 && d.adv.battery <= 3)
            fact(page, "Batteria", battery[d.adv.battery]);
        ui->d_follow = ui_text(page, "", true);
        ui_text(page,
                d.adv.separated
                    ? "Lontano dal suo proprietario: se resta con te mentre ti sposti, può essere usato per seguirti. Ombra lo riconosce anche quando cambia indirizzo."
                    : "Vicino al suo proprietario: è normale incontrarne in giro.",
                true);
    }
    snprintf(text, sizeof(text), "Potenza attesa a 1 m: %d dBm%s. La distanza è una stima: muri e corpi la allungano.",
             d.reference, d.adv.has_tx_power ? " (dichiarata)" : "");
    ui_text(page, text, true);
}

static void detail_update(ombra_ui_t *ui)
{
    ombra_device_t d;
    if (!ui->d_distance || !ombra_get(ui->selected, &d))
        return;
    char text[96], distance[16], seen[16];
    uint32_t now = now_ms(), silent = now - d.last_ms;
    distance_text(d.distance_cm, distance, sizeof(distance));
    snprintf(text, sizeof(text), "%s%s", silent > 8000 ? "~" : "", distance);
    lv_label_set_text(ui->d_distance, text);
    if (silent > 8000)
        snprintf(text, sizeof(text), "silenzioso da %lu s", (unsigned long)(silent / 1000));
    else
        snprintf(text, sizeof(text), "%d dBm - migliore %d dBm", d.rssi, d.best);
    lv_label_set_text(ui->d_signal, text);
    for (int i = 0; i < OMBRA_HISTORY; i++)
        lv_chart_set_series_value_by_id(ui->chart, ui->series, (uint32_t)i,
                                        d.history[i] <= -127 ? LV_CHART_POINT_NONE : d.history[i]);
    lv_chart_refresh(ui->chart);
    duration_text(now - d.first_ms, seen, sizeof(seen));
    snprintf(text, sizeof(text), "Nei dintorni da %s - %lu pacchetti", seen, (unsigned long)d.frames);
    lv_label_set_text(ui->d_seen, text);
    if (ui->d_follow)
    {
        duration_text(d.follow_ms, seen, sizeof(seen));
        snprintf(text, sizeof(text), d.suspect ? "Ti segue da %s" : "Con te da %s", seen);
        lv_label_set_text(ui->d_follow, text);
        lv_obj_set_style_text_color(ui->d_follow, ui_color(d.suspect ? C_EMBER : C_MIST), 0);
    }
}

/* ------------------------------------------------------------ find */

static void toggle_sound(lv_event_t *e)
{
    (void)e;
    ombra_sound_enable(!ombra_sound_enabled());
    lv_label_set_text(s_ui->f_sound, ombra_sound_enabled() ? LV_SYMBOL_VOLUME_MAX "  Geiger attivo" : LV_SYMBOL_MUTE "  Geiger muto");
}

static void find(ombra_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height, side = w < h ? w : h;
    lv_obj_t *c = ui->content;
    lv_obj_set_style_bg_color(c, ui_color(C_COLD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    ui->f_arc = lv_arc_create(c);
    lv_obj_set_size(ui->f_arc, side - 12, side - 12);
    lv_obj_center(ui->f_arc);
    lv_arc_set_bg_angles(ui->f_arc, 0, 360);
    lv_arc_set_rotation(ui->f_arc, 270);
    lv_arc_set_range(ui->f_arc, 0, 1000);
    lv_arc_set_value(ui->f_arc, 0);
    lv_obj_remove_style(ui->f_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui->f_arc, false);
    lv_obj_set_style_arc_width(ui->f_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->f_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->f_arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ui->f_arc, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->f_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->f_arc, true, LV_PART_INDICATOR);

    ombra_device_t d;
    char name[48] = "Dispositivo";
    if (ombra_get(ui->selected, &d))
        ombra_display_name(&d, name, sizeof(name));
    ui->f_name = label(c, UI_FONT_SMALL, 0xFFFFFF, name);
    lv_obj_set_style_text_opa(ui->f_name, 200, 0);
    lv_obj_align(ui->f_name, LV_ALIGN_TOP_MID, 0, h * (b->round ? 20 : 14) / 100);
    ui->f_word = label(c, ui_font_title, 0xFFFFFF, "In ascolto");
    lv_obj_align(ui->f_word, LV_ALIGN_TOP_MID, 0, h * (b->round ? 28 : 23) / 100);
    const lv_font_t *digits = h >= 360 ? ui_font_clock : ui_font_big;
    ui->f_distance = label(c, digits, 0xFFFFFF, "--");
    lv_obj_align(ui->f_distance, LV_ALIGN_CENTER, 0, -8);
    ui->f_unit = label(c, ui_font_title, 0xFFFFFF, "");
    lv_obj_set_style_text_opa(ui->f_unit, 220, 0);
    lv_obj_align(ui->f_unit, LV_ALIGN_CENTER, 0, lv_font_get_line_height(digits) / 2 + 4);
    ui->f_trend = label(c, UI_FONT_BODY, 0xFFFFFF, "");
    lv_obj_set_style_text_opa(ui->f_trend, 220, 0);
    lv_obj_align(ui->f_trend, LV_ALIGN_CENTER, 0, h >= 360 ? 118 : 74);
    ui->f_sound = label(c, UI_FONT_SMALL, 0xFFFFFF, "");
    lv_obj_set_style_text_opa(ui->f_sound, 200, 0);
    lv_obj_align(ui->f_sound, LV_ALIGN_BOTTOM_MID, 0, -h * (b->round ? 16 : 7) / 100);
    if (ombra_sound_available())
    {
        lv_label_set_text(ui->f_sound, ombra_sound_enabled() ? LV_SYMBOL_VOLUME_MAX "  Geiger attivo" : LV_SYMBOL_MUTE "  Geiger muto");
        lv_obj_set_clickable(ui->f_sound, true);
        lv_obj_set_ext_click_area(ui->f_sound, 24);
        lv_obj_add_event_cb(ui->f_sound, toggle_sound, LV_EVENT_CLICKED, NULL);
        lv_port_set_focusable(ui->f_sound);
    }
    ui->f_shown = 0;
    ui->f_trend_ref = -1;
    ui->f_trend_at = now_ms();
    ombra_sound_find(ui->selected);
    if (!ui->screen_kept)
    {
        power_keep_screen_on(true);
        ui->screen_kept = true;
    }
}

static lv_color_t warmth(int32_t p1000)
{
    /* Cold navy -> violet -> ember: through violet, not grey. */
    static const uint32_t stops[] = {0x0B2A5B, 0x3A2A78, 0x8A2F6E, 0xD9473F, 0xFF6A3D};
    int32_t position = p1000 * 4;
    int index = position / 1000;
    if (index >= 4)
        return ui_color(stops[4]);
    return lv_color_mix(ui_color(stops[index + 1]), ui_color(stops[index]), (uint8_t)((position % 1000) * 255 / 1000));
}

static void find_update(ombra_ui_t *ui)
{
    ombra_device_t d;
    bool known = ombra_get(ui->selected, &d);
    uint32_t now = now_ms();
    bool lost = !known || now - d.last_ms > 5000;
    int32_t target = 0;
    if (!lost)
    {
        target = (d.rssi + 95) * 1000 / 57;
        target = target < 0 ? 0 : target > 1000 ? 1000 : target;
    }
    /* Ease toward the target: smooth colour, honest numbers. */
    ui->f_shown += (target - ui->f_shown) / 6;
    lv_obj_set_style_bg_color(ui->content, lost ? ui_color(0x1A1A22) : warmth(ui->f_shown), 0);
    lv_arc_set_value(ui->f_arc, (int32_t)ui->f_shown);
    if (++ui->frames % 4 != 0) /* text at 4 Hz: steady enough to read */
        return;
    char text[48];
    if (lost)
    {
        lv_label_set_text(ui->f_word, "Segnale perso");
        lv_label_set_text(ui->f_distance, "--");
        lv_label_set_text(ui->f_unit, "");
        lv_label_set_text(ui->f_trend, "muoviti lentamente");
        return;
    }
    static const char *const words[] = {"Gelido", "Freddo", "Tiepido", "Caldo", "Bollente", "Ci sei!"};
    int level = ui->f_shown < 150 ? 0 : ui->f_shown < 350 ? 1 : ui->f_shown < 550 ? 2 : ui->f_shown < 750 ? 3 : ui->f_shown < 900 ? 4 : 5;
    lv_label_set_text(ui->f_word, words[level]);
    /* "1,2 m" -> "1,2" big and "m" small. */
    distance_text(d.distance_cm, text, sizeof(text));
    char *unit = strchr(text, ' ');
    lv_label_set_text(ui->f_unit, unit ? unit + 1 : "");
    if (unit)
        *unit = 0;
    lv_label_set_text(ui->f_distance, text);
    /* Trend over the last two seconds of the eased value. */
    if (ui->f_trend_ref < 0)
        ui->f_trend_ref = ui->f_shown;
    if (now - ui->f_trend_at >= 2000)
    {
        int32_t delta = ui->f_shown - ui->f_trend_ref;
        lv_label_set_text(ui->f_trend, delta > 25 ? LV_SYMBOL_UP "  ti avvicini" : delta < -25 ? LV_SYMBOL_DOWN "  ti allontani" : "fermo");
        ui->f_trend_ref = ui->f_shown;
        ui->f_trend_at = now;
    }
}

/* ------------------------------------------------------------ shield */

static void shield_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ombra_set_shield(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void shield_threshold(lv_event_t *e)
{
    static const int steps[] = {10, 15, 30, 60};
    int next = steps[0];
    for (int i = 0; i < 4; i++)
        if (steps[i] == s_ui->status.threshold_min)
            next = steps[(i + 1) % 4];
    ombra_set_threshold(next);
    char text[16];
    snprintf(text, sizeof(text), "%d min", next);
    lv_label_set_text(ui_row_value(lv_event_get_current_target(e)), text);
    s_ui->status.threshold_min = next;
}

static void shield(ombra_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    ombra_status(&ui->status);
    bool alarm = ui->status.suspects > 0;
    lv_obj_t *disc = plain(page);
    lv_obj_set_size(disc, 112, 112);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(disc, 5, 0);
    lv_obj_set_style_border_color(disc, ui_color(alarm ? C_EMBER : ui->status.shield ? C_SAFE : C_DIM), 0);
    lv_obj_set_style_bg_color(disc, ui_color(alarm ? 0x2A120C : 0x0D1A15), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_t *mark = label(disc, &lv_font_montserrat_48, alarm ? C_EMBER : C_SAFE, alarm ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
    lv_obj_center(mark);
    char text[128];
    if (alarm)
        snprintf(text, sizeof(text), ui->status.suspects == 1 ? "Un tracker ti segue" : "%d tracker ti seguono", ui->status.suspects);
    else
        snprintf(text, sizeof(text), ui->status.shield ? "Nessun tracker ti segue" : "Scudo spento");
    lv_obj_t *t = ui_title(page, text);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(text, sizeof(text), "%d tracker nei dintorni", ui->status.trackers);
    ui_text(page, text, true);
    ui_switch_row(page, LV_SYMBOL_EYE_OPEN, C_SAFE, "Scudo attivo", ui->status.shield, shield_switch, NULL);
    snprintf(text, sizeof(text), "%d min", ui->status.threshold_min);
    ui_row(page, LV_SYMBOL_BELL, C_EMBER, "Avvisa dopo", text, shield_threshold, NULL);
    int count = ombra_snapshot(ui->snapshot, SNAPSHOT, OMBRA_KIND_TRACKER);
    if (count)
        ui_section(page, "TRACKER VICINI");
    ui->filter = OMBRA_KIND_TRACKER;
    /* Reuse the cards: same look and update path as the lists. */
    cards(ui);
    list_update(ui);
    ui_text(page,
            "Un tracker separato dal proprietario che resta con te oltre la soglia fa scattare l'allarme, anche a "
            "schermo spento. Segna i tuoi come \"mio\". Tienilo attivo quando esci: a casa i tracker dei vicini restano "
            "fermi, non ti seguono.",
            true);
}

/* ------------------------------------------------------------ composition */

static void mix(ombra_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    ui_title(page, "Composizione");
    int count = ombra_snapshot(ui->snapshot, SNAPSHOT, -1);
    int kinds[OMBRA_KIND_COUNT] = {0}, private_addr = 0, named = 0;
    for (int i = 0; i < count; i++)
    {
        kinds[ui->snapshot[i].adv.kind < OMBRA_KIND_COUNT ? ui->snapshot[i].adv.kind : 0]++;
        private_addr += ui->snapshot[i].addr_type != OMBRA_ADDR_PUBLIC;
        named += ui->snapshot[i].adv.name[0] != 0;
    }
    int top = 1;
    for (int k = 0; k < OMBRA_KIND_COUNT; k++)
        top = kinds[k] > top ? kinds[k] : top;
    for (int k = 1; k <= OMBRA_KIND_COUNT; k++)
    {
        int kind = k % OMBRA_KIND_COUNT; /* unknown last */
        if (!kinds[kind])
            continue;
        lv_obj_t *row = plain(page);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        char text[40];
        snprintf(text, sizeof(text), "%s", classify_kind_name((ombra_kind_t)kind));
        lv_obj_t *n = label(row, UI_FONT_SMALL, C_INK, text);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        snprintf(text, sizeof(text), "%d", kinds[kind]);
        lv_obj_t *v = label(row, UI_FONT_SMALL, s_kind_color[kind], text);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *bar = plain(row);
        lv_obj_set_size(bar, LV_PCT(kinds[kind] * 100 / top), 6);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, ui_color(s_kind_color[kind]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 24);
        lv_obj_set_style_min_width(bar, 6, 0);
    }
    char text[200];
    if (count)
    {
        snprintf(text, sizeof(text),
                 "%d su %d usano indirizzi casuali che cambiano nel tempo, per non farsi seguire. %d trasmettono un nome.",
                 private_addr, count, named);
        ui_text(page, text, true);
        char nearest[48], line[120], distance[16];
        ombra_display_name(&ui->snapshot[0], nearest, sizeof(nearest));
        distance_text(ui->snapshot[0].distance_cm, distance, sizeof(distance));
        snprintf(line, sizeof(line), "Il più vicino: %s, circa %s.", nearest, distance);
        ui_text(page, line, false);
    }
    else
        ui_text(page, "Ancora nessun segnale.", true);
}

/* ------------------------------------------------------------ menu */

static void menu_action(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 1)
    {
        s_ui->filter = -1;
        go(s_ui, NEARBY);
    }
    else if (what == 2)
    {
        s_ui->filter = OMBRA_KIND_TRACKER;
        go(s_ui, NEARBY);
    }
    else if (what == 3)
        go(s_ui, SHIELD);
    else if (what == 4)
        go(s_ui, MIX);
    else if (what == 5)
        extapp_return_to_launcher();
}

static void sound_switch(lv_event_t *e)
{
    ombra_sound_enable(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void menu(ombra_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    ui_title(page, "Ombra");
    char text[32];
    snprintf(text, sizeof(text), "%d", ui->status.total);
    ui_row(page, LV_SYMBOL_LIST, 0x7AA7FF, "Intorno a te", text, menu_action, (void *)1);
    snprintf(text, sizeof(text), "%d", ui->status.trackers);
    ui_row(page, LV_SYMBOL_GPS, C_EMBER, "Tracker", text, menu_action, (void *)2);
    ui_row(page, LV_SYMBOL_EYE_OPEN, C_SAFE, "Scudo anti-tracker",
           ui->status.suspects ? "ALLARME" : ui->status.shield ? "attivo" : "spento", menu_action, (void *)3);
    ui_row(page, LV_SYMBOL_SHUFFLE, 0xF2C14E, "Composizione", NULL, menu_action, (void *)4);
    if (ombra_sound_available())
        ui_switch_row(page, LV_SYMBOL_VOLUME_MAX, 0x9C8CFF, "Suoni", ombra_sound_enabled(), sound_switch, NULL);
    ui_text(page, "Ascolta il Bluetooth LE intorno a te: telefoni, cuffie, orologi, tracker. Nessun collegamento, nessun dato inviato.", true);
    ui_button(page, "Esci", 0x2A2936, menu_action, (void *)5);
}

/* ------------------------------------------------------------ frame */

static void alert_answer(bool confirmed, void *user_data)
{
    if (confirmed)
    {
        s_ui->selected = (uint32_t)(uintptr_t)user_data;
        s_ui->back_to = SHIELD;
        go(s_ui, FIND);
    }
}

static void render(ombra_ui_t *ui)
{
    if (ui->view != FIND)
    {
        ombra_sound_find(0);
        if (ui->screen_kept)
        {
            power_keep_screen_on(false);
            ui->screen_kept = false;
        }
    }
    if (ui->content)
        lv_obj_delete(ui->content);
    memset(ui->cards, 0, sizeof(ui->cards));
    memset(ui->ticks, 0, sizeof(ui->ticks));
    ui->d_distance = ui->d_signal = ui->d_seen = ui->d_follow = ui->chart = NULL;
    ui->list_caption = NULL;
    ui->content = plain(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    ui->page = NULL;
    ombra_status(&ui->status);
    if (ui->view == FIELD)
    {
        field(ui);
        field_update(ui);
        return;
    }
    if (ui->view == FIND)
    {
        find(ui);
        return;
    }
    ui->page = ui_page(ui->content, NULL);
    if (ui->view == MENU)
        menu(ui);
    else if (ui->view == NEARBY)
    {
        ui_title(ui->page, ui->filter == OMBRA_KIND_TRACKER ? "Tracker" : "Intorno a te");
        ui->list_caption = ui_text(ui->page, "", true);
        cards(ui);
        list_update(ui);
    }
    else if (ui->view == DETAIL)
    {
        detail(ui);
        detail_update(ui);
    }
    else if (ui->view == SHIELD)
        shield(ui);
    else
        mix(ui);
}

static void tick(lv_timer_t *timer)
{
    ombra_ui_t *ui = lv_timer_get_user_data(timer);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
        return;
    if (ui->view == FIND)
    {
        find_update(ui);
        return;
    }
    if (++ui->frames % 16 != 0) /* lists and field once a second */
        return;
    ombra_status(&ui->status);
    if (ui->status.alert_id && ui->status.alert_id != ui->shown_alert)
    {
        ui->shown_alert = ui->status.alert_id;
        ombra_device_t d;
        if (ombra_get(ui->shown_alert, &d))
        {
            char name[48], text[160], follow[16];
            ombra_display_name(&d, name, sizeof(name));
            duration_text(d.follow_ms, follow, sizeof(follow));
            snprintf(text, sizeof(text), "%s, lontano dal suo proprietario, è con te da %s. Cercalo addosso o nella borsa.",
                     name, follow);
            ui_confirm("Tracker sospetto", text, "Trova", C_EMBER, alert_answer, (void *)(uintptr_t)d.id);
        }
    }
    switch (ui->view)
    {
    case FIELD: field_update(ui); break;
    case NEARBY:
    case SHIELD: list_update(ui); break;
    case DETAIL: detail_update(ui); break;
    default: break;
    }
}

bool ombra_ui_back(void)
{
    ombra_ui_t *ui = s_ui;
    switch (ui->view)
    {
    case FIELD: extapp_return_to_launcher(); break;
    case FIND: go(ui, DETAIL); break;
    case DETAIL: go(ui, ui->back_to == SHIELD ? SHIELD : NEARBY); break;
    case MENU: go(ui, FIELD); break;
    default: go(ui, MENU); break;
    }
    return true;
}

void ombra_ui_create(lv_obj_t *screen)
{
    ombra_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ui->snapshot = heap_caps_malloc(sizeof(ombra_device_t) * SNAPSHOT, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui->snapshot == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    ui->filter = -1;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->timer = lv_timer_create(tick, 62, ui); /* 16 Hz */
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA only: "view field|menu|nearby|trackers|detail|find|shield|mix" opens a page
 * (detail and find on the nearest device), so screenshots need no taps. */
#include "core/devcon.h"
#include <stdio.h>

bool devcon_app_command(char *line)
{
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
        return false;
    static const char *const names[] = {"field", "menu", "nearby", "detail", "find", "shield", "mix", "trackers"};
    int view = -1;
    for (int i = 0; i < 8; i++)
        if (strcmp(line + 5, names[i]) == 0)
            view = i;
    if (view < 0)
    {
        printf("@ERR unknown view\n");
        return true;
    }
    lv_port_lock();
    ombra_ui_t *ui = s_ui;
    ui->filter = view == 7 ? OMBRA_KIND_TRACKER : -1;
    if (view == DETAIL || view == FIND)
    {
        if (ombra_snapshot(ui->snapshot, SNAPSHOT, -1) > 0)
            ui->selected = ui->snapshot[0].id;
        ui->back_to = NEARBY;
    }
    go(ui, view == 7 ? NEARBY : view);
    lv_port_unlock();
    printf("@OK view %s\n", names[view]);
    return true;
}
#endif
