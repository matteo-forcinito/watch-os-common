/*
 * Motore on the watch, "3026" style (deep space, cyan/violet light).
 *
 *   home      link status and the menu
 *   drive     rpm ring, huge speed, coolant / battery / consumption
 *   live      every value the car supports
 *   diag      check engine light, codes with descriptions, readiness,
 *             freeze frame, clear (code + car stopped + engine off)
 *   trip      distance, time, fuel, averages, peaks
 *   link      adapter: Bluetooth scan and choice, or Wi-Fi address
 *   car       fuel, displacement, safety code, lock
 *   dash      the web dashboard (QR, address, pairing code)
 *
 * One screen, rebuilt per view; a timer refreshes values in place (drive,
 * live) or rebuilds on new data (the rest).
 */
#include "motore.h"

#include "appkit.h"
#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG      0x05070F
#define C_CYAN    0x3CF0FF
#define C_VIOLET  0x8B5CFF
#define C_MAGENTA 0xFF4FD8
#define C_GOOD    0x2FE39A
#define C_WARN    0xFFC04D
#define C_BAD     0xFF5D7A
#define C_DIM     0x8FA3C0

typedef enum { HOME, DRIVE, LIVE, DIAG, TRIP, LINK, CAR, DASH } view_t;

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *root;           /* current view */
    view_t view;
    uint32_t revision;
    lv_timer_t *timer;
    /* drive */
    lv_obj_t *arc, *speed, *rpm, *coolant, *battery, *fuel, *mil, *status, *gear;
    lv_obj_t *led[15];
    float shown_rpm;
    uint16_t shift;
    int lit, blink;
    /* live: value label per value */
    lv_obj_t *live[MT_VALUES];
    int last_found;
} motore_ui_t;

static motore_ui_t *s_ui;

static void go(motore_ui_t *ui, view_t view);

/* ------------------------------------------------------------ helpers */

static const char *phase_text(mt_phase_t p)
{
    switch (p)
    {
    case MT_PHASE_LIVE: return "COLLEGATO";
    case MT_PHASE_SETUP: return "AVVIO";
    case MT_PHASE_CONNECTING: return "CONNESSIONE";
    case MT_PHASE_SEARCHING: return "RICERCA";
    case MT_PHASE_NO_CAR: return "AUTO SPENTA";
    case MT_PHASE_LOST: return "RIPROVO";
    default: return "DA CONFIGURARE";
    }
}

static uint32_t phase_color(mt_phase_t p)
{
    return p == MT_PHASE_LIVE ? C_GOOD : p == MT_PHASE_LOST || p == MT_PHASE_IDLE ? C_BAD : C_WARN;
}

static void format_value(const mt_state_t *s, mt_value_t v, char *out, size_t size)
{
    const mt_value_info_t *info = mt_value_info(v);
    if (s->updated[v] == 0)
    {
        snprintf(out, size, "—");
        return;
    }
    snprintf(out, size, "%.*f %s", info->decimals, (double)s->value[v], info->unit);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void on_go(lv_event_t *e)
{
    go(s_ui, (view_t)(intptr_t)lv_event_get_user_data(e));
}

/* Status strip at the top of a page: coloured dot, phase, message. */
static void status_card(lv_obj_t *page, const mt_state_t *s)
{
    lv_obj_t *card = lv_obj_create(page);
    ui_make_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, ui_color(phase_color(s->phase)), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    char head[48];
    snprintf(head, sizeof(head), LV_SYMBOL_BULLET " %s", phase_text(s->phase));
    label(card, UI_FONT_SMALL, phase_color(s->phase), head);
    lv_obj_t *m = label(card, UI_FONT_BODY, 0xFFFFFF, s->message);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, LV_PCT(100));
    if (s->vin[0] != 0)
    {
        char vin[32];
        snprintf(vin, sizeof(vin), "VIN %s", s->vin);
        label(card, UI_FONT_SMALL, C_DIM, vin);
    }
}

/* ------------------------------------------------------------ home */

static void build_home(motore_ui_t *ui, const mt_state_t *s)
{
    lv_obj_t *page = ui_page(ui->root, "Motore");
    status_card(page, s);
    char n[24];
    ui_row(page, LV_SYMBOL_PLAY, C_CYAN, "Guida", NULL, on_go, (void *)DRIVE);
    ui_row(page, LV_SYMBOL_LIST, C_VIOLET, "Dati live", NULL, on_go, (void *)LIVE);
    snprintf(n, sizeof(n), "%s%u", s->mil ? LV_SYMBOL_WARNING " " : "", s->dtcs);
    ui_row(page, LV_SYMBOL_WARNING, s->mil || s->dtcs ? C_BAD : C_GOOD, "Diagnosi", n, on_go, (void *)DIAG);
    snprintf(n, sizeof(n), "%.1f km", (double)s->trip.km);
    ui_row(page, LV_SYMBOL_GPS, C_GOOD, "Viaggio", n, on_go, (void *)TRIP);
    mt_settings_t st;
    mt_settings(&st);
    ui_row(page, LV_SYMBOL_BLUETOOTH, C_CYAN, "Collegamento",
           st.link == MT_LINK_WIFI ? "Wi-Fi" : st.ble_name[0] ? st.ble_name : "da scegliere", on_go, (void *)LINK);
    ui_row(page, LV_SYMBOL_SETTINGS, C_DIM, "Auto e sicurezza", s->unlocked ? "sbloccato" : NULL, on_go, (void *)CAR);
    ui_row(page, LV_SYMBOL_WIFI, C_MAGENTA, "Dashboard web", NULL, on_go, (void *)DASH);
}

/* ------------------------------------------------------------ drive */

static lv_obj_t *chip(lv_obj_t *parent, const char *icon, uint32_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(box, UI_FONT_SMALL, color, icon);
    return label(box, UI_FONT_BODY, 0xFFFFFF, "—");
}

/* Shift lights: LED segments along the top edge, like a race car's wheel.
 * They light from 55% of the shift point (green, amber, red) and all flash
 * above it; the rpm arc glides towards each reading so a gear change reads
 * as a sweep, not a jump. */
#define SHIFT_LEDS  15
#define SHIFT_FROM  0.55f
#define DRIVE_MS    50

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0)
    {
        lv_label_set_text(l, text);   /* only real changes are redrawn */
    }
}

static lv_obj_t *ring(lv_obj_t *root, int32_t size, int32_t from, int32_t to, int32_t width, uint32_t color)
{
    lv_obj_t *a = lv_arc_create(root);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_bg_angles(a, from, to);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_style(a, NULL, LV_PART_INDICATOR);
    lv_obj_set_clickable(a, false);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, ui_color(color), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    return a;
}

static uint32_t led_color(int i)
{
    return i < SHIFT_LEDS / 3 ? C_GOOD : i < SHIFT_LEDS * 2 / 3 ? C_WARN : C_BAD;
}

static void build_drive(motore_ui_t *ui)
{
    lv_obj_t *root = ui->root;
    int32_t w = lv_obj_get_width(ui->screen), h = lv_obj_get_height(ui->screen);
    int32_t side = (w < h ? w : h) - 8;
    mt_settings_t st;
    mt_settings(&st);
    ui->shift = st.shift_rpm;
    ui->shown_rpm = 0;
    ui->lit = -1;
    ui->blink = 0;

    /* Shift lights on the outer edge: 200..340 degrees (the top). */
    int32_t seg = 140 / SHIFT_LEDS;
    for (int i = 0; i < SHIFT_LEDS; i++)
    {
        int32_t a = 200 + i * seg;
        ui->led[i] = ring(root, side, a, a + seg - 2, side / 30, led_color(i));
        lv_obj_set_style_arc_opa(ui->led[i], LV_OPA_20, LV_PART_MAIN);
    }

    int32_t inner = side - side / 9;
    ui->arc = lv_arc_create(root);
    lv_obj_set_size(ui->arc, inner, inner);
    lv_obj_center(ui->arc);
    lv_arc_set_bg_angles(ui->arc, 135, 45);
    lv_arc_set_range(ui->arc, 0, 7000);
    lv_arc_set_value(ui->arc, 0);
    lv_obj_remove_style(ui->arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui->arc, false);
    lv_obj_set_style_arc_width(ui->arc, side / 20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->arc, side / 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->arc, ui_color(0x141C33), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->arc, ui_color(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->arc, true, LV_PART_INDICATOR);

    /* Thin inner ring: a tick every 1000 rpm and the shift zone in magenta. */
    int32_t tick_ring = inner - side / 9;
    for (int k = 0; k <= 7; k++)
    {
        int32_t a = 135 + k * 270 / 7;
        ring(root, tick_ring, a - 1, a + 1, side / 40, C_DIM);
    }
    ring(root, tick_ring, 135 + ui->shift * 270 / 7000, 45, side / 80, C_MAGENTA);

    ui->gear = label(root, &lv_font_montserrat_32, C_VIOLET, "");
    lv_obj_align(ui->gear, LV_ALIGN_CENTER, 0, -side / 4);
    ui->speed = label(root, &lv_font_montserrat_48, 0xFFFFFF, "—");
    lv_obj_set_style_transform_scale(ui->speed, side > 400 ? 440 : 360, 0);
    lv_obj_set_style_transform_pivot_x(ui->speed, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(ui->speed, LV_PCT(50), 0);
    lv_obj_align(ui->speed, LV_ALIGN_CENTER, 0, -side / 30);
    lv_obj_t *unit = label(root, UI_FONT_SMALL, C_DIM, "KM/H");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, side / 8);
    ui->rpm = label(root, UI_FONT_BODY, C_CYAN, "— rpm");
    lv_obj_align(ui->rpm, LV_ALIGN_CENTER, 0, side / 5);

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, side * 2 / 3, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, side / 3);
    ui->coolant = chip(row, "TEMP", C_CYAN);
    ui->battery = chip(row, "BATT", C_GOOD);
    ui->fuel = chip(row, "CONS", C_VIOLET);

    ui->mil = label(root, UI_FONT_BODY, C_BAD, LV_SYMBOL_WARNING);
    lv_obj_align(ui->mil, LV_ALIGN_CENTER, -side / 5, -side / 4);
    lv_obj_set_hidden(ui->mil, true);
    ui->status = label(root, UI_FONT_SMALL, C_WARN, "");
    lv_obj_align(ui->status, LV_ALIGN_CENTER, 0, -side / 6);

    /* Tap anywhere: back to the menu. */
    lv_obj_add_event_cb(root, on_go, LV_EVENT_CLICKED, (void *)HOME);
}

static void update_drive(motore_ui_t *ui, const mt_state_t *s)
{
    char t[32];
    bool live = s->phase == MT_PHASE_LIVE;
    bool has_rpm = mt_value_fresh(s, MT_RPM);
    float target = has_rpm ? s->value[MT_RPM] : 0;
    /* Glide: 45% of the way at every 50 ms frame (~150 ms to settle). */
    ui->shown_rpm += (target - ui->shown_rpm) * 0.45f;
    float rpm = ui->shown_rpm;
    bool over = has_rpm && target >= ui->shift;
    ui->blink = over ? ui->blink + 1 : 0;
    bool flash_on = over && (ui->blink / 2) % 2 == 0;   /* 5 Hz */

    lv_arc_set_value(ui->arc, (int32_t)rpm);
    lv_obj_set_style_arc_color(ui->arc, ui_color(over ? C_MAGENTA : rpm > ui->shift * 0.8f ? C_WARN : C_CYAN), LV_PART_INDICATOR);

    /* LEDs: recolored only when the count or the flash state changes. */
    float from = ui->shift * SHIFT_FROM;
    int lit = rpm <= from ? 0 : (int)((rpm - from) / (ui->shift - from) * SHIFT_LEDS + 0.5f);
    lit = lit > SHIFT_LEDS ? SHIFT_LEDS : lit;
    int key = over ? (flash_on ? 100 : 101) : lit;
    if (key != ui->lit)
    {
        ui->lit = key;
        for (int i = 0; i < SHIFT_LEDS; i++)
        {
            if (over)
            {
                lv_obj_set_style_arc_color(ui->led[i], ui_color(C_MAGENTA), LV_PART_MAIN);
                lv_obj_set_style_arc_opa(ui->led[i], flash_on ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
            }
            else
            {
                lv_obj_set_style_arc_color(ui->led[i], ui_color(led_color(i)), LV_PART_MAIN);
                lv_obj_set_style_arc_opa(ui->led[i], i < lit ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
            }
        }
    }

    bool has_speed = mt_value_fresh(s, MT_SPEED);
    snprintf(t, sizeof(t), "%.0f", has_speed ? (double)s->value[MT_SPEED] : 0.0);
    set_text(ui->speed, has_speed ? t : "—");
    snprintf(t, sizeof(t), "%.0f rpm", (double)(has_rpm ? target : 0));
    set_text(ui->rpm, has_rpm ? t : "— rpm");
    mt_settings_t st;
    mt_settings(&st);
    int gear = has_speed && has_rpm ? mt_gear(&st, s->value[MT_SPEED], target) : 0;
    snprintf(t, sizeof(t), "%d", gear);
    set_text(ui->gear, gear ? t : (has_speed && s->value[MT_SPEED] > 3 ? "N" : ""));

    format_value(s, MT_COOLANT, t, sizeof(t));
    set_text(ui->coolant, t);
    lv_obj_set_style_text_color(ui->coolant, ui_color(s->value[MT_COOLANT] > 105 ? C_BAD : 0xFFFFFF), 0);
    format_value(s, MT_BATTERY, t, sizeof(t));
    set_text(ui->battery, t);
    float lph = mt_value_fresh(s, MT_FUEL_RATE) ? s->value[MT_FUEL_RATE] : NAN;
    float kmh = has_speed ? s->value[MT_SPEED] : 0;
    if (isnan(lph))
    {
        snprintf(t, sizeof(t), "—");
    }
    else if (kmh > 5)
    {
        snprintf(t, sizeof(t), "%.1f l/100", (double)(lph * 100 / kmh));
    }
    else
    {
        snprintf(t, sizeof(t), "%.2f l/h", (double)lph);
    }
    set_text(ui->fuel, t);
    lv_obj_set_hidden(ui->mil, !s->mil);
    set_text(ui->status, live ? "" : phase_text(s->phase));
}

/* ------------------------------------------------------------ live */

static void build_live(motore_ui_t *ui, const mt_state_t *s)
{
    lv_obj_t *page = ui_page(ui->root, "Dati live");
    memset(ui->live, 0, sizeof(ui->live));
    int rows = 0;
    for (int v = 0; v < MT_VALUES; v++)
    {
        const mt_value_info_t *info = mt_value_info((mt_value_t)v);
        bool has = s->updated[v] != 0 || (info->pid && (s->supported[(info->pid - 1) / 8] & (0x80 >> ((info->pid - 1) % 8))));
        if (!has)
        {
            continue;
        }
        char t[32];
        format_value(s, (mt_value_t)v, t, sizeof(t));
        lv_obj_t *row = ui_row(page, NULL, C_CYAN, info->name, t, NULL, NULL);
        ui->live[v] = ui_row_value(row);
        rows++;
    }
    if (rows == 0)
    {
        ui_text(page, "Nessun dato: collega l'adattatore e accendi il quadro.", true);
    }
}

static void update_live(motore_ui_t *ui, const mt_state_t *s)
{
    for (int v = 0; v < MT_VALUES; v++)
    {
        if (ui->live[v] != NULL)
        {
            char t[32];
            format_value(s, (mt_value_t)v, t, sizeof(t));
            lv_label_set_text(ui->live[v], t);
            lv_obj_set_style_text_color(ui->live[v], ui_color(mt_value_fresh(s, (mt_value_t)v) ? 0xFFFFFF : C_DIM), 0);
        }
    }
}

/* ------------------------------------------------------------ diagnosis */

static void clear_job(void *arg)
{
    (void)arg;
    char why[96];
    esp_err_t err = mt_clear_codes(why, sizeof(why));
    lv_port_lock();
    ui_toast(why);
    if (err == ESP_OK && s_ui != NULL)
    {
        s_ui->revision = 0;   /* redraw */
    }
    lv_port_unlock();
}

static void on_clear_confirmed(bool confirmed, void *arg)
{
    (void)arg;
    if (confirmed && !sys_post(clear_job, NULL))
    {
        ui_toast("Sistema occupato, riprova");
    }
}

static void ask_clear(void)
{
    ui_confirm("Cancellare i codici?",
               "Spegne la spia e azzera i test anti-inquinamento: l'auto li rifarà guidando. Auto ferma, motore spento, quadro acceso.",
               "Cancella", C_BAD, on_clear_confirmed, NULL);
}

static void on_pin_for_clear(const char *text, void *user_data)
{
    (void)user_data;
    if (text != NULL && mt_unlock_actions(text))
    {
        ask_clear();
    }
    else if (text != NULL)
    {
        ui_toast("Codice errato");
    }
}

static void on_clear(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    mt_lock();
    bool unlocked = mt_state()->unlocked;
    mt_unlock();
    if (st.pin[0] == 0)
    {
        ui_toast("Imposta prima un codice in Auto e sicurezza");
    }
    else if (!unlocked)
    {
        ui_text_input("Codice di sicurezza", "", true, on_pin_for_clear, NULL);
    }
    else
    {
        ask_clear();
    }
}

static void on_reread(lv_event_t *e)
{
    (void)e;
    mt_read_codes();
    ui_toast("Rileggo i codici...");
}

static void build_diag(motore_ui_t *ui, const mt_state_t *s)
{
    lv_obj_t *page = ui_page(ui->root, "Diagnosi");
    lv_obj_t *mil = ui_row(page, LV_SYMBOL_WARNING, s->mil ? C_BAD : C_GOOD, "Spia motore", s->mil ? "ACCESA" : "spenta", NULL, NULL);
    lv_obj_set_style_text_color(ui_row_value(mil), ui_color(s->mil ? C_BAD : C_GOOD), 0);
    ui_section(page, s->dtcs ? "Codici di errore" : "Codici di errore: nessuno");
    for (int i = 0; i < s->dtcs; i++)
    {
        char buf[96];
        const mt_dtc_t *d = &s->dtc[i];
        uint32_t color = d->kind == MT_DTC_PENDING ? C_WARN : C_BAD;
        ui_row(page, LV_SYMBOL_WARNING, color, d->code,
               d->kind == MT_DTC_PENDING ? "in attesa" : d->kind == MT_DTC_PERMANENT ? "permanente" : NULL, NULL, NULL);
        ui_text(page, mt_dtc_text(d->code, buf, sizeof(buf)), true);
    }
    if (s->freeze_code[0] != 0)
    {
        char head[48];
        snprintf(head, sizeof(head), "Fermo immagine (%s)", s->freeze_code);
        ui_section(page, head);
        for (int v = 0; v < MT_VALUES; v++)
        {
            if (!isnan(s->freeze[v]))
            {
                const mt_value_info_t *info = mt_value_info((mt_value_t)v);
                char t[32];
                snprintf(t, sizeof(t), "%.*f %s", info->decimals, (double)s->freeze[v], info->unit);
                ui_row(page, NULL, C_DIM, info->name, t, NULL, NULL);
            }
        }
    }
    bool any = false;
    for (int i = 0; i < MT_MONITORS; i++)
    {
        any |= s->monitor[i].name != NULL && s->monitor[i].supported;
    }
    if (any)
    {
        ui_section(page, "Test anti-inquinamento (readiness)");
        for (int i = 0; i < MT_MONITORS; i++)
        {
            if (s->monitor[i].name != NULL && s->monitor[i].supported)
            {
                lv_obj_t *r = ui_row(page, s->monitor[i].complete ? LV_SYMBOL_OK : LV_SYMBOL_REFRESH,
                                     s->monitor[i].complete ? C_GOOD : C_WARN, s->monitor[i].name,
                                     s->monitor[i].complete ? "pronto" : "da completare", NULL, NULL);
                (void)r;
            }
        }
    }
    ui_button(page, "Rileggi codici", C_CYAN, on_reread, NULL);
    ui_button(page, s->unlocked ? "Cancella codici" : LV_SYMBOL_EYE_CLOSE " Cancella codici", C_BAD, on_clear, NULL);
}

/* ------------------------------------------------------------ trip */

static void on_trip_reset(lv_event_t *e)
{
    (void)e;
    mt_trip_reset();
    ui_toast("Viaggio azzerato");
}

static void build_trip(motore_ui_t *ui, const mt_state_t *s)
{
    lv_obj_t *page = ui_page(ui->root, "Viaggio");
    const mt_trip_t *t = &s->trip;
    char v[32];
    snprintf(v, sizeof(v), "%.1f km", (double)t->km);
    ui_row(page, LV_SYMBOL_GPS, C_CYAN, "Distanza", v, NULL, NULL);
    snprintf(v, sizeof(v), "%lu:%02lu", (unsigned long)(t->seconds / 3600), (unsigned long)(t->seconds / 60 % 60));
    ui_row(page, LV_SYMBOL_LOOP, C_VIOLET, "Tempo a motore acceso", v, NULL, NULL);
    snprintf(v, sizeof(v), "%.2f l", (double)t->litres);
    ui_row(page, LV_SYMBOL_CHARGE, C_WARN, "Carburante stimato", v, NULL, NULL);
    if (t->km > 0.5f)
    {
        snprintf(v, sizeof(v), "%.1f l/100 km", (double)(t->litres * 100 / t->km));
        ui_row(page, LV_SYMBOL_CHARGE, C_GOOD, "Consumo medio", v, NULL, NULL);
        snprintf(v, sizeof(v), "%.1f km/l", (double)(t->km / (t->litres > 0.01f ? t->litres : 0.01f)));
        ui_row(page, LV_SYMBOL_CHARGE, C_GOOD, "Percorrenza", v, NULL, NULL);
    }
    if (t->seconds > 0)
    {
        snprintf(v, sizeof(v), "%.0f km/h", (double)(t->km * 3600 / t->seconds));
        ui_row(page, LV_SYMBOL_PLAY, C_CYAN, "Velocità media", v, NULL, NULL);
        snprintf(v, sizeof(v), "%lu%%", (unsigned long)(t->idle_seconds * 100 / t->seconds));
        ui_row(page, LV_SYMBOL_PAUSE, C_DIM, "Tempo al minimo", v, NULL, NULL);
    }
    snprintf(v, sizeof(v), "%.0f km/h", (double)t->max_speed);
    ui_row(page, LV_SYMBOL_UP, C_MAGENTA, "Velocità massima", v, NULL, NULL);
    snprintf(v, sizeof(v), "%.0f rpm", (double)t->max_rpm);
    ui_row(page, LV_SYMBOL_UP, C_MAGENTA, "Giri massimi", v, NULL, NULL);
    ui_button(page, "Azzera viaggio", C_CYAN, on_trip_reset, NULL);
}

/* ------------------------------------------------------------ link */

static mt_found_t s_found[MT_SCAN_MAX];

static void on_scan(lv_event_t *e)
{
    (void)e;
    mt_scan();
    ui_toast("Cerco adattatori per 8 secondi...");
}

static void on_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mt_choose_ble(&s_found[i]);
    ui_toast("Adattatore scelto: mi collego");
    go(s_ui, HOME);
}

static void on_link_kind(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    st.link = st.link == MT_LINK_WIFI ? MT_LINK_BLE : MT_LINK_WIFI;
    mt_set_settings(&st);
    go(s_ui, LINK);
}

static void on_host(const char *text, void *user_data)
{
    (void)user_data;
    if (text == NULL || text[0] == 0)
    {
        return;
    }
    mt_settings_t st;
    mt_settings(&st);
    char host[40];
    unsigned port = 35000;
    if (sscanf(text, "%39[^:]:%u", host, &port) >= 1)
    {
        snprintf(st.host, sizeof(st.host), "%s", host);
        st.port = (uint16_t)port;
        mt_set_settings(&st);
    }
    go(s_ui, LINK);
}

static void on_edit_host(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    char current[48];
    snprintf(current, sizeof(current), "%s:%u", st.host, st.port);
    ui_text_input("Indirizzo adattatore", current, false, on_host, NULL);
}

static void build_link(motore_ui_t *ui, const mt_state_t *s)
{
    lv_obj_t *page = ui_page(ui->root, "Collegamento");
    mt_settings_t st;
    mt_settings(&st);
    ui_row(page, st.link == MT_LINK_WIFI ? LV_SYMBOL_WIFI : LV_SYMBOL_BLUETOOTH, C_CYAN, "Tipo",
           st.link == MT_LINK_WIFI ? "Wi-Fi" : "Bluetooth LE", on_link_kind, NULL);
    if (s->adapter[0] != 0)
    {
        ui_row(page, LV_SYMBOL_USB, C_GOOD, "Adattatore", s->adapter, NULL, NULL);
    }
    if (s->protocol[0] != 0)
    {
        ui_text(page, s->protocol, true);
    }
    if (st.link == MT_LINK_WIFI)
    {
        char v[48];
        snprintf(v, sizeof(v), "%s:%u", st.host, st.port);
        ui_row(page, LV_SYMBOL_EDIT, C_VIOLET, "Indirizzo", v, on_edit_host, NULL);
        ui_text(page, "Collega prima il dispositivo alla rete Wi-Fi dell'adattatore (Impostazioni del sistema).", true);
        return;
    }
    ui_row(page, LV_SYMBOL_BLUETOOTH, C_CYAN, "Scelto", st.ble_name[0] ? st.ble_name : "nessuno", NULL, NULL);
    ui_button(page, mt_link_scanning() ? "Ricerca in corso..." : "Cerca adattatori", C_CYAN, on_scan, NULL);
    int n = mt_scan_results(s_found, MT_SCAN_MAX);
    ui->last_found = n;
    if (n > 0)
    {
        ui_section(page, "Trovati");
    }
    for (int i = 0; i < n; i++)
    {
        char v[16];
        snprintf(v, sizeof(v), "%d dBm", s_found[i].rssi);
        ui_row(page, s_found[i].likely ? LV_SYMBOL_OK : LV_SYMBOL_BLUETOOTH, s_found[i].likely ? C_GOOD : C_DIM,
               s_found[i].name[0] ? s_found[i].name : "(senza nome)", v, on_pick, (void *)(intptr_t)i);
    }
    if (n == 0 && !mt_link_scanning())
    {
        ui_text(page, "Inserisci l'adattatore nella presa OBD (sotto il volante), accendi il quadro e cerca.", true);
    }
}

/* ------------------------------------------------------------ car and safety */

static void on_fuel(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    st.fuel_type = (uint8_t)((st.fuel_type + 1) % MT_FUELS);
    mt_set_settings(&st);
    go(s_ui, CAR);
}

static void on_cc(const char *text, void *user_data)
{
    (void)user_data;
    if (text == NULL)
    {
        return;
    }
    mt_settings_t st;
    mt_settings(&st);
    st.displacement_cc = (uint16_t)atoi(text);
    mt_set_settings(&st);
    go(s_ui, CAR);
}

static void on_edit_cc(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    char v[8];
    snprintf(v, sizeof(v), "%u", st.displacement_cc);
    ui_text_input("Cilindrata (cc)", v, false, on_cc, NULL);
}

static void on_shift(const char *text, void *user_data)
{
    (void)user_data;
    if (text == NULL)
    {
        return;
    }
    mt_settings_t st;
    mt_settings(&st);
    st.shift_rpm = (uint16_t)atoi(text);
    mt_set_settings(&st);
    go(s_ui, CAR);
}

static void on_edit_shift(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    char v[8];
    snprintf(v, sizeof(v), "%u", st.shift_rpm);
    ui_text_input("Cambiata a (rpm)", v, false, on_shift, NULL);
}

static void on_new_pin(const char *text, void *user_data)
{
    (void)user_data;
    if (text == NULL)
    {
        return;
    }
    size_t len = strlen(text);
    if (len < 4 || len > 8 || strspn(text, "0123456789") != len)
    {
        ui_toast("Il codice deve avere da 4 a 8 cifre");
        return;
    }
    mt_settings_t st;
    mt_settings(&st);
    snprintf(st.pin, sizeof(st.pin), "%s", text);
    mt_set_settings(&st);
    ui_toast("Codice salvato");
    go(s_ui, CAR);
}

static void on_old_pin(const char *text, void *user_data)
{
    (void)user_data;
    mt_settings_t st;
    mt_settings(&st);
    if (text != NULL && strcmp(text, st.pin) == 0)
    {
        ui_text_input("Nuovo codice", "", true, on_new_pin, NULL);
    }
    else if (text != NULL)
    {
        ui_toast("Codice errato");
    }
}

static void on_pin(lv_event_t *e)
{
    (void)e;
    mt_settings_t st;
    mt_settings(&st);
    if (st.pin[0] == 0)
    {
        ui_text_input("Nuovo codice (4-8 cifre)", "", true, on_new_pin, NULL);
    }
    else
    {
        ui_text_input("Codice attuale", "", true, on_old_pin, NULL);
    }
}

static void on_unlock_typed(const char *text, void *user_data)
{
    (void)user_data;
    if (text != NULL)
    {
        ui_toast(mt_unlock_actions(text) ? "Sbloccato per 10 minuti" : "Codice errato");
        go(s_ui, CAR);
    }
}

static void on_lock_toggle(lv_event_t *e)
{
    (void)e;
    mt_lock();
    bool unlocked = mt_state()->unlocked;
    mt_unlock();
    if (unlocked)
    {
        mt_lock_actions();
        go(s_ui, CAR);
    }
    else
    {
        ui_text_input("Codice di sicurezza", "", true, on_unlock_typed, NULL);
    }
}

static void build_car(motore_ui_t *ui, const mt_state_t *s)
{
    static const char *const fuels[] = {"Benzina", "GPL", "Diesel"};
    lv_obj_t *page = ui_page(ui->root, "Auto e sicurezza");
    mt_settings_t st;
    mt_settings(&st);
    ui_section(page, "Auto");
    ui_row(page, LV_SYMBOL_CHARGE, C_WARN, "Carburante", fuels[st.fuel_type % MT_FUELS], on_fuel, NULL);
    char v[16];
    snprintf(v, sizeof(v), "%u cc", st.displacement_cc);
    ui_row(page, LV_SYMBOL_SETTINGS, C_DIM, "Cilindrata", v, on_edit_cc, NULL);
    ui_text(page, "Servono per stimare i consumi sulle auto senza debimetro. Con il GPL i litri sono di GPL.", true);
    snprintf(v, sizeof(v), "%u rpm", st.shift_rpm);
    ui_row(page, LV_SYMBOL_UP, C_MAGENTA, "Cambiata", v, on_edit_shift, NULL);
    ui_section(page, "Sicurezza");
    ui_row(page, LV_SYMBOL_EYE_CLOSE, C_VIOLET, "Codice", st.pin[0] ? "impostato" : "da impostare", on_pin, NULL);
    ui_row(page, s->unlocked ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE, s->unlocked ? C_WARN : C_GOOD, "Azioni critiche",
           s->unlocked ? "sbloccate" : "bloccate", on_lock_toggle, NULL);
    ui_text(page, "Le azioni che scrivono sull'auto (cancellare i codici) chiedono il codice, l'auto ferma e il motore spento. "
                  "Il blocco torna da solo dopo 10 minuti.", true);
}

/* ------------------------------------------------------------ navigation */

static void go(motore_ui_t *ui, view_t view)
{
    ui->view = view;
    if (ui->timer != NULL)
    {
        lv_timer_set_period(ui->timer, view == DRIVE ? DRIVE_MS : 200);
    }
    if (ui->root != NULL)
    {
        lv_obj_delete(ui->root);
    }
    ui->root = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->root);
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->root, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, 0);
    ui->arc = NULL;
    memset(ui->live, 0, sizeof(ui->live));
    mt_lock();
    mt_state_t *copy = heap_caps_malloc(sizeof(mt_state_t), MALLOC_CAP_SPIRAM);
    if (copy != NULL)
    {
        *copy = *mt_state();
    }
    mt_unlock();
    if (copy == NULL)
    {
        return;
    }
    ui->revision = copy->revision;
    switch (view)
    {
    case DRIVE: build_drive(ui); update_drive(ui, copy); break;
    case LIVE: build_live(ui, copy); break;
    case DIAG: build_diag(ui, copy); break;
    case TRIP: build_trip(ui, copy); break;
    case LINK: build_link(ui, copy); break;
    case CAR: build_car(ui, copy); break;
    case DASH: appkit_dashboard_page(ui_page(ui->root, "Dashboard web"), ui_color(C_CYAN)); break;
    default: build_home(ui, copy); break;
    }
    free(copy);
}

static void tick(lv_timer_t *timer)
{
    motore_ui_t *ui = lv_timer_get_user_data(timer);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
    {
        return;
    }
    /* Values change continuously: update in place; other views rebuild
     * only when something they show changes (not every value tick). */
    mt_lock();
    const mt_state_t *s = mt_state();
    if (ui->view == DRIVE && ui->arc != NULL)
    {
        update_drive(ui, s);
        mt_unlock();
        return;
    }
    if (ui->view == LIVE)
    {
        update_live(ui, s);
        mt_unlock();
        return;
    }
    bool changed = s->revision != ui->revision;
    mt_unlock();
    if (ui->view == LINK)
    {
        mt_found_t tmp[MT_SCAN_MAX];
        changed |= mt_scan_results(tmp, MT_SCAN_MAX) != ui->last_found;
    }
    /* Pages the user scrolls: rebuild at most every 2 s. */
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (changed && ui->view != DASH && now - last > 2000000)
    {
        last = now;
        go(ui, ui->view);
    }
}

bool motore_ui_back(void)
{
    motore_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return false;
    }
    if (ui->view == HOME)
    {
        extapp_return_to_launcher();
    }
    else
    {
        go(ui, HOME);
    }
    return true;
}

void motore_ui_create(lv_obj_t *screen)
{
    motore_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    go(ui, HOME);
    ui->timer = lv_timer_create(tick, 200, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA: "view home|drive|live|diag|trip|link|car|dash", "obd <cmd>". */
#include "core/devcon.h"

bool devcon_app_command(char *line)
{
    static const char *const views[] = {"home", "drive", "live", "diag", "trip", "link", "car", "dash"};
    if (strncmp(line, "obd ", 4) == 0)
    {
        char out[512];
        esp_err_t err = mt_console(line + 4, out, sizeof(out));
        printf("@OBD %s %s\n", esp_err_to_name(err), out);
        return true;
    }
    if (strncmp(line, "shift ", 6) == 0)
    {
        mt_settings_t st;
        mt_settings(&st);
        st.shift_rpm = (uint16_t)atoi(line + 6);
        mt_set_settings(&st);
        mt_settings(&st);
        printf("@OK shift %u\n", st.shift_rpm);
        return true;
    }
    if (strncmp(line, "link wifi ", 10) == 0)
    {
        mt_settings_t st;
        mt_settings(&st);
        unsigned port = 35000;
        char host[40];
        if (sscanf(line + 10, "%39[^:]:%u", host, &port) >= 1)
        {
            st.link = MT_LINK_WIFI;
            snprintf(st.host, sizeof(st.host), "%s", host);
            st.port = (uint16_t)port;
            mt_set_settings(&st);
            printf("@OK link wifi %s:%u\n", st.host, st.port);
        }
        return true;
    }
    if (strcmp(line, "status") == 0)
    {
        mt_lock();
        const mt_state_t *s = mt_state();
        printf("@MOTORE phase=%d rpm=%.0f speed=%.0f dtcs=%u vin=%s msg=%s\n", s->phase, (double)s->value[MT_RPM],
               (double)s->value[MT_SPEED], s->dtcs, s->vin, s->message);
        mt_unlock();
        return true;
    }
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
    {
        return false;
    }
    for (int v = 0; v < 8; v++)
    {
        if (strcmp(line + 5, views[v]) == 0)
        {
            lv_port_lock();
            go(s_ui, (view_t)v);
            lv_port_unlock();
            printf("@OK view %s\n", views[v]);
            return true;
        }
    }
    printf("@ERR unknown view\n");
    return true;
}
#endif
