/*
 * Pieno on the watch.
 *
 *   HOME      the best place to fill up now: price, where, how far, what you
 *             save on a usual fill against the area's median; a strip with
 *             the area's prices (cheapest, median, dearest).
 *   LIST      ranking (real cost or price), with the change since last time.
 *   STATION   one station: every fuel self/served, age of each price, the
 *             real cost of a fill there.
 *   TREND     the area's daily min and median.
 *   MENU      fuel, self/served, radius, ranking, refresh, dashboard.
 *   DASH      QR code + code for the web dashboard (appkit).
 *
 * The UI only reads the data (pn_lock); the worker downloads.
 */
#include "pieno.h"

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

#define C_BG      0x07100E
#define C_CARD    0x10201C
#define C_LINE    0x1D3530
#define C_INK     0xF1F7F4
#define C_DIM     0x8FA59E
#define C_GOOD    0x2EE39A
#define C_WARN    0xFFB547
#define C_BAD     0xFF6B6B
#define C_ACCENT  0x2EE39A

#define LIST_MAX 40

enum { HOME, LIST, STATION, TREND, MENU, DASH };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *border;
    lv_timer_t *timer;
    int view;
    int back_to;
    uint32_t station;        /* id shown in STATION */
    uint32_t revision;
    pn_phase_t phase;
} pieno_ui_t;

static pieno_ui_t *s_ui;
static void render(pieno_ui_t *ui);

/* ------------------------------------------------------------ helpers */

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

/* 1.899 -> "1,899" */
static void price_text(float price, char *out, size_t size)
{
    int milli = (int)(price * 1000 + 0.5f);
    snprintf(out, size, "%d,%03d", milli / 1000, milli % 1000);
}

static void euro_text(float value, char *out, size_t size)
{
    int cents = (int)(value * 100 + (value >= 0 ? 0.5f : -0.5f));
    snprintf(out, size, "%s%d,%02d €", cents < 0 ? "-" : "", abs(cents) / 100, abs(cents) % 100);
}

static void km_text(float km, char *out, size_t size)
{
    if (km < 1)
    {
        snprintf(out, size, "%d m", (int)(km * 1000 / 10) * 10);
    }
    else
    {
        snprintf(out, size, "%d,%d km", (int)km, (int)(km * 10) % 10);
    }
}

/* Age of a price: green within a day, amber within 4 days, red older. */
static uint32_t age_color(int64_t when)
{
    int64_t now = pn_local_now();
    if (now == 0 || when == 0)
    {
        return C_DIM;
    }
    int64_t hours = (now - when) / 3600;
    return hours < 30 ? C_GOOD : hours < 96 ? C_WARN : C_BAD;
}

static void age_text(int64_t when, char *out, size_t size)
{
    int64_t now = pn_local_now();
    if (now == 0 || when == 0)
    {
        snprintf(out, size, "data non nota");
        return;
    }
    int64_t minutes = (now - when) / 60;
    if (minutes < 90)
    {
        snprintf(out, size, "%d min fa", (int)(minutes < 1 ? 1 : minutes));
    }
    else if (minutes < 36 * 60)
    {
        snprintf(out, size, "%d h fa", (int)(minutes / 60));
    }
    else
    {
        snprintf(out, size, "%d giorni fa", (int)(minutes / 1440));
    }
}

static lv_obj_t *dot(lv_obj_t *parent, uint32_t color, int size)
{
    lv_obj_t *d = box(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, ui_color(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static void go(pieno_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

static const char *mode_name(int self)
{
    return self == PN_SELF ? "self" : "servito";
}

/* --------------------------------------------------------------- HOME */

static void cycle_fuel(lv_event_t *e)
{
    (void)e;
    pn_settings_t s;
    pn_settings(&s);
    s.fuel = (uint8_t)((s.fuel + 1) % PN_FUELS);
    pn_set_settings(&s);
}

static void open_view(lv_event_t *e)
{
    go(s_ui, (int)(intptr_t)lv_event_get_user_data(e));
}

static void open_best(lv_event_t *e)
{
    s_ui->station = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    s_ui->back_to = HOME;
    go(s_ui, STATION);
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, int view)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, UI_FONT_BODY, C_INK, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, open_view, LV_EVENT_CLICKED, (void *)(intptr_t)view);
    lv_port_set_focusable(b);
    return b;
}

/* The area's spread: a green-to-red strip with the best and median marked. */
static void spectrum(lv_obj_t *parent, int width, float lo, float mid, float hi, float best)
{
    lv_obj_t *wrap = box(parent);
    lv_obj_set_size(wrap, width, 46);
    lv_obj_t *strip = box(wrap);
    lv_obj_set_size(strip, width, 10);
    lv_obj_set_pos(strip, 0, 6);
    lv_obj_set_style_radius(strip, 5, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, ui_color(C_GOOD), 0);
    lv_obj_set_style_bg_grad_color(strip, ui_color(C_BAD), 0);
    lv_obj_set_style_bg_grad_dir(strip, LV_GRAD_DIR_HOR, 0);
    float span = hi - lo > 0.001f ? hi - lo : 0.001f;
    int x_mid = (int)((mid - lo) / span * (width - 4));
    int x_best = (int)((best - lo) / span * (width - 14));
    lv_obj_t *median = box(wrap);
    lv_obj_set_size(median, 4, 22);
    lv_obj_set_pos(median, x_mid, 0);
    lv_obj_set_style_radius(median, 2, 0);
    lv_obj_set_style_bg_color(median, ui_color(C_INK), 0);
    lv_obj_set_style_bg_opa(median, LV_OPA_COVER, 0);
    lv_obj_t *mark = dot(wrap, C_INK, 14);
    lv_obj_set_pos(mark, x_best, 4);
    lv_obj_set_style_border_color(mark, ui_color(C_GOOD), 0);
    lv_obj_set_style_border_width(mark, 3, 0);
    char text[16];
    price_text(lo, text, sizeof(text));
    lv_obj_t *l = label(wrap, UI_FONT_SMALL, C_DIM, text);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    price_text(hi, text, sizeof(text));
    l = label(wrap, UI_FONT_SMALL, C_DIM, text);
    lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    char median_text[32];
    price_text(mid, text, sizeof(text));
    snprintf(median_text, sizeof(median_text), "| mediana %s", text);
    l = label(wrap, UI_FONT_SMALL, C_INK, median_text);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void home(pieno_ui_t *ui)
{
    const board_info_t *b = board_info();
    pn_settings_t s;
    pn_settings(&s);
    pn_status_t st;
    pn_status(&st);

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, b->round ? b->width * 78 / 100 : b->width - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_center(col);
    int width = b->round ? b->width * 66 / 100 : b->width - 40;

    /* Fuel line: tap to change fuel. */
    char text[96];
    snprintf(text, sizeof(text), "%s · %s · %d km  " LV_SYMBOL_LOOP, pn_fuel_names[s.fuel], mode_name(s.self), s.radius_km);
    lv_obj_t *fuel = label(col, UI_FONT_BODY, C_ACCENT, text);
    lv_obj_set_clickable(fuel, true);
    lv_obj_set_ext_click_area(fuel, 14);
    lv_obj_add_event_cb(fuel, cycle_fuel, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(fuel);

    if (s.lat == 0 && s.lon == 0)
    {
        label(col, ui_font_title, C_INK, "Dove sei?");
        lv_obj_t *t = ui_text(col, "Imposta la posizione dalla dashboard (Menu > Dashboard) o la città del meteo.", true);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        pill_button(col, "Dashboard", DASH);
        return;
    }

    pn_lock();
    int count = 0;
    const pn_station_t *table = pn_stations(&count);
    uint16_t order[1];
    int n = pn_rank(s.fuel, s.self, &s, order, 1);
    float lo = 0, mid = 0, hi = 0;
    int in_area = 0;
    bool area = pn_area(s.fuel, s.self, &s, &lo, &mid, &hi, &in_area);
    pn_station_t best;
    if (n > 0)
    {
        best = table[order[0]];
    }
    pn_unlock();

    if (n == 0)
    {
        bool busy = st.phase != PN_IDLE && st.phase != PN_FAILED;
        label(col, ui_font_title, C_INK, busy ? "Scarico i prezzi" : "Nessun prezzo");
        lv_obj_t *t = ui_text(col, busy || st.phase == PN_FAILED ? st.message :
                              "Nessun distributore vende questo carburante nel raggio: allargalo dal menu.", true);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        pill_button(col, "Menu", MENU);
        return;
    }

    lv_obj_t *caption = label(col, UI_FONT_SMALL, C_DIM, s.real_cost ? "IL PIENO CHE COSTA MENO" : "IL PREZZO PIÙ BASSO");
    lv_obj_set_style_text_letter_space(caption, 1, 0);

    /* Price: big digits + €/L. Tap: the station. */
    lv_obj_t *hero = box(col);
    lv_obj_set_size(hero, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(hero, 6, 0);
    lv_obj_set_clickable(hero, true);
    lv_obj_add_event_cb(hero, open_best, LV_EVENT_CLICKED, (void *)(uintptr_t)best.id);
    lv_port_set_focusable(hero);
    price_text(best.price[s.fuel][s.self], text, sizeof(text));
    label(hero, ui_font_big, C_INK, text);
    lv_obj_t *unit = label(hero, UI_FONT_BODY, C_DIM, "€/L");
    lv_obj_set_style_pad_bottom(unit, 10, 0);

    /* Where. */
    lv_obj_t *where = box(col);
    lv_obj_set_size(where, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(where, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(where, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(where, 8, 0);
    dot(where, age_color(best.when[s.fuel][s.self]), 10);
    char km[16];
    km_text(best.km, km, sizeof(km));
    snprintf(text, sizeof(text), "%s · %s", best.brand, km);
    label(where, UI_FONT_LARGE, C_INK, text);
    lv_obj_t *town = label(col, UI_FONT_SMALL, C_DIM, best.town);
    lv_label_set_long_mode(town, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(town, width, 0);

    /* What it saves on a usual fill compared with the area's median. */
    if (area && in_area > 1)
    {
        float saving = (mid - best.price[s.fuel][s.self]) * s.litres;
        if (s.real_cost)
        {
            saving -= (pn_real_price(best.price[s.fuel][s.self], best.km, s.litres, s.km_per_litre) -
                       best.price[s.fuel][s.self]) * s.litres;
        }
        char euro[20];
        euro_text(saving, euro, sizeof(euro));
        if (saving > 0.05f)
        {
            snprintf(text, sizeof(text), "Risparmi %s su %d L", euro, (int)s.litres);
            label(col, UI_FONT_BODY, C_GOOD, text);
        }
        else
        {
            label(col, UI_FONT_BODY, C_DIM, "Prezzi allineati in zona");
        }
        spectrum(col, width, lo, mid, hi, best.price[s.fuel][s.self]);
        if (s.target > 0 && best.price[s.fuel][s.self] <= s.target)
        {
            label(col, UI_FONT_SMALL, C_GOOD, LV_SYMBOL_OK " Sotto il tuo prezzo obiettivo");
        }
    }

    lv_obj_t *row = box(col);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    pill_button(row, LV_SYMBOL_LIST, LIST);
    pill_button(row, LV_SYMBOL_IMAGE, TREND);
    pill_button(row, LV_SYMBOL_SETTINGS, MENU);

    if (st.phase == PN_FAILED || (st.phase != PN_IDLE && st.phase != PN_FAILED))
    {
        lv_obj_t *t = label(col, UI_FONT_SMALL, st.phase == PN_FAILED ? C_WARN : C_DIM, st.message);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
}

/* --------------------------------------------------------------- LIST */

static void open_station(lv_event_t *e)
{
    s_ui->station = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    s_ui->back_to = LIST;
    go(s_ui, STATION);
}

static void list(pieno_ui_t *ui)
{
    pn_settings_t s;
    pn_settings(&s);
    lv_obj_t *page = ui_page(ui->content, "Classifica");
    char text[96];
    snprintf(text, sizeof(text), "%s %s · per %s", pn_fuel_names[s.fuel], mode_name(s.self),
             s.real_cost ? "costo reale" : "prezzo");
    ui_text(page, text, true);

    uint16_t *order = malloc(sizeof(uint16_t) * LIST_MAX);
    pn_station_t *rows = heap_caps_malloc(sizeof(pn_station_t) * LIST_MAX, MALLOC_CAP_SPIRAM);
    if (order == NULL || rows == NULL)
    {
        free(order);
        free(rows);
        return;
    }
    pn_lock();
    int count = 0;
    const pn_station_t *table = pn_stations(&count);
    int n = pn_rank(s.fuel, s.self, &s, order, LIST_MAX);
    for (int i = 0; i < n; i++)
    {
        rows[i] = table[order[i]];
    }
    pn_unlock();

    const board_info_t *b = board_info();
    int width = b->round ? b->width * 74 / 100 : b->width - 24;
    for (int i = 0; i < n; i++)
    {
        const pn_station_t *r = &rows[i];
        lv_obj_t *card = lv_button_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, width, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, ui_color(i == 0 ? 0x123A2E : C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, ui_color(C_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_add_event_cb(card, open_station, LV_EVENT_CLICKED, (void *)(uintptr_t)r->id);
        lv_port_set_focusable(card);

        float price = r->price[s.fuel][s.self];
        price_text(price, text, sizeof(text));
        lv_obj_t *p = label(card, ui_font_title, i == 0 ? C_GOOD : C_INK, text);
        lv_obj_align(p, LV_ALIGN_TOP_RIGHT, 0, 0);

        snprintf(text, sizeof(text), "%d  %s", i + 1, r->brand);
        lv_obj_t *name = label(card, UI_FONT_BODY, C_INK, text);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, width - 24 - 100);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 2);

        char km[16];
        km_text(r->km, km, sizeof(km));
        float before = r->previous[s.fuel][s.self];
        char change[24] = "";
        if (before > 0)
        {
            int delta = (int)((price - before) * 1000 + (price > before ? 0.5f : -0.5f));
            snprintf(change, sizeof(change), "  %s%d,%03d", delta > 0 ? LV_SYMBOL_UP " " : LV_SYMBOL_DOWN " ",
                     abs(delta) / 1000, abs(delta) % 1000);
        }
        snprintf(text, sizeof(text), "%s · %s%s", km, r->town, change);
        lv_obj_t *sub = label(card, UI_FONT_SMALL, C_DIM, text);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(sub, width - 24 - 18);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 18, 34);
        lv_obj_t *age = dot(card, age_color(r->when[s.fuel][s.self]), 9);
        lv_obj_align(age, LV_ALIGN_TOP_LEFT, 0, 40);
    }
    if (n == 0)
    {
        ui_text(page, "Nessun prezzo recente per questo carburante.", true);
    }
    else
    {
        ui_text(page, "Il pallino indica quanto è recente il prezzo: verde oggi, giallo pochi giorni.", true);
    }

    /* Prices older than the limit: shown apart, never ranked. */
    int old = 0;
    pn_lock();
    table = pn_stations(&count);
    for (int i = 0; i < count && old < LIST_MAX; i++)
    {
        if (table[i].price[s.fuel][s.self] > 0 && !pn_price_valid(&table[i], s.fuel, s.self, &s))
        {
            rows[old++] = table[i];
        }
    }
    pn_unlock();
    if (old > 0)
    {
        snprintf(text, sizeof(text), "NON CONFERMATI (più di %d giorni)", s.max_age_days);
        ui_section(page, text);
        for (int i = 0; i < old; i++)
        {
            char price[16], age[24], km[16];
            price_text(rows[i].price[s.fuel][s.self], price, sizeof(price));
            age_text(rows[i].when[s.fuel][s.self], age, sizeof(age));
            km_text(rows[i].km, km, sizeof(km));
            snprintf(text, sizeof(text), "%s  %s · %s · %s", price, rows[i].brand, km, age);
            lv_obj_t *t = ui_text(page, text, true);
            lv_obj_set_style_text_color(t, ui_color(0x5E726C), 0);
        }
    }
    free(order);
    free(rows);
}

/* ------------------------------------------------------------ STATION */

static void station(pieno_ui_t *ui)
{
    pn_settings_t s;
    pn_settings(&s);
    pn_station_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_SPIRAM);
    if (r == NULL)
    {
        return;
    }
    bool found = false;
    pn_lock();
    int count = 0;
    const pn_station_t *table = pn_stations(&count);
    for (int i = 0; i < count; i++)
    {
        if (table[i].id == ui->station)
        {
            *r = table[i];
            found = true;
            break;
        }
    }
    pn_unlock();
    lv_obj_t *page = ui_page(ui->content, found ? r->brand : "Distributore");
    if (!found)
    {
        ui_text(page, "Non più nel raggio.", true);
        free(r);
        return;
    }
    char text[128], km[16];
    km_text(r->km, km, sizeof(km));
    snprintf(text, sizeof(text), "%s\n%s (%s) · %s", r->address, r->town, r->province, km);
    lv_obj_t *t = ui_text(page, text, true);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    const board_info_t *b = board_info();
    int width = b->round ? b->width * 72 / 100 : b->width - 24;
    for (int f = 0; f < PN_FUELS; f++)
    {
        if (r->price[f][0] <= 0 && r->price[f][1] <= 0)
        {
            continue;
        }
        lv_obj_t *card = box(page);
        lv_obj_set_size(card, width, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, ui_color(f == s.fuel ? 0x123A2E : C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);
        label(card, UI_FONT_BODY, f == s.fuel ? C_GOOD : C_DIM, pn_fuel_names[f]);
        for (int m = PN_SELF; m >= PN_SERVED; m--)
        {
            if (r->price[f][m] <= 0)
            {
                continue;
            }
            char price[16], age[24];
            price_text(r->price[f][m], price, sizeof(price));
            age_text(r->when[f][m], age, sizeof(age));
            lv_obj_t *line = box(card);
            lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_t *p = label(line, ui_font_title, C_INK, price);
            lv_obj_align(p, LV_ALIGN_LEFT_MID, 0, 0);
            snprintf(text, sizeof(text), "%s\n%s", mode_name(m), age);
            lv_obj_t *meta = label(line, UI_FONT_SMALL, age_color(r->when[f][m]), text);
            lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(meta, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    float price = r->price[s.fuel][s.self];
    if (price > 0)
    {
        float fill = price * s.litres;
        float trip = (pn_real_price(price, r->km, s.litres, s.km_per_litre) - price) * s.litres;
        char a[20], c[20];
        euro_text(fill, a, sizeof(a));
        euro_text(trip, c, sizeof(c));
        snprintf(text, sizeof(text), "Pieno da %d L: %s\nViaggio A/R: +%s", (int)s.litres, a, c);
        t = ui_text(page, text, false);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        float before = r->previous[s.fuel][s.self];
        if (before > 0)
        {
            char p0[16];
            price_text(before, p0, sizeof(p0));
            snprintf(text, sizeof(text), "Prima costava %s", p0);
            t = ui_text(page, text, true);
            lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    free(r);
}

/* -------------------------------------------------------------- TREND */

static void trend(pieno_ui_t *ui)
{
    pn_settings_t s;
    pn_settings(&s);
    char text[96];
    snprintf(text, sizeof(text), "%s %s", pn_fuel_names[s.fuel], mode_name(s.self));
    lv_obj_t *page = ui_page(ui->content, "Andamento");
    ui_text(page, text, true);

    pn_day_t *days = heap_caps_malloc(sizeof(pn_day_t) * 60, MALLOC_CAP_SPIRAM);
    if (days == NULL)
    {
        return;
    }
    pn_day_t *all = heap_caps_malloc(sizeof(pn_day_t) * PN_HISTORY_DAYS, MALLOC_CAP_SPIRAM);
    int total = all != NULL ? pn_history(s.fuel, s.self, all, PN_HISTORY_DAYS) : 0;
    int n = total > 60 ? 60 : total;
    if (all != NULL)
    {
        memcpy(days, all + total - n, sizeof(pn_day_t) * n);
    }
    free(all);

    if (n < 2)
    {
        lv_obj_t *t = ui_text(page, n == 1 ? "Primo giorno registrato: lo storico cresce con ogni aggiornamento (uno al giorno)."
                                           : "Ancora nessun dato.", true);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        if (n == 1)
        {
            char lo[16], md[16];
            price_text(days[0].min, lo, sizeof(lo));
            price_text(days[0].median, md, sizeof(md));
            snprintf(text, sizeof(text), "Oggi: minimo %s · mediana %s", lo, md);
            ui_text(page, text, false);
        }
        free(days);
        return;
    }

    const board_info_t *b = board_info();
    int width = b->round ? b->width * 72 / 100 : b->width - 30;
    lv_obj_t *chart = lv_chart_create(page);
    lv_obj_set_size(chart, width, b->height * 34 / 100);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, n);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, ui_color(C_LINE), LV_PART_MAIN);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    int lo = 100000, hi = 0;
    for (int i = 0; i < n; i++)
    {
        int a = (int)(days[i].min * 1000), m = (int)(days[i].median * 1000);
        lo = a < lo ? a : lo;
        hi = m > hi ? m : hi;
    }
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo - 10, hi + 10);
    lv_chart_series_t *median = lv_chart_add_series(chart, ui_color(C_WARN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *minimum = lv_chart_add_series(chart, ui_color(C_GOOD), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < n; i++)
    {
        lv_chart_set_next_value(chart, median, (int32_t)(days[i].median * 1000));
        lv_chart_set_next_value(chart, minimum, (int32_t)(days[i].min * 1000));
    }

    char a[16], c[16];
    price_text(days[n - 1].min, a, sizeof(a));
    price_text(days[n - 1].median, c, sizeof(c));
    lv_obj_t *legend = box(page);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 16, 0);
    snprintf(text, sizeof(text), "minimo %s", a);
    label(legend, UI_FONT_BODY, C_GOOD, text);
    snprintf(text, sizeof(text), "mediana %s", c);
    label(legend, UI_FONT_BODY, C_WARN, text);

    int back = n > 7 ? n - 8 : 0;
    float delta = days[n - 1].median - days[back].median;
    int permille = (int)(delta / days[back].median * 1000 + (delta >= 0 ? 0.5f : -0.5f));
    snprintf(text, sizeof(text), "In %d giorni la mediana è %s del %d,%d%%", n - 1 - back,
             delta > 0.0005f ? "salita" : delta < -0.0005f ? "scesa" : "rimasta ferma",
             abs(permille) / 10, abs(permille) % 10);
    lv_obj_t *t = ui_text(page, text, true);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    free(days);
}

/* --------------------------------------------------------------- MENU */

static void menu_action(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    pn_settings_t s;
    pn_settings(&s);
    static const uint8_t radii[] = {3, 5, 10, 15, 20, 30};
    switch (action)
    {
    case 0:
        s.fuel = (uint8_t)((s.fuel + 1) % PN_FUELS);
        break;
    case 1:
        s.self = s.self == PN_SELF ? PN_SERVED : PN_SELF;
        break;
    case 2:
    {
        int i = 0;
        while (i < (int)sizeof(radii) - 1 && radii[i] <= s.radius_km)
        {
            i++;
        }
        s.radius_km = radii[i] > s.radius_km ? radii[i] : radii[0];
        break;
    }
    case 3:
        s.real_cost = !s.real_cost;
        break;
    case 4:
        pn_refresh();
        ui_toast("Aggiorno i prezzi...");
        return;
    default:
        return;
    }
    pn_set_settings(&s);
}

static lv_obj_t *menu_row(lv_obj_t *page, const char *title, const char *value, int action)
{
    const board_info_t *b = board_info();
    lv_obj_t *row = lv_button_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, b->round ? b->width * 72 / 100 : b->width - 24, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_t *l = label(row, UI_FONT_BODY, C_INK, title);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (value != NULL)
    {
        lv_obj_t *v = label(row, UI_FONT_BODY, C_ACCENT, value);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (action >= 100)
    {
        lv_obj_add_event_cb(row, open_view, LV_EVENT_CLICKED, (void *)(intptr_t)(action - 100));
    }
    else
    {
        lv_obj_add_event_cb(row, menu_action, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    }
    lv_port_set_focusable(row);
    return row;
}

static void menu(pieno_ui_t *ui)
{
    pn_settings_t s;
    pn_settings(&s);
    pn_status_t st;
    pn_status(&st);
    lv_obj_t *page = ui_page(ui->content, "Pieno");
    char radius[12];
    snprintf(radius, sizeof(radius), "%d km", s.radius_km);
    menu_row(page, LV_SYMBOL_WIFI "  Dashboard", NULL, 100 + DASH);
    menu_row(page, "Carburante", pn_fuel_names[s.fuel], 0);
    menu_row(page, "Rifornimento", mode_name(s.self), 1);
    menu_row(page, "Raggio", radius, 2);
    menu_row(page, "Classifica per", s.real_cost ? "costo reale" : "prezzo", 3);
    menu_row(page, LV_SYMBOL_REFRESH "  Aggiorna ora", NULL, 4);

    char text[160];
    snprintf(text, sizeof(text), "Costo reale: il carburante bruciato per andare e tornare (%.0f km/L, pieno da %.0f L).",
             (double)s.km_per_litre, (double)s.litres);
    ui_text(page, text, true);
    snprintf(text, sizeof(text), "Dati MIMIT del %s · %d distributori entro %d km da %s.",
             st.extraction[0] ? st.extraction : "–", st.count, s.radius_km, s.place[0] ? s.place : "casa");
    ui_text(page, text, true);
    snprintf(text, sizeof(text), "Prezzi più vecchi di %d giorni: non confermati, fuori classifica.", s.max_age_days);
    if (s.max_age_days > 0)
    {
        ui_text(page, text, true);
    }
    ui_text(page, "Consumi, obiettivo di prezzo e posizione si cambiano dalla dashboard.", true);
}

static void dash(pieno_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dashboard");
    appkit_dashboard_page(page, ui_color(C_ACCENT));
}

/* ------------------------------------------------------------- frame */

static void render(pieno_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->content = box(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    pn_status_t st;
    pn_status(&st);
    ui->revision = st.revision;
    ui->phase = st.phase;
    switch (ui->view)
    {
    case HOME: home(ui); break;
    case LIST: list(ui); break;
    case STATION: station(ui); break;
    case TREND: trend(ui); break;
    case MENU: menu(ui); break;
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    pieno_ui_t *ui = lv_timer_get_user_data(timer);
    pn_status_t st;
    pn_status(&st);
    bool busy = st.phase == PN_STATIONS || st.phase == PN_PRICES;
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, !busy);
        if (busy)
        {
            ui_border_progress_set(ui->border, st.percent, 100);
        }
    }
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
    {
        return;
    }
    /* Redraw on new data; not the dashboard page (its QR keeps itself up to date). */
    if (st.revision != ui->revision && ui->view != DASH)
    {
        render(ui);
    }
}

bool pieno_ui_back(void)
{
    pieno_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return false;
    }
    if (ui->view == HOME)
    {
#if CONFIG_APPKIT_HOSTED
        return false;   /* internal app: the app manager closes it */
#else
        extapp_return_to_launcher();
#endif
    }
    else
    {
        go(ui, ui->view == STATION ? ui->back_to : ui->view == DASH ? MENU : HOME);
    }
    return true;
}

void pieno_ui_create(lv_obj_t *screen)
{
    pieno_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->border = ui_border_progress_create(screen, C_ACCENT);
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, true);
    }
    ui->timer = lv_timer_create(tick, 500, ui);
}

void pieno_ui_destroy(void)
{
    pieno_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return;
    }
    s_ui = NULL;
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}

#if CONFIG_WATCH_DEV_CONSOLE && !CONFIG_APPKIT_HOSTED
/* QA only: "view home|list|station|trend|menu|dash", "fuel N", "refresh", "best". */
#include "core/devcon.h"

bool devcon_app_command(char *line)
{
    static const char *const views[] = {"home", "list", "station", "trend", "menu", "dash"};
    if (strcmp(line, "refresh") == 0)
    {
        pn_refresh();
        printf("@OK refresh\n");
        return true;
    }
    if (strcmp(line, "status") == 0)
    {
        pn_status_t st;
        pn_status(&st);
        pn_settings_t s;
        pn_settings(&s);
        printf("@PIENO phase=%d percent=%d count=%d data=%s radius=%d fuel=%d at=%.4f,%.4f (%s) code=%s message=%s\n",
               st.phase, st.percent, st.count, st.extraction, s.radius_km, s.fuel, (double)s.lat, (double)s.lon, s.place,
               appkit_web_code(), st.message);
        return true;
    }
    if (strncmp(line, "fuel ", 5) == 0)
    {
        pn_settings_t s;
        pn_settings(&s);
        s.fuel = (uint8_t)(atoi(line + 5) % PN_FUELS);
        pn_set_settings(&s);
        printf("@OK fuel\n");
        return true;
    }
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
    {
        return false;
    }
    for (int v = 0; v < 6; v++)
    {
        if (strcmp(line + 5, views[v]) == 0)
        {
            lv_port_lock();
            if (v == STATION && s_ui->station == 0)
            {
                pn_settings_t s;
                pn_settings(&s);
                uint16_t order[1];
                pn_lock();
                int count = 0;
                const pn_station_t *table = pn_stations(&count);
                if (pn_rank(s.fuel, s.self, &s, order, 1) == 1)
                {
                    s_ui->station = table[order[0]].id;
                }
                pn_unlock();
                s_ui->back_to = LIST;
            }
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
