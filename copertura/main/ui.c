/*
 * Copertura on the watch.
 *
 *   LIVE     the signal now: an arc in the grade's colour, dBm, latency and
 *            loss to the router, the last minute as a line.
 *   ROOMS    pick the room you are standing in -> MEASURE (10 s) -> result:
 *            the grade in a big coloured circle and what it means.
 *   FINDER   choose: strongest spot or repeater spot; then the whole screen
 *            turns from blue (cold) to red (hot), an arrow says better/worse,
 *            the speaker ticks faster as you get closer.
 *   RESULTS  every room, latest grade first worst; ADVICE: repeater, channel.
 *   MENU / DASH.
 */
#include "copertura.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "core/power.h"
#if !CONFIG_APPKIT_HOSTED
#include "extapp_sdk.h"
#endif
#include "hardware/board.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG     0x060B12
#define C_CARD   0x111A26
#define C_LINE   0x1F2C3D
#define C_INK    0xEEF4FB
#define C_DIM    0x8B9AB0
#define C_ACCENT 0x34C6F4

enum { LIVE, ROOMS, MEASURE, FINDER_PICK, FINDER, RESULTS, ADVICE, MENU, DASH };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_timer_t *timer;
    int view;
    uint32_t revision;
    uint8_t floor;
    /* live widgets */
    lv_obj_t *arc;
    lv_obj_t *dbm;
    lv_obj_t *word;
    lv_obj_t *net;
    lv_obj_t *ping;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    /* measure */
    lv_obj_t *progress;
    lv_obj_t *progress_text;
    cp_phase_t shown_phase;
    /* finder */
    lv_obj_t *heat_ring;
    lv_obj_t *heat_word;
    lv_obj_t *heat_arrow;
    lv_obj_t *heat_rssi;
} cp_ui_t;

static cp_ui_t *s_ui;
static void render(cp_ui_t *ui);

static uint32_t grade_color(cp_grade_t g)
{
    static const uint32_t colors[] = {0x2EE39A, 0x9BE15D, 0xFFD166, 0xFFA34D, 0xFF6B6B, 0xC2185B};
    return colors[g <= CP_F ? g : CP_F];
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static void go(cp_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

static void open_view(lv_event_t *e)
{
    go(s_ui, (int)(intptr_t)lv_event_get_user_data(e));
}

static int width_of(void)
{
    const board_info_t *b = board_info();
    return b->round ? b->width * 72 / 100 : b->width - 24;
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, uint32_t color, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, UI_FONT_LARGE, color, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    return b;
}

/* ------------------------------------------------------------------ LIVE */

static void live_update(cp_ui_t *ui)
{
    cp_live_t l;
    cp_live(&l);
    if (!l.connected)
    {
        lv_label_set_text(ui->dbm, "--");
        lv_label_set_text(ui->word, "Connessione al Wi-Fi...");
        lv_arc_set_value(ui->arc, 0);
        return;
    }
    cp_grade_t g = cp_grade(l.rssi, l.ping_avg, l.loss);
    uint32_t color = grade_color(g);
    lv_arc_set_value(ui->arc, cp_signal_percent(l.rssi));
    lv_obj_set_style_arc_color(ui->arc, ui_color(color), LV_PART_INDICATOR);
    lv_label_set_text_fmt(ui->dbm, "%d", l.rssi);
    lv_label_set_text(ui->word, cp_grade_text(g));
    lv_obj_set_style_text_color(ui->word, ui_color(color), 0);
    if (l.ping >= 0)
    {
        lv_label_set_text_fmt(ui->ping, "%d ms  ·  %d%% persi", l.ping_avg, l.loss);
    }
    else
    {
        lv_label_set_text_fmt(ui->ping, "router muto  ·  %d%% persi", l.loss);
    }
    lv_label_set_text_fmt(ui->net, "canale %d · %s", l.channel, l.phy);
    if (ui->chart != NULL)
    {
        int8_t rssi[120];
        int16_t ping[120];
        int n = cp_history(rssi, ping, 120);
        lv_chart_set_point_count(ui->chart, 120);
        for (int i = 0; i < 120; i++)
        {
            int index = i - (120 - n);
            lv_chart_set_value_by_id(ui->chart, ui->series, i, index >= 0 ? rssi[index] : LV_CHART_POINT_NONE);
        }
        lv_obj_set_style_line_color(ui->chart, ui_color(color), LV_PART_ITEMS);
        lv_chart_refresh(ui->chart);
    }
}

static void live(cp_ui_t *ui)
{
    const board_info_t *b = board_info();
    int size = b->round ? (b->width < b->height ? b->width : b->height) - 20 : b->width - 70;
    ui->arc = lv_arc_create(ui->content);
    lv_obj_remove_style_all(ui->arc);
    lv_obj_set_size(ui->arc, size, size);
    lv_obj_align(ui->arc, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 6);
    lv_arc_set_rotation(ui->arc, 135);
    lv_arc_set_bg_angles(ui->arc, 0, 270);
    lv_arc_set_range(ui->arc, 0, 100);
    int width = size / 16;
    lv_obj_set_style_arc_width(ui->arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->arc, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ui->arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->arc, true, LV_PART_INDICATOR);
    lv_obj_set_clickable(ui->arc, false);

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, size * 70 / 100, LV_SIZE_CONTENT);
    lv_obj_align_to(col, ui->arc, LV_ALIGN_CENTER, 0, -size / 16);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *row = box(col);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    ui->dbm = label(row, ui_font_big, C_INK, "--");
    lv_obj_t *unit = label(row, UI_FONT_BODY, C_DIM, " dBm");
    lv_obj_set_style_pad_bottom(unit, 10, 0);
    ui->word = label(col, UI_FONT_BODY, C_DIM, "");
    lv_label_set_long_mode(ui->word, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui->word, size * 64 / 100);
    lv_obj_set_style_text_align(ui->word, LV_TEXT_ALIGN_CENTER, 0);
    ui->ping = label(col, UI_FONT_SMALL, C_DIM, "");
    ui->net = label(col, UI_FONT_SMALL, 0x5C6B80, "");
    lv_label_set_long_mode(ui->net, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(ui->net, size * 64 / 100, 0);

    /* Last minute of signal: on the rectangle under the gauge, on round inside it. */
    ui->chart = lv_chart_create(ui->content);
    lv_obj_set_size(ui->chart, b->round ? size * 46 / 100 : b->width - 40, b->round ? 34 : 46);
    if (b->round)
    {
        lv_obj_align_to(ui->chart, ui->arc, LV_ALIGN_BOTTOM_MID, 0, -size / 7);
    }
    else
    {
        lv_obj_align_to(ui->chart, ui->arc, LV_ALIGN_OUT_BOTTOM_MID, 0, -size / 7);
    }
    lv_chart_set_type(ui->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(ui->chart, LV_CHART_AXIS_PRIMARY_Y, -95, -30);
    lv_obj_set_style_bg_opa(ui->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->chart, 0, 0);
    lv_chart_set_div_line_count(ui->chart, 0, 0);
    lv_obj_set_style_size(ui->chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ui->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(ui->chart, 0, 0);
    ui->series = lv_chart_add_series(ui->chart, ui_color(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *buttons = box(ui->content);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    round_button(buttons, LV_SYMBOL_PLUS, 0x2EE39A, open_view, (void *)(intptr_t)ROOMS);
    round_button(buttons, LV_SYMBOL_GPS, 0xFF8A3D, open_view, (void *)(intptr_t)FINDER_PICK);
    round_button(buttons, LV_SYMBOL_LIST, C_ACCENT, open_view, (void *)(intptr_t)RESULTS);
    round_button(buttons, LV_SYMBOL_SETTINGS, C_DIM, open_view, (void *)(intptr_t)MENU);
    if (b->round)
    {
        lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    else
    {
        lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
    live_update(ui);
}

/* ----------------------------------------------------------------- ROOMS */

static void pick_room(lv_event_t *e)
{
    const char *room = lv_label_get_text(lv_obj_get_child(lv_event_get_target(e), 0));
    if (cp_measure(room, s_ui->floor))
    {
        go(s_ui, MEASURE);
    }
    else
    {
        ui_toast("Wi-Fi non collegato");
    }
}

static void cycle_floor(lv_event_t *e)
{
    (void)e;
    char floors[CP_MAX_FLOORS][CP_ROOM];
    int n = cp_floors(floors, CP_MAX_FLOORS);
    s_ui->floor = (uint8_t)((s_ui->floor + 1) % (n > 0 ? n : 1));
    render(s_ui);
}

static void rooms(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dove sei?");
    char floors[CP_MAX_FLOORS][CP_ROOM];
    int nf = cp_floors(floors, CP_MAX_FLOORS);
    if (ui->floor >= nf)
    {
        ui->floor = 0;
    }
    if (nf > 1)
    {
        char text[48];
        snprintf(text, sizeof(text), LV_SYMBOL_LOOP "  %s", floors[ui->floor]);
        lv_obj_t *f = ui_button(page, text, 0x2A3A50, cycle_floor, NULL);
        lv_port_set_focusable(f);
    }
    ui_text(page, "Stai al centro della stanza, con l'orologio al polso come sempre: misuro 10 secondi.", true);
    char list[CP_MAX_ROOMS][CP_ROOM];
    int n = cp_rooms(list, CP_MAX_ROOMS);
    int width = width_of();
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *b = lv_button_create(page);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, width, 52);
        lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_t *l = label(b, UI_FONT_BODY, C_INK, list[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, pick_room, LV_EVENT_CLICKED, NULL);
        lv_port_set_focusable(b);
    }
}

/* --------------------------------------------------------------- MEASURE */

static void result_card(cp_ui_t *ui, const cp_point_t *p)
{
    const board_info_t *b = board_info();
    lv_obj_t *page = ui_page(ui->content, p->room);
    cp_grade_t g = cp_grade(p->rssi, p->ping, p->loss);
    uint32_t color = grade_color(g);
    int size = (b->width < b->height ? b->width : b->height) * 36 / 100;
    lv_obj_t *badge = box(page);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, ui_color(color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(badge, 30, 0);
    lv_obj_set_style_shadow_color(badge, ui_color(color), 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_40, 0);
    lv_obj_t *letter = label(badge, ui_font_big, C_BG, cp_grade_letter(g));
    lv_obj_center(letter);
    lv_obj_t *t = label(page, UI_FONT_LARGE, color, cp_grade_text(g));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, width_of());
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    char text[160];
    snprintf(text, sizeof(text), "%d dBm (da %d a %d)\n%s%u ms dal router · %u%% persi", p->rssi, p->rssi_min, p->rssi_max,
             p->ping ? "" : "router muto · ", p->ping, p->loss);
    lv_obj_t *d = ui_text(page, text, false);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    if (p->radios > 1)
    {
        snprintf(text, sizeof(text), "Rete mesh/ripetitori: %u radio%s", p->radios,
                 p->alt_rssi && p->alt_rssi > p->rssi + 6 ? ". Un'altra radio qui è più forte: l'orologio resta su quella più debole." : ".");
        lv_obj_t *m = ui_text(page, text, true);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *again = ui_button(page, LV_SYMBOL_PLUS "  Un'altra stanza", 0x2A3A50, open_view, (void *)(intptr_t)ROOMS);
    lv_port_set_focusable(again);
    lv_obj_t *all = ui_button(page, "Tutti i risultati", C_ACCENT, open_view, (void *)(intptr_t)RESULTS);
    lv_port_set_focusable(all);
}

static void measure_update(cp_ui_t *ui)
{
    cp_measure_t m;
    cp_measure_status(&m);
    if (m.phase != ui->shown_phase)
    {
        render(ui);
        return;
    }
    if (ui->progress != NULL)
    {
        lv_arc_set_value(ui->progress, m.percent);
        cp_live_t l;
        cp_live(&l);
        lv_label_set_text_fmt(ui->progress_text, "%s\n%d dBm", m.phase == CP_SCANNING ? "Ascolto le reti..." : "Misuro...",
                              l.rssi);
    }
}

static void measure(cp_ui_t *ui)
{
    cp_measure_t m;
    cp_measure_status(&m);
    ui->shown_phase = m.phase;
    ui->progress = NULL;
    if (m.phase == CP_IDLE)
    {
        /* Done: show the spot just saved. */
        cp_point_t p;
        bool found = false;
        cp_lock();
        int count = 0;
        const cp_point_t *points = cp_points(&count);
        for (int i = 0; i < count; i++)
        {
            if (points[i].id == m.last_point)
            {
                p = points[i];
                found = true;
            }
        }
        cp_unlock();
        if (found)
        {
            result_card(ui, &p);
        }
        else
        {
            go(ui, LIVE);
        }
        return;
    }
    const board_info_t *b = board_info();
    int size = (b->width < b->height ? b->width : b->height) - 60;
    ui->progress = lv_arc_create(ui->content);
    lv_obj_remove_style_all(ui->progress);
    lv_obj_set_size(ui->progress, size, size);
    lv_obj_center(ui->progress);
    lv_arc_set_rotation(ui->progress, 270);
    lv_arc_set_bg_angles(ui->progress, 0, 360);
    lv_arc_set_range(ui->progress, 0, 100);
    lv_obj_set_style_arc_width(ui->progress, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->progress, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->progress, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->progress, ui_color(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->progress, true, LV_PART_INDICATOR);
    lv_obj_set_clickable(ui->progress, false);
    lv_obj_t *room = label(ui->content, ui_font_title, C_INK, m.room);
    lv_obj_align(room, LV_ALIGN_CENTER, 0, -30);
    ui->progress_text = label(ui->content, UI_FONT_BODY, C_DIM, "");
    lv_obj_set_style_text_align(ui->progress_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui->progress_text, LV_ALIGN_CENTER, 0, 24);
    measure_update(ui);
}

/* ---------------------------------------------------------------- FINDER */

static void finder_go(lv_event_t *e)
{
    cp_finder_start((int)(intptr_t)lv_event_get_user_data(e));
    go(s_ui, FINDER);
}

static void finder_pick(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Cerca");
    lv_obj_t *a = ui_button(page, LV_SYMBOL_UP "  Segnale più forte", 0x2EE39A, finder_go, (void *)(intptr_t)0);
    lv_port_set_focusable(a);
    ui_text(page, "Dove mettere il PC, la TV o la console: girando, l'orologio ticchetta più veloce dove il segnale sale.", true);
    lv_obj_t *r = ui_button(page, LV_SYMBOL_SHUFFLE "  Posto per il ripetitore", 0xFF8A3D, finder_go, (void *)(intptr_t)1);
    lv_port_set_focusable(r);
    ui_text(page, "Un ripetitore rende quando riceve ancora bene il router (circa -62 dBm) ma è il più lontano possibile: "
                  "parti dal router e vai verso le stanze scoperte finché senti il doppio tono.", true);
}

static uint32_t heat_color(int heat)
{
    /* blue -> violet -> orange -> red */
    static const uint32_t stops[] = {0x1D4ED8, 0x7C3AED, 0xF97316, 0xEF4444};
    int segment = heat >= 100 ? 2 : heat * 3 / 100;
    int t = heat >= 100 ? 100 : (heat * 3) % 100;
    uint32_t a = stops[segment], b = stops[segment + 1];
    int r = (int)((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t / 100);
    int g = (int)((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t / 100);
    int bl = (int)(a & 255) + (((int)(b & 255) - (int)(a & 255)) * t / 100);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static void finder_stop_cb(lv_event_t *e)
{
    (void)e;
    cp_finder_stop();
    go(s_ui, LIVE);
}

static void finder_update(cp_ui_t *ui)
{
    if (ui->heat_ring == NULL)
    {
        return;
    }
    cp_finder_t f;
    cp_finder_state(&f);
    uint32_t color = heat_color(f.heat);
    lv_obj_set_style_bg_color(ui->content, ui_color(color), 0);
    lv_obj_set_style_bg_grad_color(ui->content, ui_color(C_BG), 0);
    lv_arc_set_value(ui->heat_ring, f.heat);
    bool here = f.mode == 1 ? f.heat >= 85 : f.heat >= 95;
    lv_label_set_text(ui->heat_word, here ? (f.mode == 1 ? "Qui!" : "Il punto migliore") :
                      f.heat >= 70 ? "Caldo" : f.heat >= 40 ? "Tiepido" : "Freddo");
    lv_label_set_text(ui->heat_arrow, f.trend > 0 ? LV_SYMBOL_UP : f.trend < 0 ? LV_SYMBOL_DOWN : LV_SYMBOL_MINUS);
    if (f.mode == 1)
    {
        lv_label_set_text_fmt(ui->heat_rssi, "%d dBm · obiettivo -62", f.rssi);
    }
    else
    {
        lv_label_set_text_fmt(ui->heat_rssi, "%d dBm · migliore %d", f.rssi, f.best);
    }
}

static void finder(cp_ui_t *ui)
{
    const board_info_t *b = board_info();
    lv_obj_set_style_bg_grad_dir(ui->content, LV_GRAD_DIR_VER, 0);
    int size = (b->width < b->height ? b->width : b->height) - 30;
    ui->heat_ring = lv_arc_create(ui->content);
    lv_obj_remove_style_all(ui->heat_ring);
    lv_obj_set_size(ui->heat_ring, size, size);
    lv_obj_align(ui->heat_ring, LV_ALIGN_CENTER, 0, b->round ? 0 : -24);
    lv_arc_set_rotation(ui->heat_ring, 135);
    lv_arc_set_bg_angles(ui->heat_ring, 0, 270);
    lv_arc_set_range(ui->heat_ring, 0, 100);
    lv_obj_set_style_arc_width(ui->heat_ring, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->heat_ring, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ui->heat_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->heat_ring, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->heat_ring, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->heat_ring, true, LV_PART_INDICATOR);
    lv_obj_set_clickable(ui->heat_ring, false);
    /* Fixed-width, centred labels: their text changes every 250 ms. */
    lv_obj_t *labels[3];
    ui->heat_arrow = labels[0] = label(ui->content, &lv_font_montserrat_48, C_INK, LV_SYMBOL_MINUS);
    ui->heat_word = labels[1] = label(ui->content, ui_font_title, C_INK, "");
    ui->heat_rssi = labels[2] = label(ui->content, UI_FONT_SMALL, C_INK, "");
    static const int8_t offsets[] = {-7, 16, 6};
    for (int i = 0; i < 3; i++)
    {
        lv_obj_set_width(labels[i], size * 80 / 100);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(labels[i], ui->heat_ring, LV_ALIGN_CENTER, 0, offsets[i] < 0 ? size / offsets[i] : size / offsets[i]);
    }
    lv_obj_t *stop = round_button(ui->content, LV_SYMBOL_CLOSE, C_INK, finder_stop_cb, NULL);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, b->round ? -12 : -10);
    finder_update(ui);
}

/* --------------------------------------------------------------- RESULTS */

typedef struct
{
    char room[CP_ROOM];
    cp_point_t latest;
    int spots;
} room_t;

static int collect_rooms(room_t *out, int max)
{
    int n = 0;
    cp_lock();
    int count = 0;
    const cp_point_t *points = cp_points(&count);
    for (int i = 0; i < count; i++)
    {
        int r = 0;
        while (r < n && strcmp(out[r].room, points[i].room) != 0)
        {
            r++;
        }
        if (r == n)
        {
            if (n == max)
            {
                continue;
            }
            n++;
            snprintf(out[r].room, sizeof(out[r].room), "%s", points[i].room);
            out[r].spots = 0;
        }
        out[r].latest = points[i];   /* points are in time order: the last wins */
        out[r].spots++;
    }
    cp_unlock();
    /* Worst first: that is what needs attention. */
    for (int i = 1; i < n; i++)
    {
        room_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].latest.rssi > key.latest.rssi)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

static void results(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Risultati");
    room_t *list = heap_caps_malloc(sizeof(room_t) * CP_MAX_ROOMS * 2, MALLOC_CAP_SPIRAM);
    if (list == NULL)
    {
        return;
    }
    int n = collect_rooms(list, CP_MAX_ROOMS * 2);
    int width = width_of();
    for (int i = 0; i < n; i++)
    {
        const cp_point_t *p = &list[i].latest;
        cp_grade_t g = cp_grade(p->rssi, p->ping, p->loss);
        lv_obj_t *card = box(page);
        lv_obj_set_size(card, width, 60);
        lv_obj_set_style_bg_color(card, ui_color(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_t *badge = box(card);
        lv_obj_set_size(badge, 40, 40);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, ui_color(grade_color(g)), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_t *letter = label(badge, UI_FONT_LARGE, C_BG, cp_grade_letter(g));
        lv_obj_center(letter);
        lv_obj_t *name = label(card, UI_FONT_BODY, C_INK, list[i].room);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 62, 8);
        char text[64];
        snprintf(text, sizeof(text), "%d dBm · %u ms%s", p->rssi, p->ping, p->loss ? " · perdite" : "");
        lv_obj_t *sub = label(card, UI_FONT_SMALL, C_DIM, text);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 62, 34);
    }
    if (n == 0)
    {
        ui_text(page, "Nessuna stanza misurata. Tocca + e scegli dove sei.", true);
        free(list);
        return;
    }
    lv_obj_t *adv = ui_button(page, LV_SYMBOL_EYE_OPEN "  Consigli", C_ACCENT, open_view, (void *)(intptr_t)ADVICE);
    lv_port_set_focusable(adv);
    free(list);
}

static void advice(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Consigli");
    room_t *list = heap_caps_malloc(sizeof(room_t) * CP_MAX_ROOMS * 2, MALLOC_CAP_SPIRAM);
    cp_spot_t *spots = heap_caps_malloc(sizeof(cp_spot_t) * CP_MAX_ROOMS * 2, MALLOC_CAP_SPIRAM);
    if (list == NULL || spots == NULL)
    {
        free(list);
        free(spots);
        return;
    }
    int n = collect_rooms(list, CP_MAX_ROOMS * 2);
    int weak = 0;
    char text[200];
    for (int i = 0; i < n; i++)
    {
        spots[i].rssi = list[i].latest.rssi;
        spots[i].weak = list[i].latest.rssi < -72 || list[i].latest.loss >= 10;
        weak += spots[i].weak;
    }
    ui_section(page, "RIPETITORE");
    int best = cp_repeater_spot(spots, n);
    if (n == 0)
    {
        ui_text(page, "Misura almeno le stanze principali.", true);
    }
    else if (weak == 0)
    {
        ui_text(page, "Tutte le stanze misurate sono coperte bene: un ripetitore non serve.", false);
    }
    else if (best >= 0)
    {
        snprintf(text, sizeof(text), "%s (%d dBm): riceve ancora bene il router ed è verso le stanze scoperte. "
                 "Mettilo lì, a mezza altezza, lontano da specchi e acquari.", list[best].room, list[best].latest.rssi);
        ui_text(page, text, false);
    }
    else
    {
        ui_text(page, "Nessuna stanza misurata è adatta: misura i punti a metà strada tra router e zone deboli, "
                      "o usa Cerca > Posto per il ripetitore.", false);
    }
    ui_section(page, "CANALE");
    cp_neighbour_t *neighbours = heap_caps_malloc(sizeof(cp_neighbour_t) * CP_MAX_APS, MALLOC_CAP_SPIRAM);
    cp_live_t l;
    cp_live(&l);
    int m = 0, ours = 0;
    cp_lock();
    int count = 0;
    const cp_ap_t *aps = cp_aps(&count, NULL);
    for (int i = 0; i < count && neighbours != NULL; i++)
    {
        if (aps[i].ours)
        {
            ours++;
            continue;
        }
        neighbours[m].channel = aps[i].channel;
        neighbours[m].rssi = aps[i].rssi;
        m++;
    }
    cp_unlock();
    if (neighbours != NULL)
    {
        cp_channels_t c;
        cp_channel_advice(neighbours, m, l.channel, &c);
        if (c.worth_changing)
        {
            snprintf(text, sizeof(text), "Sul canale %d ci sono vicini forti (affollamento %d). Nelle impostazioni del "
                     "router scegli il %d (affollamento %d).", c.current, c.current_score, c.best, c.best_score);
        }
        else
        {
            snprintf(text, sizeof(text), "Il canale %d va bene: %d reti vicine, nessun canale è chiaramente più libero.",
                     c.current, m);
        }
        ui_text(page, text, false);
        free(neighbours);
    }
    if (ours > 1)
    {
        snprintf(text, sizeof(text), "La tua rete ha %d radio (mesh o ripetitori).", ours);
        ui_text(page, text, true);
    }
    ui_text(page, "Sulla dashboard: la mappa di calore sulla piantina di casa.", true);
    free(list);
    free(spots);
}

/* ------------------------------------------------------------------ MENU */

static void clear_confirmed(bool ok, void *data)
{
    (void)data;
    if (ok)
    {
        cp_points_clear();
        ui_toast("Misure cancellate");
    }
}

static void clear_all(lv_event_t *e)
{
    (void)e;
    ui_confirm("Ricominciare?", "Cancello tutte le misure (stanze e piantine restano).", "Cancella", 0xFF6B6B,
               clear_confirmed, NULL);
}

static void scan_now(lv_event_t *e)
{
    (void)e;
    cp_scan_now();
    ui_toast("Ascolto le reti...");
}

static void menu(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Copertura");
    lv_obj_t *b = ui_button(page, LV_SYMBOL_WIFI "  Dashboard", C_ACCENT, open_view, (void *)(intptr_t)DASH);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_EYE_OPEN "  Consigli", 0x2A3A50, open_view, (void *)(intptr_t)ADVICE);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_REFRESH "  Ascolta le reti", 0x2A3A50, scan_now, NULL);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_TRASH "  Cancella le misure", 0x3A2530, clear_all, NULL);
    lv_port_set_focusable(b);
    ui_text(page, "Stanze, piani e piantina si modificano dalla dashboard.", true);
}

static void dash(cp_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dashboard");
    appkit_dashboard_page(page, ui_color(C_ACCENT));
}

/* ----------------------------------------------------------------- frame */

static void render(cp_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->arc = NULL;
    ui->chart = NULL;
    ui->progress = NULL;
    ui->heat_ring = NULL;
    ui->content = box(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    ui->revision = cp_revision();
    if (ui->view != FINDER)
    {
        cp_finder_stop();
    }
    switch (ui->view)
    {
    case LIVE: live(ui); break;
    case ROOMS: rooms(ui); break;
    case MEASURE: measure(ui); break;
    case FINDER_PICK: finder_pick(ui); break;
    case FINDER: finder(ui); break;
    case RESULTS: results(ui); break;
    case ADVICE: advice(ui); break;
    case MENU: menu(ui); break;
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    cp_ui_t *ui = lv_timer_get_user_data(timer);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
    {
        return;
    }
    switch (ui->view)
    {
    case LIVE: live_update(ui); break;
    case MEASURE: measure_update(ui); break;
    case FINDER: finder_update(ui); break;
    default: break;
    }
}

bool copertura_ui_back(void)
{
    cp_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return false;
    }
    if (ui->view == LIVE)
    {
        cp_finder_stop();
#if CONFIG_APPKIT_HOSTED
        return false;   /* internal app: the app manager closes it */
#else
        extapp_return_to_launcher();
#endif
    }
    else
    {
        go(ui, ui->view == ADVICE ? RESULTS : ui->view == DASH ? MENU : LIVE);
    }
    return true;
}

void copertura_ui_create(lv_obj_t *screen)
{
    cp_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->timer = lv_timer_create(tick, 250, ui);
}

void copertura_ui_destroy(void)
{
    cp_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return;
    }
    s_ui = NULL;
    cp_finder_stop();
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}

#if CONFIG_WATCH_DEV_CONSOLE && !CONFIG_APPKIT_HOSTED
/* QA only: "view live|rooms|measure|pick|finder|results|advice|menu|dash", "measure <room>", "code". */
#include "core/devcon.h"

bool devcon_app_command(char *line)
{
    static const char *const views[] = {"live", "rooms", "measure", "pick", "finder", "results", "advice", "menu", "dash"};
    if (strcmp(line, "code") == 0)
    {
        printf("@CODE %s\n", appkit_web_code());
        return true;
    }
    if (strncmp(line, "measure ", 8) == 0)
    {
        bool ok = cp_measure(line + 8, 0);
        printf(ok ? "@OK measure\n" : "@ERR measure\n");
        if (ok && s_ui != NULL)
        {
            lv_port_lock();
            go(s_ui, MEASURE);
            lv_port_unlock();
        }
        return true;
    }
    if (strncmp(line, "finder ", 7) == 0)
    {
        cp_finder_start(atoi(line + 7));
        lv_port_lock();
        go(s_ui, FINDER);
        lv_port_unlock();
        printf("@OK finder\n");
        return true;
    }
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
    {
        return false;
    }
    for (int v = 0; v < 9; v++)
    {
        if (strcmp(line + 5, views[v]) == 0)
        {
            lv_port_lock();
            go(s_ui, v);
            lv_port_unlock();
            printf("@OK view %s\n", views[v]);
            return true;
        }
    }
    printf("@ERR unknown view\n");
    return true;
}
#endif
