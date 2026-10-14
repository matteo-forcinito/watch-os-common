/*
 * Giornata on the watch.
 *
 *   DAY      the day as a 24-hour ring: events are coloured arcs (overlaps on
 *            inner lanes), the part of the day already gone is lit, a dot is
 *            "now". In the middle: what is next ("tra 25 min") or running.
 *            Round panels: the ring fills the screen; rectangular: the ring on
 *            top and the next items below.
 *   AGENDA   the next 14 days, grouped by day.
 *   TASKS    today's tasks first, then the others; tap the circle = done.
 *   TASK     one task: notes, due time, focus done, "Avvia focus".
 *   FOCUS    the session ring and its time; pause, stop; break after.
 *   MENU     sync, calendars' state, dashboard.
 *   DASH     QR code + code for the web dashboard (appkit).
 * A reminder (agenda.c) covers any view with a card until it is dismissed.
 */
#include "giornata.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_BG     0x0A0C14
#define C_CARD   0x151927
#define C_LINE   0x262C40
#define C_INK    0xF2F4FA
#define C_DIM    0x8C93A8
#define C_FAINT  0x4A5168
#define C_ACCENT 0x7C8CFF
#define C_GOOD   0x2FCF7F
#define C_WARN   0xFFB547

enum { DAY, AGENDA, TASKS, TASK, FOCUS, MENU, DASH };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *alert;
    lv_timer_t *timer;
    int view;
    int back_to;
    uint32_t task;              /* TASK view */
    uint32_t revision;
    uint32_t alert_serial;
    int minute;                 /* redraw DAY every minute */
    /* FOCUS live widgets */
    lv_obj_t *focus_arc;
    lv_obj_t *focus_time;
    lv_obj_t *focus_state;
    gd_focus_state_t focus_shown;
} giornata_ui_t;

static giornata_ui_t *s_ui;
static void render(giornata_ui_t *ui);

static const char *const DAYS[] = {"lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato", "domenica"};
static const char *const MONTHS[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
                                     "agosto", "settembre", "ottobre", "novembre", "dicembre"};

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

static void hhmm(ics_time_t t, char *out, size_t size)
{
    snprintf(out, size, "%02d:%02d", (int)((t % 86400) / 3600), (int)((t % 3600) / 60));
}

static void go(giornata_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

static void open_view(lv_event_t *e)
{
    go(s_ui, (int)(intptr_t)lv_event_get_user_data(e));
}

static int content_width(void)
{
    const board_info_t *b = board_info();
    return b->round ? b->width * 74 / 100 : b->width - 24;
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int view, uint32_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 54, 54);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, UI_FONT_LARGE, color, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, open_view, LV_EVENT_CLICKED, (void *)(intptr_t)view);
    lv_port_set_focusable(b);
    return b;
}

/* "tra 25 min", "tra 2 h 10", "domani 9:00"... */
static void until_text(ics_time_t now, const gd_item_t *it, char *out, size_t size)
{
    char at[8];
    hhmm(it->start, at, sizeof(at));
    if (it->start <= now)
    {
        char end[8];
        hhmm(it->end, end, sizeof(end));
        snprintf(out, size, "ora · fino alle %s", end);
        return;
    }
    int64_t minutes = (it->start - now + 59) / 60;
    if (now / 86400 != it->start / 86400)
    {
        snprintf(out, size, "%s %s", it->start / 86400 == now / 86400 + 1 ? "domani" : DAYS[ics_weekday(it->start / 86400)], at);
    }
    else if (minutes < 60)
    {
        snprintf(out, size, "tra %d min · %s", (int)minutes, at);
    }
    else
    {
        snprintf(out, size, "tra %d h %02d · %s", (int)(minutes / 60), (int)(minutes % 60), at);
    }
}

/* ---------------------------------------------------------------- DAY */

#define LANES 3

static lv_obj_t *ring_arc(lv_obj_t *parent, int size, int width, uint32_t color, lv_opa_t opa, int from_deg, int to_deg)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 270);            /* 0 h at the top */
    lv_arc_set_bg_angles(a, (lv_value_precise_t)from_deg, (lv_value_precise_t)to_deg);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, ui_color(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_clickable(a, false);
    return a;
}

static int deg_of(ics_time_t t)
{
    return (int)((t % 86400) * 360 / 86400);
}

static void day_ring(lv_obj_t *parent, int size, ics_time_t now, const gd_item_t *items, int count)
{
    int width = size / 26 > 8 ? size / 26 : 8;
    lv_obj_t *ring = box(parent);
    lv_obj_set_size(ring, size, size);

    /* Track: the whole day, then the part already gone. */
    ring_arc(ring, size, width, C_LINE, LV_OPA_COVER, 0, 360);
    int now_deg = deg_of(now);
    if (now_deg > 1)
    {
        ring_arc(ring, size, width, 0x2E3550, LV_OPA_COVER, 0, now_deg);
    }

    /* Hour marks inside the ring. */
    static const char *const marks[] = {"0", "6", "12", "18"};
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *m = label(ring, UI_FONT_SMALL, C_FAINT, marks[i]);
        int r = size / 2 - width * 2 - 14;
        static const int8_t dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
        lv_obj_align(m, LV_ALIGN_CENTER, dx[i] * r, dy[i] * r);
    }

    /* Events: lanes from the outside in, so overlaps stay visible. */
    ics_time_t day_start = now / 86400 * 86400, day_end = day_start + 86400;
    ics_time_t lane_end[LANES] = {0};
    for (int i = 0; i < count; i++)
    {
        const gd_item_t *it = &items[i];
        if (it->all_day || it->end <= day_start || it->start >= day_end)
        {
            continue;
        }
        ics_time_t s = it->start < day_start ? day_start : it->start;
        ics_time_t e = it->end > day_end ? day_end - 1 : it->end;
        if (e - s < 15 * 60)
        {
            e = s + 15 * 60;   /* a reminder or a short call is still a visible tick */
        }
        int lane = 0;
        while (lane < LANES - 1 && lane_end[lane] > s)
        {
            lane++;
        }
        lane_end[lane] = e;
        int lane_size = size - lane * (width * 2 + 6);
        bool past = e <= now;
        ring_arc(ring, lane_size, width, it->color ? it->color : C_ACCENT, past ? LV_OPA_40 : LV_OPA_COVER,
                 deg_of(s), deg_of(e) <= deg_of(s) ? 360 : deg_of(e));
    }

    /* Now: a white dot on the ring. */
    lv_obj_t *dot = box(ring);
    int d = width + 8;
    lv_obj_set_size(dot, d, d);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, ui_color(C_INK), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(dot, 16, 0);
    lv_obj_set_style_shadow_color(dot, ui_color(C_INK), 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_50, 0);
    /* Position from the angle: sin/cos by LVGL's integer trigonometry. */
    int angle = (now_deg + 270) % 360;
    int r = size / 2 - width / 2;
    int x = (int)(lv_trigo_cos((int16_t)angle) * r / LV_TRIGO_SIN_MAX);
    int y = (int)(lv_trigo_sin((int16_t)angle) * r / LV_TRIGO_SIN_MAX);
    lv_obj_align(dot, LV_ALIGN_CENTER, x, y);
}

static void open_agenda(lv_event_t *e)
{
    (void)e;
    go(s_ui, AGENDA);
}

static void day(giornata_ui_t *ui)
{
    const board_info_t *b = board_info();
    ics_time_t now = gd_now();
    lv_obj_t *page = ui->content;

    gd_item_t *items = heap_caps_malloc(sizeof(gd_item_t) * GD_MAX_ITEMS, MALLOC_CAP_SPIRAM);
    if (items == NULL)
    {
        return;
    }
    gd_lock();
    int count = 0;
    const gd_item_t *all = gd_items(&count);
    memcpy(items, all, sizeof(gd_item_t) * count);
    int tasks_today = 0, tasks_done = 0, task_count = 0;
    const gd_task_t *tasks = gd_tasks(&task_count);
    for (int i = 0; i < task_count; i++)
    {
        tasks_today += tasks[i].column == GD_TODAY;
        tasks_done += tasks[i].column == GD_DONE && tasks[i].completed / 86400 == time(NULL) / 86400;
    }
    gd_unlock();

    int size = b->round ? (b->width < b->height ? b->width : b->height) - 12
                        : (b->width < b->height * 62 / 100 ? b->width : b->height * 62 / 100) - 20;
    lv_obj_t *ring_host = box(page);
    lv_obj_set_size(ring_host, size, size);
    lv_obj_align(ring_host, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 10);
    if (now == 0)
    {
        label(ring_host, UI_FONT_BODY, C_DIM, "Attendo l'ora dalla rete...");
        lv_obj_center(lv_obj_get_child(ring_host, 0));
        free(items);
        return;
    }
    day_ring(ring_host, size, now, items, count);

    /* Centre: date, next item, tasks. */
    lv_obj_t *centre = box(ring_host);
    lv_obj_set_size(centre, size * 62 / 100, LV_SIZE_CONTENT);
    lv_obj_center(centre);
    lv_obj_set_flex_flow(centre, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(centre, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(centre, 4, 0);
    lv_obj_set_clickable(centre, true);
    lv_obj_add_event_cb(centre, open_agenda, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(centre);

    int y, m, d;
    ics_civil_from_days(now / 86400, &y, &m, &d);
    char text[96];
    snprintf(text, sizeof(text), "%.3s %d %.3s", DAYS[ics_weekday(now / 86400)], d, MONTHS[m - 1]);
    for (char *c = text; *c; c++)
    {
        *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    }
    lv_obj_t *date = label(centre, UI_FONT_SMALL, C_DIM, text);
    lv_obj_set_style_text_letter_space(date, 1, 0);

    const gd_item_t *next = NULL, *running = NULL;
    for (int i = 0; i < count; i++)
    {
        if (items[i].all_day)
        {
            continue;
        }
        if (items[i].start <= now && items[i].end > now && running == NULL)
        {
            running = &items[i];
        }
        if (items[i].start > now && next == NULL)
        {
            next = &items[i];
        }
    }
    const gd_item_t *show = running != NULL ? running : next;
    if (show != NULL)
    {
        lv_obj_t *t = label(centre, size > 300 ? ui_font_title : UI_FONT_LARGE, C_INK, show->title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(t, size * 60 / 100, 0);
        lv_obj_set_style_max_height(t, 80, 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        until_text(now, show, text, sizeof(text));
        lv_obj_t *when = label(centre, UI_FONT_BODY, running == show ? C_GOOD : (show->color ? show->color : C_ACCENT), text);
        lv_label_set_long_mode(when, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(when, size * 60 / 100);
        lv_obj_set_style_text_align(when, LV_TEXT_ALIGN_CENTER, 0);
    }
    else
    {
        label(centre, ui_font_title, C_INK, "Giornata libera");
        label(centre, UI_FONT_BODY, C_DIM, "nessun impegno in vista");
    }
    /* All-day events of today. */
    int all_day = 0;
    for (int i = 0; i < count; i++)
    {
        if (items[i].all_day && items[i].start <= now && items[i].end > now && all_day < 1)
        {
            snprintf(text, sizeof(text), LV_SYMBOL_BELL " %s", items[i].title);
            lv_obj_t *a = label(centre, UI_FONT_SMALL, items[i].color ? items[i].color : C_WARN, text);
            lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_max_width(a, size * 58 / 100, 0);
            all_day++;
        }
    }
    snprintf(text, sizeof(text), LV_SYMBOL_OK " %d di %d attività", tasks_done, tasks_done + tasks_today);
    label(centre, UI_FONT_SMALL, tasks_today == 0 && tasks_done > 0 ? C_GOOD : C_DIM, text);

    /* Navigation. Round: three buttons inside the bottom of the ring; rectangle: under it, with the next items. */
    lv_obj_t *row = box(page);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    round_button(row, LV_SYMBOL_LIST, TASKS, C_GOOD);
    gd_focus_t f;
    gd_focus(&f);
    round_button(row, f.state == GD_FOCUS_IDLE ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, FOCUS,
                 f.state == GD_FOCUS_IDLE ? C_WARN : C_GOOD);
    round_button(row, LV_SYMBOL_SETTINGS, MENU, C_DIM);
    if (b->round)
    {
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -size / 9);
    }
    else
    {
        lv_obj_align_to(row, ring_host, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
        /* The next two items, compact. */
        lv_obj_t *list = box(page);
        lv_obj_set_size(list, b->width - 28, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 4, 0);
        lv_obj_align_to(list, row, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
        int shown = 0;
        for (int i = 0; i < count && shown < 2; i++)
        {
            if (items[i].all_day || items[i].end <= now || &items[i] == show)
            {
                continue;
            }
            char at[8];
            hhmm(items[i].start, at, sizeof(at));
            snprintf(text, sizeof(text), "%s  %s", at, items[i].title);
            lv_obj_t *l = label(list, UI_FONT_BODY, C_INK, text);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(l, b->width - 28);
            lv_obj_set_style_border_side(l, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(l, 4, 0);
            lv_obj_set_style_border_color(l, ui_color(items[i].color ? items[i].color : C_ACCENT), 0);
            lv_obj_set_style_pad_left(l, 8, 0);
            shown++;
        }
    }
    free(items);
}

/* -------------------------------------------------------------- AGENDA */

static void agenda(giornata_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Agenda");
    ics_time_t now = gd_now();
    gd_item_t *items = heap_caps_malloc(sizeof(gd_item_t) * GD_MAX_ITEMS, MALLOC_CAP_SPIRAM);
    if (items == NULL)
    {
        return;
    }
    gd_lock();
    int count = 0;
    memcpy(items, gd_items(&count), sizeof(gd_item_t) * GD_MAX_ITEMS);
    gd_items(&count);
    gd_unlock();
    int width = content_width();
    int64_t shown_day = -1;
    int shown = 0;
    char text[120];
    for (int i = 0; i < count; i++)
    {
        const gd_item_t *it = &items[i];
        if (it->end <= now && !(it->all_day && it->start / 86400 == now / 86400))
        {
            continue;
        }
        int64_t day = it->start / 86400 < now / 86400 ? now / 86400 : it->start / 86400;
        if (day != shown_day)
        {
            shown_day = day;
            int y, m, d;
            ics_civil_from_days(day, &y, &m, &d);
            if (day == now / 86400)
            {
                snprintf(text, sizeof(text), "OGGI");
            }
            else if (day == now / 86400 + 1)
            {
                snprintf(text, sizeof(text), "DOMANI");
            }
            else
            {
                snprintf(text, sizeof(text), "%s %d %s", DAYS[ics_weekday(day)], d, MONTHS[m - 1]);
                for (char *c = text; *c; c++)
                {
                    *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
                }
            }
            ui_section(page, text);
        }
        lv_obj_t *card = box(page);
        lv_obj_set_size(card, width, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, ui_color(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_style_pad_left(card, 16, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(card, 5, 0);
        lv_obj_set_style_border_color(card, ui_color(it->color ? it->color : C_ACCENT), 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 2, 0);
        lv_obj_t *title = label(card, UI_FONT_BODY, C_INK, it->title);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(title, width - 32);
        char a[8], b2[8];
        hhmm(it->start, a, sizeof(a));
        hhmm(it->end, b2, sizeof(b2));
        if (it->all_day)
        {
            snprintf(text, sizeof(text), "tutto il giorno%s%s", it->location[0] ? " · " : "", it->location);
        }
        else if (it->source == -2)
        {
            snprintf(text, sizeof(text), "%s · scadenza attività", a);
        }
        else
        {
            snprintf(text, sizeof(text), "%s – %s%s%s", a, b2, it->location[0] ? " · " : "", it->location);
        }
        lv_obj_t *sub = label(card, UI_FONT_SMALL, C_DIM, text);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(sub, width - 32);
        if (++shown >= 60)
        {
            break;
        }
    }
    if (shown == 0)
    {
        ui_text(page, "Nessun impegno nei prossimi 14 giorni. Collega i tuoi calendari dalla dashboard.", true);
    }
    free(items);
}

/* --------------------------------------------------------------- TASKS */

static void toggle_task(lv_event_t *e)
{
    gd_task_toggle((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void open_task(lv_event_t *e)
{
    s_ui->task = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    go(s_ui, TASK);
}

static void task_row(lv_obj_t *page, const gd_task_t *t, int width)
{
    lv_obj_t *row = lv_button_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_min_height(row, 56, 0);
    lv_obj_add_event_cb(row, open_task, LV_EVENT_CLICKED, (void *)(uintptr_t)t->id);
    lv_port_set_focusable(row);

    bool done = t->column == GD_DONE;
    lv_obj_t *check = lv_button_create(row);
    lv_obj_remove_style_all(check);
    lv_obj_set_size(check, 34, 34);
    lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(check, 3, 0);
    lv_obj_set_style_border_color(check, ui_color(done ? C_GOOD : t->priority ? C_WARN : C_FAINT), 0);
    lv_obj_set_style_bg_color(check, ui_color(C_GOOD), 0);
    lv_obj_set_style_bg_opa(check, done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_ext_click_area(check, 10);
    lv_obj_align(check, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(check, toggle_task, LV_EVENT_CLICKED, (void *)(uintptr_t)t->id);
    if (done)
    {
        lv_obj_t *mark = label(check, UI_FONT_BODY, C_BG, LV_SYMBOL_OK);
        lv_obj_center(mark);
    }

    lv_obj_t *text_col = box(row);
    lv_obj_set_size(text_col, width - 20 - 46, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text_col, 2, 0);
    lv_obj_align(text_col, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_t *title = label(text_col, UI_FONT_BODY, done ? C_FAINT : C_INK, t->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(title, width - 20 - 46);
    if (done)
    {
        lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }
    if (t->due || t->estimate)
    {
        char text[64] = "", at[8];
        int n = 0;
        if (t->due)
        {
            hhmm(t->due, at, sizeof(at));
            ics_time_t now = gd_now();
            n += snprintf(text, sizeof(text), "%s%s", t->due / 86400 == now / 86400 ? "oggi " :
                          t->due / 86400 == now / 86400 + 1 ? "domani " : "", at);
        }
        if (t->estimate)
        {
            snprintf(text + n, sizeof(text) - n, "%s%d/%d " LV_SYMBOL_PLAY, n ? " · " : "", t->sessions, t->estimate);
        }
        label(text_col, UI_FONT_SMALL, C_DIM, text);
    }
}

static void tasks_view(giornata_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Attività");
    gd_task_t *copy = heap_caps_malloc(sizeof(gd_task_t) * GD_MAX_TASKS, MALLOC_CAP_SPIRAM);
    if (copy == NULL)
    {
        return;
    }
    gd_lock();
    int count = 0;
    const gd_task_t *tasks = gd_tasks(&count);
    memcpy(copy, tasks, sizeof(gd_task_t) * count);
    gd_unlock();
    int width = content_width();
    static const char *const heads[] = {"DA FARE", "OGGI", "FATTE OGGI"};
    static const int order[] = {GD_TODAY, GD_TODO, GD_DONE};
    int shown = 0;
    for (int c = 0; c < 3; c++)
    {
        int column = order[c];
        int in_column = 0;
        for (uint16_t pos = 0; pos < GD_MAX_TASKS; pos++)
        {
            for (int i = 0; i < count; i++)
            {
                const gd_task_t *t = &copy[i];
                if (t->column != column || t->order != pos)
                {
                    continue;
                }
                if (column == GD_DONE && t->completed / 86400 != time(NULL) / 86400)
                {
                    continue;   /* only what was finished today */
                }
                if (in_column++ == 0)
                {
                    ui_section(page, heads[column]);
                }
                task_row(page, t, width);
                shown++;
            }
        }
    }
    if (shown == 0)
    {
        ui_text(page, "Nessuna attività. Aggiungile dalla dashboard: compaiono qui subito.", true);
    }
    free(copy);
}

/* ---------------------------------------------------------------- TASK */

static void start_focus(lv_event_t *e)
{
    gd_focus_start((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    go(s_ui, FOCUS);
}

static void toggle_and_back(lv_event_t *e)
{
    gd_task_toggle((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    go(s_ui, TASKS);
}

static void task_view(giornata_ui_t *ui)
{
    gd_task_t t;
    bool found = false;
    gd_lock();
    int count = 0;
    const gd_task_t *tasks = gd_tasks(&count);
    for (int i = 0; i < count; i++)
    {
        if (tasks[i].id == ui->task)
        {
            t = tasks[i];
            found = true;
        }
    }
    gd_unlock();
    lv_obj_t *page = ui_page(ui->content, "Attività");
    if (!found)
    {
        ui_text(page, "Attività eliminata.", true);
        return;
    }
    lv_obj_t *title = label(page, ui_font_title, C_INK, t.title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(title, content_width());
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    if (t.notes[0])
    {
        lv_obj_t *n = ui_text(page, t.notes, true);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    }
    char text[96];
    snprintf(text, sizeof(text), "%d sessioni di focus · %lu min", t.sessions, (unsigned long)t.focus_minutes);
    ui_text(page, text, true);
    if (t.due)
    {
        char at[8];
        hhmm(t.due, at, sizeof(at));
        int y, m, d;
        ics_civil_from_days(t.due / 86400, &y, &m, &d);
        snprintf(text, sizeof(text), "Scadenza %d %s alle %s", d, MONTHS[m - 1], at);
        ui_text(page, text, false);
    }
    lv_obj_t *b = ui_button(page, LV_SYMBOL_PLAY "  Avvia focus", C_WARN, start_focus, (void *)(uintptr_t)t.id);
    lv_port_set_focusable(b);
    b = ui_button(page, t.column == GD_DONE ? "Riapri" : LV_SYMBOL_OK "  Fatta", C_GOOD, toggle_and_back,
                  (void *)(uintptr_t)t.id);
    lv_port_set_focusable(b);
}

/* --------------------------------------------------------------- FOCUS */

static void focus_action(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    gd_focus_t f;
    gd_focus(&f);
    if (action == 0)
    {
        if (f.state == GD_FOCUS_RUNNING)
        {
            gd_focus_pause();
        }
        else if (f.state == GD_FOCUS_BREAK)
        {
            gd_focus_skip_break();
        }
        else
        {
            gd_focus_start(f.state == GD_FOCUS_PAUSED ? f.task : 0);
        }
    }
    else
    {
        gd_focus_stop();
    }
    render(s_ui);
}

static void action_button(lv_obj_t *parent, const char *symbol, uint32_t color, int action)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 68, 68);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *l = label(b, UI_FONT_LARGE, C_BG, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, focus_action, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    lv_port_set_focusable(b);
}

static void focus_update(giornata_ui_t *ui)
{
    if (ui->focus_arc == NULL)
    {
        return;
    }
    gd_focus_t f;
    gd_focus(&f);
    if (f.state != ui->focus_shown)
    {
        render(ui);
        return;
    }
    int32_t left = f.state == GD_FOCUS_IDLE ? gd_settings().focus_minutes * 60 : f.remaining_s;
    lv_arc_set_range(ui->focus_arc, 0, f.length_s > 0 ? f.length_s : 1);
    lv_arc_set_value(ui->focus_arc, f.length_s - left);
    lv_label_set_text_fmt(ui->focus_time, "%02d:%02d", (int)(left / 60), (int)(left % 60));
}

static void focus_view(giornata_ui_t *ui)
{
    const board_info_t *b = board_info();
    gd_focus_t f;
    gd_focus(&f);
    gd_settings_t s = gd_settings();
    ui->focus_shown = f.state;
    int size = b->round ? (b->width < b->height ? b->width : b->height) - 16 : b->width - 60;
    lv_obj_t *arc = lv_arc_create(ui->content);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 14);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    uint32_t color = f.state == GD_FOCUS_BREAK ? 0x5B8CFF : f.state == GD_FOCUS_PAUSED ? C_DIM : C_WARN;
    int width = size / 18;
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, ui_color(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_clickable(arc, false);
    ui->focus_arc = arc;

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, size * 70 / 100, LV_SIZE_CONTENT);
    lv_obj_align_to(col, arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    const char *state = f.state == GD_FOCUS_RUNNING ? "FOCUS" : f.state == GD_FOCUS_PAUSED ? "IN PAUSA" :
                        f.state == GD_FOCUS_BREAK ? "PAUSA" : "PRONTO";
    ui->focus_state = label(col, UI_FONT_SMALL, color, state);
    lv_obj_set_style_text_letter_space(ui->focus_state, 2, 0);
    ui->focus_time = label(col, ui_font_big, C_INK, "");
    if (f.state == GD_FOCUS_IDLE)
    {
        lv_label_set_text_fmt(ui->focus_time, "%02d:00", s.focus_minutes);
    }
    lv_obj_t *t = label(col, UI_FONT_BODY, C_DIM, f.title[0] ? f.title : f.state == GD_FOCUS_IDLE ?
                        "Senza attività" : "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(t, size * 62 / 100, 0);
    char text[48];
    snprintf(text, sizeof(text), "%d sessioni oggi", f.done_today);
    label(col, UI_FONT_SMALL, C_FAINT, text);

    lv_obj_t *row = box(ui->content);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (b->round)
    {
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -size / 12);
    }
    else
    {
        lv_obj_align_to(row, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
    const char *main_symbol = f.state == GD_FOCUS_RUNNING ? LV_SYMBOL_PAUSE : f.state == GD_FOCUS_BREAK ?
                              LV_SYMBOL_NEXT : LV_SYMBOL_PLAY;
    action_button(row, main_symbol, f.state == GD_FOCUS_BREAK ? 0x5B8CFF : C_WARN, 0);
    if (f.state == GD_FOCUS_RUNNING || f.state == GD_FOCUS_PAUSED)
    {
        action_button(row, LV_SYMBOL_STOP, 0x3A4058, 1);
    }
    focus_update(ui);
}

/* ---------------------------------------------------------------- MENU */

static void sync_now(lv_event_t *e)
{
    (void)e;
    gd_sync_now();
    ui_toast("Aggiorno i calendari...");
}

static void menu(giornata_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Giornata");
    lv_obj_t *b = ui_button(page, LV_SYMBOL_WIFI "  Dashboard", C_ACCENT, open_view, (void *)(intptr_t)DASH);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_LIST "  Agenda", 0x3A4058, open_view, (void *)(intptr_t)AGENDA);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_REFRESH "  Aggiorna calendari", 0x3A4058, sync_now, NULL);
    lv_port_set_focusable(b);
    gd_status_t st;
    gd_status(&st);
    ui_text(page, st.message, true);
    ui_section(page, "CALENDARI");
    gd_lock();
    int count = 0;
    const gd_source_t *sources = gd_sources(&count);
    char text[128];
    for (int i = 0; i < count; i++)
    {
        if (sources[i].error[0])
        {
            snprintf(text, sizeof(text), "%s: %s", sources[i].name, sources[i].error);
        }
        else
        {
            snprintf(text, sizeof(text), "%s: %d impegni%s", sources[i].name, sources[i].count,
                     sources[i].enabled ? "" : " (spento)");
        }
        lv_obj_t *t = ui_text(page, text, false);
        lv_obj_set_style_text_color(t, ui_color(sources[i].error[0] ? C_WARN : sources[i].color), 0);
    }
    gd_unlock();
    if (count == 0)
    {
        ui_text(page, "Nessuno. Nella dashboard incolla l'indirizzo segreto iCal di Google Calendar, iCloud o Outlook.", true);
    }
}

static void dash(giornata_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dashboard");
    appkit_dashboard_page(page, ui_color(C_ACCENT));
}

/* --------------------------------------------------------------- ALERT */

static void dismiss(lv_event_t *e)
{
    (void)e;
    gd_alert_dismiss();
    if (s_ui->alert != NULL)
    {
        lv_obj_delete(s_ui->alert);
        s_ui->alert = NULL;
    }
}

static void show_alert(giornata_ui_t *ui, const gd_alert_t *a)
{
    if (ui->alert != NULL)
    {
        lv_obj_delete(ui->alert);
    }
    const board_info_t *b = board_info();
    lv_obj_t *o = box(ui->screen);
    ui->alert = o;
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, true);
    lv_obj_t *halo = box(o);
    int size = (b->width < b->height ? b->width : b->height) * 80 / 100;
    lv_obj_set_size(halo, size, size);
    lv_obj_center(halo);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(halo, 6, 0);
    lv_obj_set_style_border_color(halo, ui_color(a->color ? a->color : C_ACCENT), 0);
    lv_obj_set_style_shadow_width(halo, 40, 0);
    lv_obj_set_style_shadow_color(halo, ui_color(a->color ? a->color : C_ACCENT), 0);
    lv_obj_set_style_shadow_opa(halo, LV_OPA_40, 0);
    lv_obj_t *col = box(o);
    lv_obj_set_size(col, size * 78 / 100, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    label(col, UI_FONT_LARGE, a->color ? a->color : C_ACCENT, LV_SYMBOL_BELL);
    lv_obj_t *t = label(col, ui_font_title, C_INK, a->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, size * 74 / 100);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *d = label(col, UI_FONT_BODY, C_DIM, a->detail);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(d, size * 74 / 100);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *ok = ui_button(col, "OK", a->color ? a->color : C_ACCENT, dismiss, NULL);
    lv_port_set_focusable(ok);
}

/* --------------------------------------------------------------- frame */

static void render(giornata_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->focus_arc = NULL;
    ui->content = box(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    ui->revision = gd_revision();
    ics_time_t now = gd_now();
    ui->minute = (int)(now / 60);
    switch (ui->view)
    {
    case DAY: day(ui); break;
    case AGENDA: agenda(ui); break;
    case TASKS: tasks_view(ui); break;
    case TASK: task_view(ui); break;
    case FOCUS: focus_view(ui); break;
    case MENU: menu(ui); break;
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    giornata_ui_t *ui = lv_timer_get_user_data(timer);
    gd_alert_t a;
    gd_alert(&a);
    if (a.active && a.serial != ui->alert_serial)
    {
        ui->alert_serial = a.serial;
        show_alert(ui, &a);
    }
    else if (!a.active && ui->alert != NULL)
    {
        lv_obj_delete(ui->alert);
        ui->alert = NULL;
    }
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
    {
        return;
    }
    if (ui->view == FOCUS)
    {
        focus_update(ui);
    }
    bool new_minute = (int)(gd_now() / 60) != ui->minute;
    if (ui->view != DASH && ui->view != FOCUS && (gd_revision() != ui->revision || (ui->view == DAY && new_minute)))
    {
        render(ui);
    }
}

bool giornata_ui_back(void)
{
    giornata_ui_t *ui = s_ui;
    if (ui->alert != NULL)
    {
        gd_alert_dismiss();
        lv_obj_delete(ui->alert);
        ui->alert = NULL;
        return true;
    }
    if (ui->view == DAY)
    {
        extapp_return_to_launcher();
    }
    else
    {
        go(ui, ui->view == TASK ? TASKS : ui->view == DASH ? MENU : DAY);
    }
    return true;
}

void giornata_ui_create(lv_obj_t *screen)
{
    giornata_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->timer = lv_timer_create(tick, 500, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA only: "view day|agenda|tasks|task|focus|menu|dash", "demo" (sample tasks/events), "alert", "code". */
#include "core/devcon.h"

static void demo(void)
{
    ics_time_t now = gd_now();
    ics_time_t today = now / 86400 * 86400;
    gd_task_add("Preparare la demo del firmware", GD_TODAY, today + 17 * 3600, 3, 1);
    gd_task_add("Rispondere a Luca sul preventivo", GD_TODAY, 0, 1, 0);
    gd_task_add("Comprare il regalo", GD_TODO, today + 86400 + 18 * 3600, 0, 0);
    gd_task_add("Prenotare il tagliando", GD_TODO, 0, 0, 0);
    static const struct { int h, m, len; const char *title; uint32_t color; } ev[] = {
        {8, 30, 30, "Palestra", 0x2FCF7F}, {10, 0, 60, "Stand-up prodotto", 0x7C8CFF},
        {11, 30, 45, "Call fornitore", 0xFF6B9A}, {13, 0, 60, "Pranzo con Sara", 0xFFB547},
        {15, 0, 120, "Revisione progetto", 0x7C8CFF}, {19, 30, 90, "Calcetto", 0x2FCF7F},
    };
    for (size_t i = 0; i < sizeof(ev) / sizeof(ev[0]); i++)
    {
        gd_event_t e = {0};
        snprintf(e.title, sizeof(e.title), "%s", ev[i].title);
        e.start = today + ev[i].h * 3600 + ev[i].m * 60;
        e.end = e.start + ev[i].len * 60;
        e.color = ev[i].color;
        e.remind = -1;
        gd_event_put(&e);
    }
    gd_event_t trip = {0};
    snprintf(trip.title, sizeof(trip.title), "Ferie a Lisbona");
    trip.start = today + 3 * 86400;
    trip.end = today + 6 * 86400;
    trip.all_day = true;
    trip.color = 0x4FB3FF;
    trip.remind = -1;
    gd_event_put(&trip);
}

bool devcon_app_command(char *line)
{
    static const char *const views[] = {"day", "agenda", "tasks", "task", "focus", "menu", "dash"};
    if (strcmp(line, "demo") == 0)
    {
        demo();
        printf("@OK demo\n");
        return true;
    }
    if (strcmp(line, "code") == 0)
    {
        printf("@CODE %s\n", appkit_web_code());
        return true;
    }
    if (strcmp(line, "focus") == 0)
    {
        gd_focus_start(0);
        printf("@OK focus\n");
        return true;
    }
    if (strncmp(line, "view ", 5) != 0 || s_ui == NULL)
    {
        return false;
    }
    for (int v = 0; v < 7; v++)
    {
        if (strcmp(line + 5, views[v]) == 0)
        {
            lv_port_lock();
            if (v == TASK && s_ui->task == 0)
            {
                gd_lock();
                int count = 0;
                const gd_task_t *t = gd_tasks(&count);
                s_ui->task = count > 0 ? t[0].id : 0;
                gd_unlock();
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
