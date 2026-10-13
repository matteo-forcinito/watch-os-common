/*
 * Cielo UI:
 *
 *   SKY     disc centred on you, north up: every aircraft is an arrow pointing
 *           where it flies, coloured by altitude, with its recent trail. Between
 *           reports (12 s) positions advance with speed and track, 10 times a
 *           second: aircraft glide instead of jumping. Below (or over the lower
 *           part on round panels): "look up" - the aircraft highest on your
 *           horizon, its route and where to look.
 *   LIST    flights nearest first, radius, exit
 *   FLIGHT  one flight: route, aircraft, numbers, and a dome seen from below
 *           with the point of the sky where it is right now
 */
#include "cielo.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SKY, LIST, FLIGHT };

#define C_BG 0x03060D
#define C_DISC 0x07101F
#define C_RING 0x15233A
#define C_INK 0xEEF3FA
#define C_MIST 0x8797B0
#define C_GOLD 0xFFC86B
#define C_RED 0xFF4D4D

#define DRAWN 40
#define CARDS 20
#define TRAIL_POINTS (CIELO_TRAIL + 1)

typedef struct
{
    lv_obj_t *arrow, *trail, *tag;
    lv_point_precise_t arrow_points[5], trail_points[TRAIL_POINTS];
    char hex[8];
} plane_t;

typedef struct
{
    lv_obj_t *card, *bar, *title, *subtitle, *distance, *altitude;
    char hex[8];
} card_t;

typedef struct
{
    lv_obj_t *screen, *content, *page;
    lv_timer_t *timer;
    int view, frames, back_to;
    char selected[8];
    /* sky */
    int cx, cy, radius;
    lv_obj_t *disc, *status_label, *hero_kicker, *hero_title, *hero_line, *hero_look, *iss;
    lv_obj_t *ring_labels[2];
    plane_t planes[DRAWN];
    /* list */
    card_t cards[CARDS];
    lv_obj_t *list_caption, *radius_row;
    /* flight */
    lv_obj_t *f_route, *f_cities, *f_numbers, *f_look, *f_dot, *f_dome, *f_airline, *f_type;
    int dome_r;
    cielo_aircraft_t *aircraft;
    int count;
    cielo_status_t status;
} cielo_ui_t;

static cielo_ui_t *s_ui;
static void render(cielo_ui_t *ui);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------ helpers */

static uint32_t altitude_color(int32_t m)
{
    if (m == INT32_MIN)
        return 0x6B7280; /* on the ground */
    if (m < 1000)
        return 0xFF9F5A;
    if (m < 3000)
        return C_GOLD;
    if (m < 6000)
        return 0x9FE3C1;
    if (m < 9000)
        return 0x7CC4FF;
    return 0xB59CFF;
}

static const char *compass_word(float degrees)
{
    static const char *const words[] = {"nord", "nord-est", "est", "sud-est", "sud", "sud-ovest", "ovest", "nord-ovest"};
    int index = (int)floorf(fmodf(degrees + 360.0f + 22.5f, 360.0f) / 45.0f);
    return words[index & 7];
}

/* Elevation above your horizon, with the Earth's curvature (it matters at 100 km). */
static float elevation_deg(int32_t altitude_m, float ground_km)
{
    if (altitude_m == INT32_MIN)
        return -90;
    float d = ground_km * 1000.0f;
    float drop = d * d / (2.0f * 6371000.0f);
    return atanf((altitude_m - drop) / (d < 1 ? 1 : d)) * 180.0f / (float)M_PI;
}

static void thousands(int32_t value, char *out, size_t size)
{
    if (value >= 1000 || value <= -1000)
        snprintf(out, size, "%ld.%03ld", (long)(value / 1000), (long)labs(value % 1000));
    else
        snprintf(out, size, "%ld", (long)value);
}

static void callsign_text(const cielo_aircraft_t *a, char *out, size_t size)
{
    snprintf(out, size, "%s", a->callsign[0] ? a->callsign : a->registration[0] ? a->registration : a->hex);
}

static void route_text(const cielo_aircraft_t *a, char *out, size_t size, bool cities)
{
    cielo_route_t route;
    out[0] = 0;
    if (!cielo_route(a->callsign, &route))
        return;
    if (cities && route.from_city[0] && route.to_city[0])
        snprintf(out, size, "%s " LV_SYMBOL_RIGHT " %s", route.from_city, route.to_city);
    else
        snprintf(out, size, "%s " LV_SYMBOL_RIGHT " %s", route.from_code, route.to_code);
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

static void go(cielo_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

static void open_flight(cielo_ui_t *ui, const char *hex)
{
    snprintf(ui->selected, sizeof(ui->selected), "%s", hex);
    ui->back_to = ui->view;
    go(ui, FLIGHT);
}

/* ------------------------------------------------------------ sky */

static void sky_pressed(lv_event_t *e)
{
    cielo_ui_t *ui = s_ui;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE)
    {
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP)
        {
            lv_indev_wait_release(input);
            go(ui, LIST);
        }
        return;
    }
    /* Tap: the aircraft closest to the finger, else the list. */
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int best = -1, best_d = 40 * 40;
    for (int i = 0; i < DRAWN; i++)
    {
        plane_t *p = &ui->planes[i];
        if (!p->hex[0] || lv_obj_is_hidden(p->arrow))
            continue;
        int dx = (int)p->arrow_points[0].x - point.x, dy = (int)p->arrow_points[0].y - point.y;
        if (dx * dx + dy * dy < best_d)
        {
            best_d = dx * dx + dy * dy;
            best = i;
        }
    }
    if (best >= 0)
        open_flight(ui, ui->planes[best].hex);
    else
        go(ui, LIST);
}

static void hero_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui->selected[0])
        open_flight(s_ui, s_ui->selected);
}

static lv_obj_t *circle(lv_obj_t *parent, int cx, int cy, int r, uint32_t border, int width)
{
    lv_obj_t *c = plain(parent);
    lv_obj_set_size(c, r * 2, r * 2);
    lv_obj_set_pos(c, cx - r, cy - r);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(c, ui_color(border), 0);
    lv_obj_set_style_border_width(c, width, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    return c;
}

static void sky(cielo_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height;
    lv_obj_t *c = ui->content;
    if (b->round)
    {
        ui->radius = w / 2 - 6;
        ui->cx = w / 2;
        ui->cy = h / 2;
    }
    else
    {
        const int status_h = 26, card_h = 118;
        int by_height = (h - status_h - card_h - 12) / 2;
        ui->radius = w / 2 - 10 < by_height ? w / 2 - 10 : by_height;
        ui->cx = w / 2;
        ui->cy = status_h + ui->radius;
    }
    ui->disc = circle(c, ui->cx, ui->cy, ui->radius, C_RING, 1);
    lv_obj_set_style_bg_color(ui->disc, ui_color(C_DISC), 0);
    lv_obj_set_style_bg_opa(ui->disc, LV_OPA_COVER, 0);
    for (int i = 1; i <= 2; i++)
    {
        int r = ui->radius * i / 3;
        circle(c, ui->cx, ui->cy, r, C_RING, 1);
        ui->ring_labels[i - 1] = label(c, UI_FONT_SMALL, 0x3C5070, "");
        lv_obj_set_pos(ui->ring_labels[i - 1], ui->cx + (int)(r * 0.72f) + 3, ui->cy - (int)(r * 0.72f) - 18);
    }
    lv_obj_t *north = label(c, UI_FONT_SMALL, C_MIST, "N");
    lv_obj_set_pos(north, ui->cx - 6, ui->cy - ui->radius + 6);
    /* You. */
    lv_obj_t *halo = circle(c, ui->cx, ui->cy, 9, 0x2B4A78, 1);
    (void)halo;
    lv_obj_t *me = plain(c);
    lv_obj_set_size(me, 6, 6);
    lv_obj_set_pos(me, ui->cx - 3, ui->cy - 3);
    lv_obj_set_style_radius(me, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(me, ui_color(C_INK), 0);
    lv_obj_set_style_bg_opa(me, LV_OPA_COVER, 0);

    for (int i = 0; i < DRAWN; i++)
    {
        plane_t *p = &ui->planes[i];
        p->trail = lv_line_create(c);
        lv_obj_set_style_line_width(p->trail, 2, 0);
        lv_obj_set_style_line_opa(p->trail, 70, 0);
        lv_obj_set_style_line_rounded(p->trail, true, 0);
        lv_obj_set_hidden(p->trail, true);
        p->arrow = lv_line_create(c);
        lv_obj_set_style_line_width(p->arrow, 2, 0);
        lv_obj_set_style_line_rounded(p->arrow, true, 0);
        lv_obj_set_hidden(p->arrow, true);
        p->tag = label(c, UI_FONT_SMALL, C_MIST, "");
        lv_obj_set_hidden(p->tag, true);
        p->hex[0] = 0;
    }
    ui->iss = label(c, UI_FONT_SMALL, 0xFFFFFF, "ISS");
    lv_obj_set_hidden(ui->iss, true);

    /* Look-up card. */
    lv_obj_t *hero = plain(c);
    int hero_y = b->round ? h * 62 / 100 : ui->cy + ui->radius + 8;
    lv_obj_set_size(hero, b->round ? w * 64 / 100 : w - 24, LV_SIZE_CONTENT);
    lv_obj_set_pos(hero, b->round ? w * 18 / 100 : 12, hero_y);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hero, 8, 0);
    lv_obj_set_style_pad_row(hero, 1, 0);
    lv_obj_set_style_radius(hero, 18, 0);
    lv_obj_set_style_bg_color(hero, ui_color(0x0B1527), 0);
    lv_obj_set_style_bg_opa(hero, b->round ? 215 : LV_OPA_COVER, 0);
    lv_obj_set_clickable(hero, true);
    lv_obj_add_event_cb(hero, hero_clicked, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(hero);
    ui->hero_kicker = label(hero, UI_FONT_SMALL, C_GOLD, "ALZA LO SGUARDO");
    lv_obj_set_style_text_letter_space(ui->hero_kicker, 2, 0);
    ui->hero_title = label(hero, UI_FONT_LARGE, C_INK, "");
    ui->hero_line = label(hero, UI_FONT_SMALL, C_MIST, "");
    ui->hero_look = label(hero, UI_FONT_SMALL, C_INK, "");
    lv_obj_t *lines[] = {ui->hero_title, ui->hero_line, ui->hero_look};
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    {
        lv_obj_set_width(lines[i], LV_PCT(100));
        lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_DOTS);
    }
    ui->status_label = label(c, UI_FONT_SMALL, 0x4A5C7A, "");
    lv_obj_set_width(ui->status_label, w * 70 / 100);
    lv_obj_set_style_text_align(ui->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui->status_label, LV_ALIGN_TOP_MID, 0, b->round ? ui->cy - ui->radius + 30 : 4);

    lv_obj_set_clickable(c, true);
    lv_obj_set_gesture_bubble(c, false);
    lv_obj_add_event_cb(c, sky_pressed, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(c, sky_pressed, LV_EVENT_GESTURE, NULL);
    lv_port_set_focusable(c);
}

static void arrow_points(lv_point_precise_t *pts, float x, float y, float track_deg, float size)
{
    /* Chevron: nose, right wing, notch, left wing, nose. Screen y grows downward. */
    float a = track_deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
#define P(i, fx, fy) pts[i] = (lv_point_precise_t){x + (fx) * c - (fy) * s, y - ((fx) * s + (fy) * c)}
    /* In the aircraft frame: forward = (0, 1), right = (1, 0); rotate by the track. */
    P(0, 0, size);
    P(1, 0.7f * size, -0.8f * size);
    P(2, 0, -0.35f * size);
    P(3, -0.7f * size, -0.8f * size);
    P(4, 0, size);
#undef P
}

/* 10 Hz: move every aircraft along its track. Cheap: only lines are touched. */
static void sky_motion(cielo_ui_t *ui)
{
    uint32_t now = now_ms();
    float scale = ui->radius / (float)(ui->status.radius_km ? ui->status.radius_km : 60);
    for (int i = 0; i < DRAWN; i++)
    {
        plane_t *p = &ui->planes[i];
        if (!p->hex[0])
            continue;
        const cielo_aircraft_t *a = NULL;
        for (int k = 0; k < ui->count; k++)
            if (strcmp(ui->aircraft[k].hex, p->hex) == 0)
                a = &ui->aircraft[k];
        if (a == NULL)
            continue;
        float e, n;
        cielo_position_now(a, now, &e, &n);
        float x = ui->cx + e * scale, y = ui->cy - n * scale;
        float dx = x - ui->cx, dy = y - ui->cy;
        bool inside = dx * dx + dy * dy < (float)(ui->radius - 6) * (ui->radius - 6);
        lv_obj_set_hidden(p->arrow, !inside);
        lv_obj_set_hidden(p->trail, !inside || a->trail_count == 0);
        lv_obj_set_hidden(p->tag, !inside || !lv_label_get_text(p->tag)[0]);
        if (!inside)
            continue;
        arrow_points(p->arrow_points, x, y, a->track_deg, ui->radius > 150 ? 9 : 7);
        lv_line_set_points(p->arrow, p->arrow_points, 5);
        /* Trail: only its recent part inside the disc (a line never crosses the rim). */
        int count = 0;
        float limit = (float)(ui->radius - 2) * (ui->radius - 2);
        for (int t = 0; t < a->trail_count; t++)
        {
            float tx = ui->cx + a->trail_e[t] * scale, ty = ui->cy - a->trail_n[t] * scale;
            if ((tx - ui->cx) * (tx - ui->cx) + (ty - ui->cy) * (ty - ui->cy) > limit)
            {
                count = 0; /* older points beyond the rim: start after them */
                continue;
            }
            p->trail_points[count++] = (lv_point_precise_t){tx, ty};
        }
        p->trail_points[count++] = (lv_point_precise_t){x, y};
        lv_obj_set_hidden(p->trail, count < 2);
        if (count >= 2)
            lv_line_set_points(p->trail, p->trail_points, (uint32_t)count);
        lv_obj_set_pos(p->tag, (int32_t)x + 9, (int32_t)y - 8);
    }
}

/* 1 Hz: assign aircraft to drawn slots, colours, tags, look-up card, ISS. */
static void sky_update(cielo_ui_t *ui)
{
    char text[96], route[64], number[16];
    bool used[DRAWN] = {false};
    int slot_of[CIELO_AIRCRAFT];
    int count = ui->count < CIELO_AIRCRAFT ? ui->count : CIELO_AIRCRAFT;
    for (int k = 0; k < count; k++)
    {
        slot_of[k] = -1;
        for (int i = 0; i < DRAWN; i++)
            if (!used[i] && strcmp(ui->planes[i].hex, ui->aircraft[k].hex) == 0)
            {
                slot_of[k] = i;
                used[i] = true;
                break;
            }
    }
    for (int k = 0; k < count; k++)
        for (int i = 0; i < DRAWN && slot_of[k] < 0; i++)
            if (!used[i])
            {
                slot_of[k] = i;
                used[i] = true;
            }
    for (int i = 0; i < DRAWN; i++)
        if (!used[i])
        {
            ui->planes[i].hex[0] = 0;
            lv_obj_set_hidden(ui->planes[i].arrow, true);
            lv_obj_set_hidden(ui->planes[i].trail, true);
            lv_obj_set_hidden(ui->planes[i].tag, true);
        }
    /* The aircraft highest on your horizon gets the card and a tag. */
    int best = -1;
    float best_elevation = -91;
    uint32_t now = now_ms();
    for (int k = 0; k < count; k++)
    {
        float e, n;
        cielo_position_now(&ui->aircraft[k], now, &e, &n);
        float elevation = elevation_deg(ui->aircraft[k].altitude_m, sqrtf(e * e + n * n));
        if (elevation > best_elevation)
        {
            best_elevation = elevation;
            best = k;
        }
    }
    for (int k = 0; k < count; k++)
    {
        if (slot_of[k] < 0)
            continue;
        plane_t *p = &ui->planes[slot_of[k]];
        const cielo_aircraft_t *a = &ui->aircraft[k];
        snprintf(p->hex, sizeof(p->hex), "%s", a->hex);
        uint32_t color = a->emergency ? C_RED : altitude_color(a->altitude_m);
        lv_obj_set_style_line_color(p->arrow, ui_color(color), 0);
        lv_obj_set_style_line_color(p->trail, ui_color(color), 0);
        lv_obj_set_style_line_width(p->arrow, k == best ? 3 : 2, 0);
        if (k == best || a->emergency)
        {
            callsign_text(a, text, sizeof(text));
            lv_label_set_text(p->tag, text);
            lv_obj_set_style_text_color(p->tag, ui_color(k == best ? C_INK : C_RED), 0);
        }
        else
            lv_label_set_text(p->tag, "");
    }
    sky_motion(ui);

    for (int i = 0; i < 2; i++)
    {
        snprintf(text, sizeof(text), "%d km", (ui->status.radius_km ? ui->status.radius_km : 60) * (i + 1) / 3);
        lv_label_set_text(ui->ring_labels[i], text);
    }
    if (best >= 0)
    {
        const cielo_aircraft_t *a = &ui->aircraft[best];
        snprintf(ui->selected, sizeof(ui->selected), "%s", a->hex);
        float e, n;
        cielo_position_now(a, now, &e, &n);
        float km = sqrtf(e * e + n * n), bearing = atan2f(e, n) * 180.0f / (float)M_PI;
        callsign_text(a, text, sizeof(text));
        route_text(a, route, sizeof(route), true);
        if (route[0])
        {
            char line[sizeof(text) + sizeof(route) + 2];
            snprintf(line, sizeof(line), "%s  %s", text, route);
            lv_label_set_text(ui->hero_title, line);
        }
        else
            lv_label_set_text(ui->hero_title, text);
        thousands(a->altitude_m == INT32_MIN ? 0 : a->altitude_m, number, sizeof(number));
        const char *model = cielo_type_name(a->type);
        snprintf(text, sizeof(text), "%s m · %ld km/h · %s", number, (long)a->speed_kmh, model ? model : a->type);
        lv_label_set_text(ui->hero_line, text);
        if (best_elevation >= 5)
            snprintf(text, sizeof(text), "Guarda a %s, %d%s sopra l'orizzonte", compass_word(bearing), (int)(best_elevation + 0.5f), "\xC2\xB0");
        else
            snprintf(text, sizeof(text), "Basso sull'orizzonte a %s, %d km", compass_word(bearing), (int)(km + 0.5f));
        lv_label_set_text(ui->hero_look, text);
        lv_label_set_text(ui->hero_kicker, best_elevation >= 60 ? "SOPRA DI TE" : "ALZA LO SGUARDO");
    }
    else
    {
        ui->selected[0] = 0;
        lv_label_set_text(ui->hero_kicker, "CIELO");
        lv_label_set_text(ui->hero_title, ui->status.updated_ms ? "Cielo libero" : "In ascolto...");
        lv_label_set_text(ui->hero_line, ui->status.message);
        lv_label_set_text(ui->hero_look, "");
    }
    /* Space station: on the rim, in its direction, when within 2500 km. */
    bool iss = ui->status.iss_valid && ui->status.iss_km < 2500;
    lv_obj_set_hidden(ui->iss, !iss);
    if (iss)
    {
        float rad = ui->status.iss_bearing * (float)M_PI / 180.0f;
        snprintf(text, sizeof(text), "ISS %d km", (int)ui->status.iss_km);
        lv_label_set_text(ui->iss, text);
        lv_obj_update_layout(ui->iss);
        int r = ui->radius - 22;
        lv_obj_set_pos(ui->iss, ui->cx + (int)(sinf(rad) * r) - lv_obj_get_width(ui->iss) / 2,
                       ui->cy - (int)(cosf(rad) * r) - 9);
    }
    uint32_t age = ui->status.updated_ms ? (now - ui->status.updated_ms) / 1000 : 0;
    if (ui->status.updated_ms && age < 30)
        snprintf(text, sizeof(text), "%d in volo · %d km", ui->status.count, ui->status.radius_km);
    else
        snprintf(text, sizeof(text), "%s", ui->status.message);
    lv_label_set_text(ui->status_label, text);
}

/* ------------------------------------------------------------ list */

static void card_clicked(lv_event_t *e)
{
    card_t *card = lv_event_get_user_data(e);
    if (card->hex[0])
        open_flight(s_ui, card->hex);
}

static void cycle_radius(lv_event_t *e)
{
    static const int steps[] = {25, 60, 120};
    int next = steps[0];
    for (int i = 0; i < 3; i++)
        if (steps[i] == s_ui->status.radius_km)
            next = steps[(i + 1) % 3];
    cielo_set_radius(next);
    s_ui->status.radius_km = next;
    char text[16];
    snprintf(text, sizeof(text), "%d km", next);
    lv_label_set_text(ui_row_value(lv_event_get_current_target(e)), text);
}

static void leave(lv_event_t *e)
{
    (void)e;
    extapp_return_to_launcher();
}

static int by_distance(const void *a, const void *b)
{
    const cielo_aircraft_t *x = a, *y = b;
    float dx = x->east_km * x->east_km + x->north_km * x->north_km;
    float dy = y->east_km * y->east_km + y->north_km * y->north_km;
    return dx < dy ? -1 : dx > dy;
}

static void list(cielo_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    ui_title(page, "In volo");
    ui->list_caption = ui_text(page, "", true);
    for (int i = 0; i < CARDS; i++)
    {
        card_t *k = &ui->cards[i];
        k->card = lv_obj_create(page);
        lv_obj_remove_style_all(k->card);
        lv_obj_set_size(k->card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(k->card, ui_color(0x0B1527), 0);
        lv_obj_set_style_bg_color(k->card, ui_color(0x142440), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(k->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(k->card, 16, 0);
        lv_obj_set_style_pad_all(k->card, 12, 0);
        lv_obj_set_style_pad_left(k->card, 18, 0);
        lv_obj_set_scrollable(k->card, false);
        lv_obj_add_event_cb(k->card, card_clicked, LV_EVENT_CLICKED, k);
        lv_port_set_focusable(k->card);
        k->bar = plain(k->card);
        lv_obj_set_size(k->bar, 4, LV_PCT(100));
        lv_obj_set_style_radius(k->bar, 2, 0);
        lv_obj_set_style_bg_opa(k->bar, LV_OPA_COVER, 0);
        lv_obj_align(k->bar, LV_ALIGN_LEFT_MID, -10, 0);
        k->title = label(k->card, UI_FONT_BODY, C_INK, "");
        k->subtitle = label(k->card, UI_FONT_SMALL, C_MIST, "");
        lv_obj_set_width(k->subtitle, LV_PCT(64));
        lv_label_set_long_mode(k->subtitle, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(k->subtitle, LV_ALIGN_TOP_LEFT, 0, 26);
        k->distance = label(k->card, UI_FONT_BODY, C_INK, "");
        lv_obj_align(k->distance, LV_ALIGN_TOP_RIGHT, 0, 0);
        k->altitude = label(k->card, UI_FONT_SMALL, C_MIST, "");
        lv_obj_align(k->altitude, LV_ALIGN_TOP_RIGHT, 0, 26);
        lv_obj_set_hidden(k->card, true);
        k->hex[0] = 0;
    }
    char text[16];
    snprintf(text, sizeof(text), "%d km", ui->status.radius_km);
    ui->radius_row = ui_row(page, LV_SYMBOL_GPS, C_GOLD, "Raggio", text, cycle_radius, NULL);
    ui_text(page, "Dati dalla rete pubblica ADS-B (adsb.lol), rotte da adsbdb.com. La posizione è quella del Meteo.", true);
    ui_button(page, "Esci", 0x1B2638, leave, NULL);
}

static void list_update(cielo_ui_t *ui)
{
    if (lv_obj_is_scrolling(ui->page))
        return;
    for (int i = 0; i < CARDS; i++)
        if (lv_obj_has_state(ui->cards[i].card, LV_STATE_PRESSED))
            return;
    qsort(ui->aircraft, (size_t)ui->count, sizeof(cielo_aircraft_t), by_distance);
    char text[96], number[16];
    lv_label_set_text(ui->list_caption, ui->status.message);
    for (int i = 0; i < CARDS; i++)
    {
        card_t *k = &ui->cards[i];
        if (i >= ui->count)
        {
            lv_obj_set_hidden(k->card, true);
            k->hex[0] = 0;
            continue;
        }
        const cielo_aircraft_t *a = &ui->aircraft[i];
        snprintf(k->hex, sizeof(k->hex), "%s", a->hex);
        callsign_text(a, text, sizeof(text));
        lv_label_set_text(k->title, text);
        route_text(a, text, sizeof(text), true);
        if (!text[0])
        {
            const char *model = cielo_type_name(a->type);
            snprintf(text, sizeof(text), "%s", model ? model : a->type[0] ? a->type : "Velivolo");
        }
        lv_label_set_text(k->subtitle, text);
        float km = sqrtf(a->east_km * a->east_km + a->north_km * a->north_km);
        snprintf(text, sizeof(text), "%.0f km", (double)km);
        lv_label_set_text(k->distance, text);
        thousands(a->altitude_m == INT32_MIN ? 0 : a->altitude_m, number, sizeof(number));
        snprintf(text, sizeof(text), a->altitude_m == INT32_MIN ? "a terra" : "%s m", number);
        lv_label_set_text(k->altitude, text);
        lv_obj_set_style_bg_color(k->bar, ui_color(a->emergency ? C_RED : altitude_color(a->altitude_m)), 0);
        lv_obj_set_hidden(k->card, false);
    }
}

/* ------------------------------------------------------------ flight */

static void flight(cielo_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    cielo_aircraft_t a;
    if (!cielo_find(ui->selected, &a))
    {
        ui_title(page, "Fuori portata");
        ui_text(page, "Questo volo non è più nel raggio.", true);
        return;
    }
    char text[96];
    ui->f_airline = label(page, UI_FONT_SMALL, C_GOLD, "");
    lv_obj_set_style_text_letter_space(ui->f_airline, 2, 0);
    callsign_text(&a, text, sizeof(text));
    ui_title(page, text);
    ui->f_route = label(page, ui_font_headline, C_INK, "");
    ui->f_cities = label(page, UI_FONT_SMALL, C_MIST, "");
    if (a.emergency)
    {
        lv_obj_t *alert = ui_text(page, "Codice di emergenza in corso", false);
        lv_obj_set_style_text_color(alert, ui_color(C_RED), 0);
    }
    /* Dome seen from below: centre = straight up, rim = horizon. */
    ui->dome_r = 64;
    lv_obj_t *box = plain(page);
    lv_obj_set_size(box, ui->dome_r * 2 + 40, ui->dome_r * 2 + 40);
    ui->f_dome = box;
    int c = ui->dome_r + 20;
    circle(box, c, c, ui->dome_r, 0x2B3D5C, 2);
    circle(box, c, c, ui->dome_r * 2 / 3, C_RING, 1); /* 30 degrees */
    circle(box, c, c, ui->dome_r / 3, C_RING, 1);     /* 60 degrees */
    static const char *const cardinal[] = {"N", "E", "S", "O"};
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *l = label(box, UI_FONT_SMALL, C_MIST, cardinal[i]);
        int x = c + (i == 1 ? ui->dome_r + 10 : i == 3 ? -ui->dome_r - 10 : 0);
        int y = c + (i == 0 ? -ui->dome_r - 10 : i == 2 ? ui->dome_r + 10 : 0);
        lv_obj_set_pos(l, x - 5, y - 9);
    }
    ui->f_dot = plain(box);
    lv_obj_set_size(ui->f_dot, 14, 14);
    lv_obj_set_style_radius(ui->f_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui->f_dot, ui_color(altitude_color(a.altitude_m)), 0);
    lv_obj_set_style_bg_opa(ui->f_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(ui->f_dot, 14, 0);
    lv_obj_set_style_shadow_color(ui->f_dot, ui_color(altitude_color(a.altitude_m)), 0);
    lv_obj_set_style_shadow_opa(ui->f_dot, 140, 0);
    ui->f_look = ui_text(page, "", false);
    lv_obj_set_style_text_align(ui->f_look, LV_TEXT_ALIGN_CENTER, 0);
    ui->f_numbers = ui_text(page, "", true);
    ui->f_type = ui_text(page, "", true);
}

static void flight_update(cielo_ui_t *ui)
{
    cielo_aircraft_t a;
    if (!ui->f_look || !cielo_find(ui->selected, &a))
        return;
    char text[200], number[16], feet[16];
    cielo_route_t route;
    bool known = cielo_route(a.callsign, &route);
    lv_label_set_text(ui->f_airline, known && route.airline[0] ? route.airline : a.callsign[0] ? "VOLO" : "VELIVOLO");
    if (known)
    {
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s", route.from_code, route.to_code);
        lv_label_set_text(ui->f_route, text);
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s", route.from_city, route.to_city);
        lv_label_set_text(ui->f_cities, text);
    }
    else
    {
        lv_label_set_text(ui->f_route, "");
        lv_label_set_text(ui->f_cities, a.callsign[0] ? "Rotta in arrivo o non pubblica" : "Nessun nominativo");
    }
    float e, n;
    cielo_position_now(&a, now_ms(), &e, &n);
    float km = sqrtf(e * e + n * n), bearing = atan2f(e, n) * 180.0f / (float)M_PI;
    float elevation = elevation_deg(a.altitude_m, km);
    /* Dot on the dome: distance from the centre = 90 - elevation. */
    float shown = elevation < 0 ? 0 : elevation;
    float r = (90.0f - shown) / 90.0f * ui->dome_r, rad = bearing * (float)M_PI / 180.0f;
    int c = ui->dome_r + 20;
    lv_obj_set_pos(ui->f_dot, c + (int)(sinf(rad) * r) - 7, c - (int)(cosf(rad) * r) - 7);
    if (elevation >= 1)
        snprintf(text, sizeof(text), "Guarda verso %s, %d%s sopra l'orizzonte", compass_word(bearing), (int)(elevation + 0.5f), "\xC2\xB0");
    else
        snprintf(text, sizeof(text), "Sotto il tuo orizzonte, verso %s", compass_word(bearing));
    lv_label_set_text(ui->f_look, text);
    thousands(a.altitude_m == INT32_MIN ? 0 : a.altitude_m, number, sizeof(number));
    thousands(a.altitude_m == INT32_MIN ? 0 : (int32_t)(a.altitude_m / 0.3048f), feet, sizeof(feet));
    const char *vertical = a.vertical_mpm > 150 ? "in salita" : a.vertical_mpm < -150 ? "in discesa" : "in crociera";
    snprintf(text, sizeof(text), "Quota %s m (%s ft), %s\nVelocità %ld km/h verso %s\nDistanza %.1f km",
             number, feet, vertical, (long)a.speed_kmh, compass_word(a.track_deg), (double)km);
    lv_label_set_text(ui->f_numbers, text);
    const char *model = cielo_type_name(a.type);
    snprintf(text, sizeof(text), "%s%s%s%s%s", model ? model : a.type, a.registration[0] ? " - " : "", a.registration,
             a.squawk[0] ? " - squawk " : "", a.squawk);
    lv_label_set_text(ui->f_type, text);
}

/* ------------------------------------------------------------ frame */

static void render(cielo_ui_t *ui)
{
    if (ui->content)
        lv_obj_delete(ui->content);
    memset(ui->planes, 0, sizeof(ui->planes));
    memset(ui->cards, 0, sizeof(ui->cards));
    ui->f_look = NULL;
    ui->content = plain(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    cielo_status(&ui->status);
    ui->count = cielo_aircraft(ui->aircraft, CIELO_AIRCRAFT);
    if (ui->view == SKY)
    {
        sky(ui);
        sky_update(ui);
        return;
    }
    ui->page = ui_page(ui->content, NULL);
    if (ui->view == LIST)
    {
        list(ui);
        list_update(ui);
    }
    else
    {
        flight(ui);
        flight_update(ui);
    }
}

static void tick(lv_timer_t *timer)
{
    cielo_ui_t *ui = lv_timer_get_user_data(timer);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
        return;
    bool second = ++ui->frames % 10 == 0;
    if (second)
    {
        cielo_status(&ui->status);
        ui->count = cielo_aircraft(ui->aircraft, CIELO_AIRCRAFT);
    }
    if (ui->view == SKY)
    {
        if (second)
            sky_update(ui);
        else
            sky_motion(ui);
    }
    else if (second && ui->view == LIST)
        list_update(ui);
    else if (second)
        flight_update(ui);
}

bool cielo_ui_back(void)
{
    cielo_ui_t *ui = s_ui;
    if (ui->view == SKY)
        extapp_return_to_launcher();
    else
        go(ui, ui->view == FLIGHT ? ui->back_to : SKY);
    return true;
}

void cielo_ui_create(lv_obj_t *screen)
{
    cielo_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ui->aircraft = heap_caps_calloc(CIELO_AIRCRAFT, sizeof(cielo_aircraft_t), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui->aircraft == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->timer = lv_timer_create(tick, 100, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA only: "view sky|list|flight" opens a page (flight = the look-up aircraft). */
#include "core/devcon.h"

bool devcon_app_command(char *line)
{
    if (strncmp(line, "radius ", 7) == 0)
    {
        cielo_set_radius(atoi(line + 7));
        printf("@OK radius\n");
        return true;
    }
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
        return false;
    const char *name = line + 5;
    int view = strcmp(name, "sky") == 0 ? SKY : strcmp(name, "list") == 0 ? LIST : strcmp(name, "flight") == 0 ? FLIGHT : -1;
    if (view < 0)
    {
        printf("@ERR unknown view\n");
        return true;
    }
    lv_port_lock();
    if (view == FLIGHT && !s_ui->selected[0] && s_ui->count > 0)
        snprintf(s_ui->selected, sizeof(s_ui->selected), "%s", s_ui->aircraft[0].hex);
    s_ui->back_to = SKY;
    go(s_ui, view);
    lv_port_unlock();
    printf("@OK view %s\n", name);
    return true;
}
#endif
