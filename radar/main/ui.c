/*
 * Radar UI:
 *
 *   MAP      the radar loop over the dark map, full screen. You are the dot in
 *            the middle, with 10 and 25 km rings for scale. A time pill on top,
 *            the loop scrubber and the verdict at the bottom ("Pioggia tra
 *            25 min, debole" / "Pioggia a 18 km da ovest, arriva tra ~35 min").
 *            Tap: pause / play. Swipe up: details. Knob: zoom.
 *   DETAILS  verdict, next two hours (15-minute bars), next 12 hours
 *            (probability), zoom, refresh, sources.
 *
 * The picture is one RGB565 buffer recomposed only when the loop steps
 * (base + frame, a few ms), shown by an lv_image.
 */
#include "radar.h"

#include "core/app.h"
#include "core/clock.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "sdkconfig.h"
#include "ui/ui.h"
#include "misc/cache/instance/lv_image_cache.h" /* drop the cached picture after recomposing it */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { MAP, DETAILS };

#define C_INK 0xEEF3FA
#define C_MIST 0x93A1B8
#define C_CARD 0x0A1220
#define C_RAIN 0x4FB3FF
#define C_DRY 0x7FD1A8

#define STEP_MS 420
#define HOLD_MS 1700

typedef struct
{
    lv_obj_t *screen, *content, *page;
    lv_timer_t *timer;
    int view;
    /* map */
    lv_obj_t *image, *pill, *verdict, *detail, *scrubber, *border, *hint, *rings[2], *ring_labels[2];
    lv_obj_t *ticks[RADAR_FRAMES];
    lv_image_dsc_t dsc;
    uint16_t *pixels;
    int frame, shown_frame;
    bool playing;
    uint32_t next_step;
    uint32_t revision, base_revision;
    /* details */
    lv_obj_t *d_verdict, *d_detail, *bars[RADAR_NOWCAST], *bar_labels[RADAR_NOWCAST], *hours[RADAR_HOURS];
    radar_status_t status;
} radar_ui_t;

static radar_ui_t *s_ui;
static void render(radar_ui_t *ui);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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

static const char *direction_word(float degrees)
{
    static const char *const words[] = {"nord", "nord-est", "est", "sud-est", "sud", "sud-ovest", "ovest", "nord-ovest"};
    return words[((int)((degrees + 22.5f) / 45.0f)) & 7];
}

static const char *intensity_word(float mm_per_15min)
{
    float per_hour = mm_per_15min * 4;
    return per_hour < 2.5f ? "debole" : per_hour < 8 ? "moderata" : per_hour < 25 ? "forte" : "molto forte";
}

/* The sentence of the app: what to expect, from the nowcast and the radar itself. */
static void verdict(const radar_status_t *s, char *title, size_t title_size, char *line, size_t line_size)
{
    time_t now = time(NULL);
    bool clock_ok = clock_is_valid();
    line[0] = 0;
    if (!s->located)
    {
        snprintf(title, title_size, "Dove sei?");
        snprintf(line, line_size, "Imposta la citta' nel Meteo dell'orologio");
        return;
    }
    if (s->frames == 0)
    {
        snprintf(title, title_size, "Radar in arrivo");
        snprintf(line, line_size, "%s", s->message[0] ? s->message : "Scarico la mappa e le immagini...");
        return;
    }
    bool raining = s->rain_here > 0.12f || (s->cell_valid && s->cell_km < 2.5f);
    if (raining)
    {
        snprintf(title, title_size, "Sta piovendo");
        if (s->nowcast_valid && clock_ok)
        {
            int dry = -1;
            for (int i = 0; i < RADAR_NOWCAST && dry < 0; i++)
                if (s->rain_mm[i] < 0.05f && s->nowcast_start + i * 900 > now)
                    dry = i;
            if (dry >= 0)
            {
                time_t t = (time_t)(s->nowcast_start + dry * 900);
                struct tm local;
                localtime_r(&t, &local);
                snprintf(line, line_size, "Dovrebbe smettere verso le %02d:%02d", local.tm_hour, local.tm_min);
            }
            else
                snprintf(line, line_size, "Continua per almeno due ore");
        }
        return;
    }
    if (s->nowcast_valid && clock_ok)
        for (int i = 0; i < RADAR_NOWCAST; i++)
        {
            int64_t start = s->nowcast_start + i * 900;
            if (s->rain_mm[i] >= 0.1f && start + 900 > now)
            {
                int minutes = (int)((start - now) / 60);
                if (minutes <= 2)
                    snprintf(title, title_size, "Pioggia a minuti");
                else
                    snprintf(title, title_size, "Pioggia tra %d min", minutes);
                snprintf(line, line_size, "%s%s%s", intensity_word(s->rain_mm[i]),
                         s->cell_valid ? ", da " : "", s->cell_valid ? direction_word(s->cell_bearing) : "");
                return;
            }
        }
    if (s->cell_valid && s->cell_km < 80)
    {
        snprintf(title, title_size, "Pioggia a %d km", (int)(s->cell_km + 0.5f));
        if (s->cell_speed_kmh > 4)
        {
            int eta = (int)(s->cell_km / s->cell_speed_kmh * 60.0f);
            snprintf(line, line_size, "verso %s, si avvicina: ~%d min", direction_word(s->cell_bearing), eta);
        }
        else if (s->cell_speed_kmh < -4)
            snprintf(line, line_size, "verso %s, si allontana", direction_word(s->cell_bearing));
        else
            snprintf(line, line_size, "verso %s, quasi ferma", direction_word(s->cell_bearing));
        return;
    }
    snprintf(title, title_size, "Niente pioggia in vista");
    int best = 0, at = 0;
    for (int i = 0; i < RADAR_HOURS; i++)
        if (s->hour_probability[i] > best)
        {
            best = s->hour_probability[i];
            at = i;
        }
    if (best >= 20)
        snprintf(line, line_size, "Probabilita' %d%% verso le %02d", best, (s->hour_start + at) % 24);
    else
        snprintf(line, line_size, "Cielo tranquillo nelle prossime 12 ore");
}

/* ------------------------------------------------------------ map */

static void map_event(lv_event_t *e)
{
    radar_ui_t *ui = s_ui;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE)
    {
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP)
        {
            lv_indev_wait_release(input);
            ui->view = DETAILS;
            render(ui);
        }
        return;
    }
    ui->playing = !ui->playing;
    lv_label_set_text(ui->hint, ui->playing ? "" : LV_SYMBOL_PAUSE "  in pausa");
    ui->next_step = now_ms();
}

static void map_knob(lv_event_t *e)
{
    lv_port_knob_t *k = lv_event_get_param(e);
    radar_ui_t *ui = s_ui;
    if (ui->view != MAP)
        return;
    int zoom = ui->status.zoom + (k->delta > 0 ? 1 : -1);
    if (zoom >= 0 && zoom < RADAR_ZOOM_COUNT)
    {
        radar_set_zoom((radar_zoom_t)zoom);
        ui_toast(zoom == RADAR_ZOOM_NEAR ? "Vicino" : zoom == RADAR_ZOOM_REGION ? "Regione" : "Italia");
    }
    k->handled = true;
}

static lv_obj_t *ring(lv_obj_t *parent, int cx, int cy, int r)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, ui_color(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, 45, 0);
    return o;
}

static void map(radar_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height;
    lv_obj_t *c = ui->content;
    ui->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ui->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ui->dsc.header.w = w;
    ui->dsc.header.h = h;
    ui->dsc.header.stride = w * 2;
    ui->dsc.data = (const uint8_t *)ui->pixels;
    ui->dsc.data_size = (uint32_t)w * h * 2;
    radar_base(ui->pixels);
    ui->image = lv_image_create(c);
    lv_image_set_src(ui->image, &ui->dsc);
    lv_obj_set_pos(ui->image, 0, 0);
    lv_obj_set_clickable(ui->image, false);
    ui->shown_frame = -1;

    for (int i = 0; i < 2; i++)
    {
        ui->rings[i] = ring(c, w / 2, h / 2, 10);
        ui->ring_labels[i] = label(c, UI_FONT_SMALL, 0xFFFFFF, "");
        lv_obj_set_style_text_opa(ui->ring_labels[i], 150, 0);
    }
    lv_obj_t *halo = ring(c, w / 2, h / 2, 9);
    lv_obj_set_style_border_opa(halo, 160, 0);
    lv_obj_t *me = plain(c);
    lv_obj_set_size(me, 7, 7);
    lv_obj_set_pos(me, w / 2 - 3, h / 2 - 3);
    lv_obj_set_style_radius(me, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(me, ui_color(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(me, LV_OPA_COVER, 0);

    /* Time pill. */
    ui->pill = label(c, UI_FONT_SMALL, C_INK, "");
    lv_obj_set_style_bg_color(ui->pill, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(ui->pill, 200, 0);
    lv_obj_set_style_pad_hor(ui->pill, 12, 0);
    lv_obj_set_style_pad_ver(ui->pill, 4, 0);
    lv_obj_set_style_radius(ui->pill, 12, 0);
    lv_obj_align(ui->pill, LV_ALIGN_TOP_MID, 0, b->round ? h * 9 / 100 : 10);

    /* Verdict card + scrubber. */
    lv_obj_t *card = plain(c);
    lv_obj_set_size(card, b->round ? w * 70 / 100 : w - 20, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, 215, 0);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 9 / 100 : -10);
    ui->scrubber = plain(card);
    lv_obj_set_size(ui->scrubber, LV_PCT(80), 6);
    lv_obj_set_flex_flow(ui->scrubber, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ui->scrubber, 3, 0);
    for (int i = 0; i < RADAR_FRAMES; i++)
    {
        ui->ticks[i] = plain(ui->scrubber);
        lv_obj_set_flex_grow(ui->ticks[i], 1);
        lv_obj_set_height(ui->ticks[i], 4);
        lv_obj_set_style_radius(ui->ticks[i], 2, 0);
        lv_obj_set_style_bg_color(ui->ticks[i], ui_color(0x3A4A64), 0);
        lv_obj_set_style_bg_opa(ui->ticks[i], LV_OPA_COVER, 0);
    }
    ui->verdict = label(card, ui_font_title, C_INK, "");
    ui->detail = label(card, UI_FONT_SMALL, C_MIST, "");
    lv_obj_t *lines[] = {ui->verdict, ui->detail};
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_width(lines[i], LV_PCT(100));
        lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    ui->hint = label(c, UI_FONT_SMALL, C_INK, "");
    lv_obj_align(ui->hint, LV_ALIGN_TOP_MID, 0, b->round ? h * 17 / 100 : 66);
    /* OpenStreetMap asks for visible credit on the map. */
    lv_obj_t *credit = label(c, UI_FONT_SMALL, 0xFFFFFF, "\xC2\xA9 OpenStreetMap");
    lv_obj_set_style_text_opa(credit, 110, 0);
    lv_obj_align(credit, LV_ALIGN_TOP_MID, 0, b->round ? h * 3 / 100 : 42);

    lv_obj_set_clickable(c, true);
    lv_obj_set_gesture_bubble(c, false);
    lv_obj_add_event_cb(c, map_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(c, map_event, LV_EVENT_GESTURE, NULL);
    lv_port_set_focusable(c);
}

static void map_text(radar_ui_t *ui)
{
    char title[64], line[96];
    verdict(&ui->status, title, sizeof(title), line, sizeof(line));
    lv_label_set_text(ui->verdict, title);
    lv_label_set_text(ui->detail, line);
    lv_obj_set_hidden(ui->detail, !line[0]);
    /* Scale rings: 10 and 25 km (50/100 when zoomed out). */
    const board_info_t *b = board_info();
    int w = b->width, h = b->height;
    static const int steps[RADAR_ZOOM_COUNT][2] = {{10, 25}, {25, 75}, {50, 150}};
    for (int i = 0; i < 2; i++)
    {
        int km = steps[ui->status.zoom][i];
        int r = ui->status.km_per_pixel > 0 ? (int)(km / ui->status.km_per_pixel) : 0;
        bool show = r > 12 && r < (w < h ? w : h) / 2 - 4;
        lv_obj_set_hidden(ui->rings[i], !show);
        lv_obj_set_hidden(ui->ring_labels[i], !show);
        if (!show)
            continue;
        lv_obj_set_size(ui->rings[i], r * 2, r * 2);
        lv_obj_set_pos(ui->rings[i], w / 2 - r, h / 2 - r);
        char text[16];
        snprintf(text, sizeof(text), "%d km", km);
        lv_label_set_text(ui->ring_labels[i], text);
        lv_obj_set_pos(ui->ring_labels[i], w / 2 + (int)(r * 0.71f) + 2, h / 2 - (int)(r * 0.71f) - 16);
    }
}

static void map_step(radar_ui_t *ui)
{
    int frames = ui->status.frames;
    uint32_t now = now_ms();
    if (frames == 0)
    {
        if (ui->base_revision != ui->status.revision)
        {
            ui->base_revision = ui->status.revision;
            radar_base(ui->pixels);
            lv_image_cache_drop(&ui->dsc);
            lv_obj_invalidate(ui->image);
        }
        lv_label_set_text(ui->pill, ui->status.loading ? "Caricamento..." : "");
        return;
    }
    if (ui->frame >= frames)
        ui->frame = frames - 1;
    if (ui->playing && (int32_t)(now - ui->next_step) >= 0)
    {
        ui->frame = (ui->frame + 1) % frames;
        ui->next_step = now + (ui->frame == frames - 1 ? HOLD_MS : STEP_MS);
    }
    if (ui->frame == ui->shown_frame && ui->revision == ui->status.revision)
        return;
    ui->shown_frame = ui->frame;
    ui->revision = ui->status.revision;
    if (!radar_compose(ui->frame, ui->pixels))
        return;
    lv_image_cache_drop(&ui->dsc);
    lv_obj_invalidate(ui->image);
    int64_t t = ui->status.frame_time[ui->frame];
    int minutes = (int)((ui->status.newest - t) / 60);
    time_t local_time = (time_t)t;
    struct tm local;
    localtime_r(&local_time, &local);
    char text[40];
    if (minutes == 0)
        snprintf(text, sizeof(text), "%02d:%02d  \xC2\xB7  ultima", local.tm_hour, local.tm_min);
    else
        snprintf(text, sizeof(text), "%02d:%02d  \xC2\xB7  -%d min", local.tm_hour, local.tm_min, minutes);
    lv_label_set_text(ui->pill, text);
    for (int i = 0; i < RADAR_FRAMES; i++)
    {
        lv_obj_set_hidden(ui->ticks[i], i >= frames);
        lv_obj_set_style_bg_color(ui->ticks[i], ui_color(i == ui->frame ? C_RAIN : 0x3A4A64), 0);
    }
}

/* ------------------------------------------------------------ details */

static void choose_zoom(lv_event_t *e)
{
    radar_set_zoom((radar_zoom_t)(intptr_t)lv_event_get_user_data(e));
    s_ui->view = MAP;
    render(s_ui);
}

static void details_action(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 1)
    {
        radar_refresh_now();
        ui_toast("Aggiorno il radar");
    }
    else
        extapp_return_to_launcher();
}

static void bar_chart(lv_obj_t *page, lv_obj_t **bars, lv_obj_t **labels, int count, int height)
{
    lv_obj_t *row = plain(page);
    lv_obj_set_size(row, LV_PCT(100), height + 20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *col = plain(row);
        lv_obj_set_size(col, LV_SIZE_CONTENT, height + 20);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        bars[i] = plain(col);
        lv_obj_set_size(bars[i], count > 8 ? 10 : 16, 2);
        lv_obj_set_style_radius(bars[i], 3, 0);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        if (labels)
        {
            labels[i] = label(col, UI_FONT_SMALL, C_MIST, "");
            lv_obj_set_style_text_font(labels[i], &lv_font_montserrat_12, 0);
        }
    }
}

static void details(radar_ui_t *ui)
{
    lv_obj_t *page = ui->page;
    ui_title(page, "Radar");
    ui->d_verdict = label(page, ui_font_headline, C_INK, "");
    ui->d_detail = ui_text(page, "", true);
    ui_section(page, "PROSSIME 2 ORE");
    bar_chart(page, ui->bars, ui->bar_labels, RADAR_NOWCAST, 60);
    ui_section(page, "PROSSIME 12 ORE");
    bar_chart(page, ui->hours, NULL, RADAR_HOURS, 40);
    ui_section(page, "ZOOM");
    static const char *const names[] = {"Vicino (~170 km)", "Regione (~340 km)", "Italia (~700 km)"};
    for (int z = 0; z < RADAR_ZOOM_COUNT; z++)
        ui_row(page, z == ui->status.zoom ? LV_SYMBOL_OK : LV_SYMBOL_GPS, z == ui->status.zoom ? C_DRY : C_MIST,
               names[z], NULL, choose_zoom, (void *)(intptr_t)z);
    ui_button(page, "Aggiorna", 0x1C3A5E, details_action, (void *)1);
    ui_text(page,
            "Radar RainViewer (ultime 2 ore, ogni 10 min), mappa \xC2\xA9 OpenStreetMap, previsione Open-Meteo. "
            "L'arrivo stimato confronta la pioggia piu' vicina ora e 30 minuti fa.",
            true);
    ui_button(page, "Esci", 0x1B2638, details_action, (void *)2);
}

static void details_update(radar_ui_t *ui)
{
    char title[64], line[96];
    verdict(&ui->status, title, sizeof(title), line, sizeof(line));
    lv_label_set_text(ui->d_verdict, title);
    lv_label_set_text(ui->d_detail, line);
    for (int i = 0; i < RADAR_NOWCAST; i++)
    {
        float mm = ui->status.nowcast_valid ? ui->status.rain_mm[i] : 0;
        int height = 2 + (int)(mm * 4 / 10.0f * 58); /* 10 mm/h fills the bar */
        lv_obj_set_height(ui->bars[i], height > 60 ? 60 : height);
        lv_obj_set_style_bg_color(ui->bars[i], ui_color(mm >= 0.1f ? C_RAIN : 0x2A3A52), 0);
        time_t t = (time_t)(ui->status.nowcast_start + i * 900);
        struct tm local;
        localtime_r(&t, &local);
        char text[8];
        snprintf(text, sizeof(text), i % 2 == 0 ? "%02d:%02d" : "", local.tm_hour, local.tm_min);
        lv_label_set_text(ui->bar_labels[i], ui->status.nowcast_valid ? text : "");
    }
    for (int i = 0; i < RADAR_HOURS; i++)
    {
        int p = ui->status.hour_probability[i];
        lv_obj_set_height(ui->hours[i], 2 + p * 38 / 100);
        lv_obj_set_style_bg_color(ui->hours[i], ui_color(p >= 50 ? C_RAIN : p >= 20 ? 0x3F6F9E : 0x2A3A52), 0);
    }
}

/* ------------------------------------------------------------ frame */

static void render(radar_ui_t *ui)
{
    if (ui->content)
        lv_obj_delete(ui->content);
    ui->content = plain(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(0x05080F), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    radar_status(&ui->status);
    if (ui->view == MAP)
    {
        map(ui);
        map_text(ui);
        map_step(ui);
        return;
    }
    ui->page = ui_page(ui->content, NULL);
    details(ui);
    details_update(ui);
}

static void tick(lv_timer_t *timer)
{
    radar_ui_t *ui = lv_timer_get_user_data(timer);
    uint32_t revision = ui->status.revision;
    radar_status(&ui->status);
    lv_obj_set_hidden(ui->border, !ui->status.loading);
    if (ui->status.loading)
        ui_border_progress_set(ui->border, ui->status.progress, 100);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
        return;
    if (ui->view == MAP)
    {
        map_step(ui);
        if (revision != ui->status.revision)
            map_text(ui);
    }
    else if (revision != ui->status.revision)
        details_update(ui);
}

bool radar_ui_back(void)
{
    radar_ui_t *ui = s_ui;
    if (ui->view == MAP)
        extapp_return_to_launcher();
    else
    {
        ui->view = MAP;
        render(ui);
    }
    return true;
}

void radar_ui_create(lv_obj_t *screen)
{
    const board_info_t *b = board_info();
    radar_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ui->pixels = heap_caps_malloc((size_t)b->width * b->height * 2, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui->pixels == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    ui->playing = true;
    lv_obj_set_style_bg_color(screen, ui_color(0x05080F), 0);
    lv_obj_add_event_cb(screen, map_knob, lv_port_knob_event(), NULL);
    ui->border = ui_border_progress_create(screen, C_RAIN);
    render(ui);
    ui->timer = lv_timer_create(tick, 50, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* QA only: "view map|details", "zoom 0..2", "frame n" (pauses on that frame). */
#include "core/devcon.h"
#include <stdlib.h>

bool devcon_app_command(char *line)
{
    if (s_ui == NULL)
        return false;
    lv_port_lock();
    bool handled = true;
    if (strcmp(line, "view map") == 0 || strcmp(line, "view details") == 0)
    {
        s_ui->view = strcmp(line, "view map") == 0 ? MAP : DETAILS;
        render(s_ui);
    }
    else if (strncmp(line, "loc ", 4) == 0)
    {
        char *end = NULL;
        float lat = strtof(line + 4, &end);
        radar_test_location(lat, strtof(end, NULL));
    }
    else if (strncmp(line, "zoom ", 5) == 0)
        radar_set_zoom((radar_zoom_t)atoi(line + 5));
    else if (strncmp(line, "frame ", 6) == 0)
    {
        s_ui->playing = false;
        s_ui->frame = atoi(line + 6);
        s_ui->shown_frame = -1;
    }
    else
        handled = false;
    lv_port_unlock();
    if (handled)
        printf("@OK %s\n", line);
    return handled;
}
#endif
