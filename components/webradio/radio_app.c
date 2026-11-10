#include "webradio/radio_app.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "core/state.h"
#include "ui/ui.h"
#include "webradio/radio.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Web radio app. The same app is an internal app (boards that register it:
 * where supported) and the home screen of the external WebRadio app:
 * see radio_app.h.
 *
 *   ┌──────────────────────────┐
 *   │ m2o                      │  station (headline)
 *   │ Artist - Song            │  ICY title, scrolling
 *   │ In riproduzione · AAC 48 │  state / format
 *   │   ⏮     ( ■ )     ⏭     │
 *   │ ♪ ───────●──────         │  volume (the knob turns it, if there is one)
 *   └──────────────────────────┘
 *   [Preferite] [Tutte] [Pop] [Rock] ...   genre filter (scrolls sideways)
 *   rows of the chosen group (long press = add/remove favourite)
 *
 * Nothing here blocks: commands go to the radio worker.
 */

#define MAX_ROWS     128
#define MAX_GENRES   24
#define VOLUME_STEP  2       /* % per knob detent */
#define METER_MAX    48      /* bars of the loudness meter (bar panels) */
#define QUICK_MAX    3       /* favourites one tap away (bar panels, portrait) */

enum
{
    FILTER_FAVORITES = -2,
    FILTER_ALL = -1,
};

typedef struct
{
    lv_obj_t *page;
    lv_obj_t *station;
    lv_obj_t *song;
    lv_obj_t *status;
    lv_obj_t *play_label;
    lv_obj_t *volume;
    lv_obj_t *volume_text;
    lv_obj_t *chips;
    lv_obj_t *list;                    /* container of the station rows */
    lv_obj_t *rows[MAX_ROWS];
    int row_station[MAX_ROWS];
    int row_count;
    int filter;                        /* FILTER_xxx or a genre index */
    int genre_count;
    uint32_t list_revision;
    char genres[MAX_GENRES][16];

    /* Bar panels only (see "touch bar layout") */
    bool bar_ui, landscape, tinted;
    lv_obj_t *hero, *badge, *play, *meter, *library, *quick;
    lv_timer_t *meter_timer;
    uint32_t tint, seed, phase;
    int meter_bars;
    uint8_t bars[METER_MAX];
    int quick_station[QUICK_MAX];
    int quick_count;
} radio_ui_t;

static radio_ui_t *s_ui = NULL;

static void bar_refresh(int station, bool on);
static void library_show(bool show);
static lv_obj_t *bar_station_tile(int station, const radio_info_t *info);

static void volume_text_update(void)
{
    lv_label_set_text_fmt(s_ui->volume_text, s_ui->bar_ui && s_ui->landscape ? "%d%%" : "Volume %d%%",
                          settings_get()->volume);
}

/* ------------------------------------------------------------- refresh */

static void refresh(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    if (s_ui == NULL)
    {
        return;
    }

    radio_info_t info;
    int station = radio_last();
    bool on = radio_is_on();

    lv_label_set_text(s_ui->station, station >= 0 && radio_get(station, &info) ? info.name : "Scegli una radio");

    char song[96] = "";
    char format[32] = "";

    lv_label_set_text(s_ui->song, on && radio_player_title(song, sizeof(song)) ? song : "");
    radio_player_format_text(format, sizeof(format));

    if (on || radio_player_state() == RADIO_STATE_FAILED)
    {
        if (format[0] != '\0' && radio_player_state() == RADIO_STATE_PLAYING)
        {
            /* Bar panels say "LIVE" in the badge: the line is the format alone. */
            if (s_ui->bar_ui) lv_label_set_text(s_ui->status, format);
            else lv_label_set_text_fmt(s_ui->status, "%s  ·  %s", radio_player_state_text(), format);
        }
        else
        {
            lv_label_set_text(s_ui->status, radio_player_state_text());
        }
    }
    else
    {
        lv_label_set_text(s_ui->status, s_ui->bar_ui ? "" : "Ferma");
    }

    lv_obj_set_style_text_color(s_ui->status, lv_color_hex(radio_player_state() == RADIO_STATE_FAILED ? UI_COLOR_RED
                                                                                                        : UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_ui->play_label, on ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);

    /* The volume may have changed elsewhere (knob, web page). */
    if (!lv_obj_has_state(s_ui->volume, LV_STATE_PRESSED))
    {
        lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_OFF);
    }
    volume_text_update();

    /* Highlight the station playing now. */
    for (int i = 0; i < s_ui->row_count; i++)
    {
        bool current = on && s_ui->row_station[i] == radio_current();
        lv_obj_set_style_bg_color(s_ui->rows[i], lv_color_hex(current ? 0x2A1830 : UI_COLOR_CARD), 0);
    }

    if (s_ui->bar_ui) bar_refresh(station, on);
}

/* ------------------------------------------------------------ controls */

static void on_play(lv_event_t *e)
{
    (void)e;
    radio_toggle();
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    radio_next(-1);
}

static void on_next(lv_event_t *e)
{
    (void)e;
    radio_next(1);
}

static void on_volume(lv_event_t *e)
{
    radio_set_volume(lv_slider_get_value(lv_event_get_target_obj(e)));
    volume_text_update();
}

/* Boards with a knob: it is the volume knob here. */
static void on_knob(lv_event_t *e)
{
    lv_port_knob_t *knob = lv_event_get_param(e);
    int volume = settings_get()->volume + knob->delta * VOLUME_STEP;

    radio_set_volume(volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_ON);
    volume_text_update();
    knob->handled = true;
}

static void on_station(lv_event_t *e)
{
    int station = (int)(intptr_t)lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
    {
        bool favorite = !radio_is_favorite(station);
        radio_set_favorite(station, favorite);
        ui_toast(favorite ? "Aggiunta ai preferiti" : "Tolta dai preferiti");
        lv_indev_wait_release(lv_indev_active());   /* no click after the long press */
        return;
    }

    radio_play(station);
    library_show(false);   /* bar panels: back to what is playing */
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, int32_t size, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *button = lv_obj_create(parent);
    ui_make_card(button);
    lv_obj_set_clickable(button, true);
    lv_port_set_focusable(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, size > 80 ? &lv_font_montserrat_32 : UI_FONT_LARGE, 0);
    lv_obj_center(label);

    return label;
}

/* ------------------------------------------------------- station lists */

static void add_station_row(int station)
{
    radio_info_t info;

    if (s_ui->row_count >= MAX_ROWS || !radio_get(station, &info))
    {
        return;
    }

    const char *detail = s_ui->filter == FILTER_ALL || s_ui->filter == FILTER_FAVORITES ? info.genre : NULL;
    lv_obj_t *row = s_ui->bar_ui ? bar_station_tile(station, &info)
                                 : ui_row(s_ui->list, radio_is_favorite(station) ? LV_SYMBOL_AUDIO : NULL, UI_COLOR_PINK,
                                          info.name, detail, NULL, NULL);
    lv_obj_set_clickable(row, true);
    lv_port_set_focusable(row);
    lv_obj_add_event_cb(row, on_station, LV_EVENT_CLICKED, (void *)(intptr_t)station);
    lv_obj_add_event_cb(row, on_station, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)station);

    s_ui->rows[s_ui->row_count] = row;
    s_ui->row_station[s_ui->row_count] = station;
    s_ui->row_count++;
}

static void fill_list(void)
{
    s_ui->list_revision = radio_list_revision();
    lv_obj_clean(s_ui->list);
    s_ui->row_count = 0;

    if (s_ui->filter == FILTER_FAVORITES)
    {
        int favorites[RADIO_FAVORITES_MAX];
        int count = radio_favorites(favorites, RADIO_FAVORITES_MAX);

        if (count == 0)
        {
            ui_text(s_ui->list, "Nessuna preferita: tieni premuta una stazione per aggiungerla.", true);
        }

        for (int i = 0; i < count; i++)
        {
            add_station_row(favorites[i]);
        }
    }
    else
    {
        for (int i = 0; i < radio_count(); i++)
        {
            radio_info_t info;

            if (s_ui->filter == FILTER_ALL ||
                (radio_get(i, &info) && strcasecmp(info.genre, s_ui->genres[s_ui->filter]) == 0))
            {
                add_station_row(i);
            }
        }
    }

    refresh(NULL, NULL);
}

static void collect_genres(void)
{
    s_ui->genre_count = 0;

    for (int i = 0; i < radio_count() && s_ui->genre_count < MAX_GENRES; i++)
    {
        radio_info_t info;

        if (!radio_get(i, &info) || info.genre[0] == '\0')
        {
            continue;
        }

        bool known = false;

        for (int g = 0; g < s_ui->genre_count && !known; g++)
        {
            known = strcasecmp(s_ui->genres[g], info.genre) == 0;
        }

        if (!known)
        {
            strncpy(s_ui->genres[s_ui->genre_count], info.genre, sizeof(s_ui->genres[0]) - 1);
            s_ui->genre_count++;
        }
    }
}

static void style_chips(void)
{
    uint32_t count = lv_obj_get_child_count(s_ui->chips);

    for (uint32_t i = 0; i < count; i++)
    {
        lv_obj_t *chip = lv_obj_get_child(s_ui->chips, (int32_t)i);
        bool selected = (int)(intptr_t)lv_obj_get_user_data(chip) == s_ui->filter;
        lv_obj_set_style_bg_color(chip, lv_color_hex(selected ? UI_COLOR_PINK : UI_COLOR_CARD_HI), 0);
    }
}

static void on_chip(lv_event_t *e)
{
    s_ui->filter = (int)(intptr_t)lv_event_get_user_data(e);
    style_chips();
    fill_list();
}

static void add_chip(const char *text, int filter)
{
    lv_obj_t *chip = lv_obj_create(s_ui->chips);
    ui_make_card(chip);
    lv_obj_set_size(chip, s_ui->bar_ui && s_ui->landscape ? LV_PCT(100) : LV_SIZE_CONTENT, s_ui->bar_ui ? 40 : 44);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, 16, 0);
    lv_obj_set_style_pad_ver(chip, 0, 0);
    lv_obj_set_clickable(chip, true);
    lv_port_set_focusable(chip);
    lv_obj_set_user_data(chip, (void *)(intptr_t)filter);
    lv_obj_add_event_cb(chip, on_chip, LV_EVENT_CLICKED, (void *)(intptr_t)filter);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_center(label);
}

/* Stations or favourites changed (here, or from the web page): rebuild the rows. */
static void on_list_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    if (s_ui != NULL && s_ui->filter < s_ui->genre_count)
    {
        /* Song metadata and PCM format use the same notification. Recreating
         * every LVGL station row on each new song can starve audio decoding. */
        if (s_ui->list_revision != radio_list_revision())
        {
            collect_genres();
            if (s_ui->filter >= s_ui->genre_count) s_ui->filter = FILTER_ALL;
            lv_obj_clean(s_ui->chips);
            add_chip(LV_SYMBOL_AUDIO " Preferite", FILTER_FAVORITES);
            add_chip("Tutte", FILTER_ALL);
            for (int g = 0; g < s_ui->genre_count; g++) add_chip(s_ui->genres[g], g);
            style_chips();
            fill_list();
        }
        else refresh(NULL, NULL);
    }
}

/* ------------------------------------------------------ touch bar layout */

/*
 * Bar panels (172 px on the short side, LCD 3.49): two full-screen views
 * instead of one squeezed page.
 *
 *   landscape 640x172
 *   ┌─────────────────────────────────────┬──────────────────────┐
 *   │ LIVE  AAC 48 kHz stereo         (☰) │   ⏮    ( ■ )    ⏭    │
 *   │ m2o                                 │                      │
 *   │ Artist - Song                       │  ♪ ──────●────  35%  │
 *   │ ▂▅▇▅▃▂▅▇▅▃▂▅▇▅▃▂▅▇▅▃▂▅▇▅▃▂▅▇▅▃     │                      │
 *   └─────────────────────────────────────┴──────────────────────┘
 *   portrait 172x640: the same hero on top, then transport, volume,
 *   "Stazioni" and three quick favourites.
 *
 *   library (☰ / Stazioni): genre chips and station tiles; a tap plays and
 *   comes back, a long press toggles the favourite, BOOT or ‹ closes it.
 *
 * Each station gets its own accent colour (from its name): the hero
 * gradient, the play button and the meter follow it. The meter moves with
 * the real loudness of the audio (radio_player_level), it is not a spectrum.
 */

#define METER_PERIOD_MS 80

static const uint32_t s_tints[] = {UI_COLOR_PINK, UI_COLOR_PURPLE, UI_COLOR_BLUE, UI_COLOR_ORANGE, UI_COLOR_GREEN};

static uint32_t station_tint(const char *name)
{
    uint32_t hash = 2166136261u;
    for (const char *c = name; *c != '\0'; c++)
    {
        hash = (hash ^ (uint8_t)*c) * 16777619u;
    }
    return s_tints[hash % (sizeof(s_tints) / sizeof(s_tints[0]))];
}

static lv_obj_t *bar_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static lv_obj_t *bar_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    if (w > 0) lv_obj_set_width(label, w);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

/* A flat touch pad with a centred symbol; returns the pad (child 0 = label). */
static lv_obj_t *bar_pad(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color,
                         const char *text, const lv_font_t *font, lv_event_cb_t cb, void *user)
{
    lv_obj_t *pad = bar_box(parent, x, y, w, h, radius, color);
    lv_obj_set_clickable(pad, true);
    lv_obj_set_style_bg_color(pad, lv_color_mix(lv_color_white(), lv_color_hex(color), 50), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(pad, 4);
    if (cb != NULL) lv_obj_add_event_cb(pad, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *label = lv_label_create(pad);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(label);
    return pad;
}

/* ---- meter: bars mirrored around the centre line, drawn in one object */

static void meter_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_ui == NULL || layer == NULL) return;

    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    int32_t w = lv_area_get_width(&box), h = lv_area_get_height(&box);
    int32_t pitch = w / s_ui->meter_bars;
    int32_t bar_w = pitch > 5 ? pitch - 3 : pitch - 1;
    int32_t mid = box.y1 + h / 2;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = bar_w / 2;
    dsc.bg_color = lv_color_hex(s_ui->tint);

    for (int i = 0; i < s_ui->meter_bars; i++)
    {
        int32_t bar_h = 4 + (h - 4) * s_ui->bars[i] / 255;
        lv_area_t area = {box.x1 + i * pitch, mid - bar_h / 2, box.x1 + i * pitch + bar_w - 1, mid - bar_h / 2 + bar_h - 1};
        dsc.bg_opa = (lv_opa_t)(90 + s_ui->bars[i] * 165 / 255);
        lv_draw_rect(layer, &dsc, &area);
    }
}

static void meter_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_ui == NULL || s_ui->meter == NULL) return;

    int level = radio_player_level();
    bool starting = radio_is_on() && radio_player_state() != RADIO_STATE_PLAYING;
    bool changed = false;
    int count = s_ui->meter_bars;
    s_ui->phase++;

    for (int i = 0; i < count; i++)
    {
        int target;
        if (starting)
        {
            /* Connecting or buffering: a soft wave travels along the bars. */
            int step = (i * 2 + (int)s_ui->phase) % 24;
            target = 30 + (step < 12 ? step : 23 - step) * 9;
        }
        else
        {
            /* Loudness, taller in the middle, with a little life per bar. */
            s_ui->seed = s_ui->seed * 1664525u + 1013904223u;
            int life = 110 + (int)((s_ui->seed >> 24) & 0x7F);            /* 110..237 */
            int centre = 255 - LV_ABS(2 * i - (count - 1)) * 150 / count;  /* 105..255 */
            target = level * life / 128 * centre / 255;
            if (target > 255) target = 255;
        }

        int value = s_ui->bars[i];
        if (target > value)
        {
            value += (target - value) * 3 / 4 + 1;
            if (value > target) value = target;
        }
        else if (target < value)
        {
            value -= (value - target) / 4 + 2;
            if (value < target) value = target;
        }
        if (value != s_ui->bars[i])
        {
            s_ui->bars[i] = (uint8_t)value;
            changed = true;
        }
    }

    if (changed) lv_obj_invalidate(s_ui->meter);   /* nothing to draw while silent and settled */
}

/* ---- library overlay */

static void library_show(bool show)
{
    if (s_ui == NULL || s_ui->library == NULL) return;
    lv_obj_set_hidden(s_ui->library, !show);
    if (show)
    {
        /* Bring the playing station into view. */
        for (int i = 0; i < s_ui->row_count; i++)
        {
            if (s_ui->row_station[i] == radio_current())
            {
                lv_obj_scroll_to_view(s_ui->rows[i], LV_ANIM_OFF);
                break;
            }
        }
    }
}

static void on_library_open(lv_event_t *e)
{
    (void)e;
    library_show(true);
}

static void on_library_close(lv_event_t *e)
{
    (void)e;
    library_show(false);
}

bool radio_app_back(void)
{
    if (s_ui == NULL || s_ui->library == NULL || lv_obj_has_flag(s_ui->library, LV_OBJ_FLAG_HIDDEN))
    {
        return false;
    }
    library_show(false);
    return true;
}

/* A station tile of the library (bar panels). */
static lv_obj_t *bar_station_tile(int station, const radio_info_t *info)
{
    bool favorite = radio_is_favorite(station);
    lv_obj_t *tile = lv_obj_create(s_ui->list);
    ui_make_card(tile);
    lv_obj_set_size(tile, s_ui->landscape ? 150 : LV_PCT(100), s_ui->landscape ? 76 : 54);
    lv_obj_set_style_pad_hor(tile, 12, 0);
    lv_obj_set_style_pad_ver(tile, 6, 0);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_border_side(tile, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(tile, favorite ? 4 : 0, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(station_tint(info->name)), 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, info->name);
    lv_obj_set_style_text_font(name, UI_FONT_SMALL, 0);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(name, lv_font_get_line_height(UI_FONT_SMALL) * (s_ui->landscape ? 2 : 1));
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *genre = lv_label_create(tile);
    lv_label_set_text_fmt(genre, "%s%s", favorite ? LV_SYMBOL_AUDIO " " : "", info->genre);
    lv_obj_set_style_text_font(genre, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(genre, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(genre, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return tile;
}

/* ---- quick favourites (portrait) */

static void on_quick(lv_event_t *e)
{
    radio_play((int)(intptr_t)lv_event_get_user_data(e));
}

static void quick_fill(void)
{
    if (s_ui->quick == NULL) return;

    int favorites[RADIO_FAVORITES_MAX];
    int count = radio_favorites(favorites, RADIO_FAVORITES_MAX);
    if (count > QUICK_MAX) count = QUICK_MAX;

    bool same = count == s_ui->quick_count;
    for (int i = 0; i < count && same; i++) same = favorites[i] == s_ui->quick_station[i];
    if (same && lv_obj_get_child_count(s_ui->quick) > 0) return;

    lv_obj_clean(s_ui->quick);
    s_ui->quick_count = count;
    int32_t w = lv_obj_get_style_width(s_ui->quick, 0);

    if (count == 0)
    {
        lv_obj_t *hint = bar_label(s_ui->quick, 4, 4, w - 8, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(hint, "Tieni premuta una stazione in Stazioni per averla qui.");
        return;
    }

    for (int i = 0; i < count; i++)
    {
        radio_info_t info;
        s_ui->quick_station[i] = favorites[i];
        if (!radio_get(favorites[i], &info)) continue;
        lv_obj_t *pad = bar_pad(s_ui->quick, 0, i * 34, w, 30, 15, UI_COLOR_CARD, info.name, UI_FONT_SMALL, on_quick,
                                (void *)(intptr_t)favorites[i]);
        lv_obj_t *label = lv_obj_get_child(pad, 0);
        lv_obj_set_width(label, w - 24);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(label, lv_font_get_line_height(UI_FONT_SMALL));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        lv_obj_set_style_border_width(pad, 1, 0);
        lv_obj_set_style_border_color(pad, lv_color_hex(station_tint(info.name)), 0);
        lv_obj_set_style_border_opa(pad, LV_OPA_60, 0);
    }
}

/* ---- state -> hero colours, badge, quick list (called from refresh) */

static void bar_refresh(int station, bool on)
{
    radio_info_t info;
    radio_state_t state = radio_player_state();
    uint32_t tint = station >= 0 && radio_get(station, &info) ? station_tint(info.name) : UI_COLOR_PINK;

    if (tint != s_ui->tint || !s_ui->tinted)
    {
        s_ui->tint = tint;
        s_ui->tinted = true;
        lv_color_t deep = lv_color_hex(0x07080D);
        lv_obj_set_style_bg_color(s_ui->hero, lv_color_mix(lv_color_hex(tint), deep, 70), 0);
        lv_obj_set_style_bg_grad_color(s_ui->hero, lv_color_mix(lv_color_hex(tint), deep, 12), 0);
        lv_obj_set_style_border_color(s_ui->hero, lv_color_mix(lv_color_hex(tint), deep, 120), 0);
        lv_obj_set_style_bg_color(s_ui->play, lv_color_hex(tint), 0);
        lv_obj_set_style_bg_color(s_ui->play, lv_color_mix(lv_color_white(), lv_color_hex(tint), 60), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_ui->volume, lv_color_hex(tint), LV_PART_INDICATOR);
        lv_obj_set_style_outline_color(s_ui->play, lv_color_hex(tint), 0);
        lv_obj_invalidate(s_ui->meter);
    }

    const char *badge = "FERMA";
    uint32_t badge_color = UI_COLOR_CARD_HI;
    bool neutral = true;
    if (state == RADIO_STATE_FAILED)
    {
        badge = "ERRORE";
        badge_color = UI_COLOR_RED;
        neutral = false;
    }
    else if (on && state == RADIO_STATE_PLAYING)
    {
        badge = "LIVE";
        badge_color = tint;
        neutral = false;
    }
    else if (on)
    {
        badge = "ATTESA";
        badge_color = UI_COLOR_ORANGE;
        neutral = false;
    }
    lv_obj_t *badge_text = lv_obj_get_child(s_ui->badge, 0);
    lv_label_set_text(badge_text, badge);
    lv_obj_set_style_bg_color(s_ui->badge, lv_color_hex(badge_color), 0);
    lv_obj_set_style_text_color(badge_text, neutral ? lv_color_hex(UI_COLOR_TEXT_DIM) : lv_color_black(), 0);

    quick_fill();

    /* The quick favourite playing now is filled with its colour. */
    for (int i = 0; s_ui->quick != NULL && i < s_ui->quick_count; i++)
    {
        lv_obj_t *pad = lv_obj_get_child(s_ui->quick, i);
        if (pad == NULL) break;
        bool current = on && s_ui->quick_station[i] == radio_current();
        lv_obj_set_style_bg_color(pad, current ? lv_color_mix(lv_color_hex(tint), lv_color_hex(0x07080D), 110)
                                               : lv_color_hex(UI_COLOR_CARD), 0);
    }
}

static void bar_create(lv_obj_t *screen, bool landscape)
{
    lv_display_t *display = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(display), h = lv_display_get_vertical_resolution(display);
    const int32_t gap = 6;

    s_ui->bar_ui = true;
    s_ui->landscape = landscape;
    s_ui->seed = 0x2545F491u;
    s_ui->meter_bars = landscape ? 46 : 17;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* ---- hero: what is playing */
    int32_t hw = landscape ? w * 61 / 100 : w - 2 * gap, hh = landscape ? h - 2 * gap : 232;
    lv_obj_t *hero = bar_box(screen, gap, gap, hw, hh, 22, UI_COLOR_CARD);
    s_ui->hero = hero;
    lv_obj_set_style_bg_grad_dir(hero, landscape ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_border_opa(hero, LV_OPA_70, 0);
    const int32_t pad = 14;
    int32_t inner = hw - 2 * pad;

    s_ui->badge = bar_box(hero, pad, 12, 62, 22, 11, UI_COLOR_CARD_HI);
    lv_obj_t *badge_text = lv_label_create(s_ui->badge);
    lv_obj_set_style_text_font(badge_text, UI_FONT_CAPTION, 0);
    lv_obj_set_width(badge_text, 62);
    lv_obj_set_style_text_align(badge_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(badge_text);

    if (landscape)
    {
        s_ui->status = bar_label(hero, pad + 72, 16, inner - 72 - 52, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);
        lv_label_set_long_mode(s_ui->status, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_ui->status, lv_font_get_line_height(UI_FONT_CAPTION));
        lv_obj_t *open = bar_pad(hero, hw - pad - 42, 8, 42, 42, 21, 0x000000, LV_SYMBOL_LIST, UI_FONT_ICON,
                                 on_library_open, NULL);
        lv_obj_set_style_bg_opa(open, LV_OPA_40, 0);
        lv_obj_set_ext_click_area(open, 10);

        s_ui->station = bar_label(hero, pad, 42, inner - 52, ui_font_headline, UI_COLOR_TEXT);
        lv_label_set_long_mode(s_ui->station, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_ui->station, lv_font_get_line_height(ui_font_headline));
        s_ui->song = bar_label(hero, pad, 88, inner, UI_FONT_SMALL, UI_COLOR_TEXT);
        lv_label_set_long_mode(s_ui->song, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        s_ui->meter = bar_box(hero, pad, 114, inner, hh - 114 - 10, 0, 0);
    }
    else
    {
        s_ui->station = bar_label(hero, pad, 44, inner, ui_font_title, UI_COLOR_TEXT);
        lv_label_set_long_mode(s_ui->station, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_ui->station, lv_font_get_line_height(ui_font_title) * 2);
        int32_t y = 44 + lv_font_get_line_height(ui_font_title) * 2 + 4;
        s_ui->song = bar_label(hero, pad, y, inner, UI_FONT_SMALL, UI_COLOR_TEXT);
        lv_label_set_long_mode(s_ui->song, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        s_ui->status = bar_label(hero, pad, y + 22, inner, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);
        lv_label_set_long_mode(s_ui->status, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_ui->status, lv_font_get_line_height(UI_FONT_CAPTION));
        s_ui->meter = bar_box(hero, pad, y + 44, inner, hh - (y + 44) - 12, 0, 0);
    }
    lv_obj_set_style_bg_opa(s_ui->meter, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_ui->meter, meter_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* ---- transport and volume */
    if (landscape)
    {
        int32_t px = hw + 2 * gap, pw = w - px - gap, ph = h - 2 * gap;
        lv_obj_t *panel = bar_box(screen, px, gap, pw, ph, 22, UI_COLOR_CARD);
        int32_t play_size = 78, side = 56;
        int32_t space = (pw - play_size - 2 * side) / 4;
        bar_pad(panel, space, 10 + (play_size - side) / 2, side, side, side / 2, UI_COLOR_CARD_HI, LV_SYMBOL_PREV,
                UI_FONT_ICON, on_prev, NULL);
        s_ui->play = bar_pad(panel, 2 * space + side, 10, play_size, play_size, play_size / 2, UI_COLOR_PINK,
                             LV_SYMBOL_PLAY, &lv_font_montserrat_32, on_play, NULL);
        bar_pad(panel, 3 * space + side + play_size, 10 + (play_size - side) / 2, side, side, side / 2,
                UI_COLOR_CARD_HI, LV_SYMBOL_NEXT, UI_FONT_ICON, on_next, NULL);

        lv_obj_t *speaker = bar_label(panel, 14, ph - 46, 0, UI_FONT_ICON, UI_COLOR_TEXT_DIM);
        lv_label_set_text(speaker, LV_SYMBOL_VOLUME_MAX);
        s_ui->volume = lv_slider_create(panel);
        lv_obj_set_pos(s_ui->volume, 54, ph - 38);
        lv_obj_set_size(s_ui->volume, pw - 54 - 66, 12);
        s_ui->volume_text = bar_label(panel, pw - 58, ph - 42, 48, UI_FONT_SMALL, UI_COLOR_TEXT);
        lv_obj_set_style_text_align(s_ui->volume_text, LV_TEXT_ALIGN_RIGHT, 0);
    }
    else
    {
        int32_t y = gap + hh + 10;
        int32_t play_size = 84;
        s_ui->play = bar_pad(screen, (w - play_size) / 2, y, play_size, play_size, play_size / 2, UI_COLOR_PINK,
                             LV_SYMBOL_PLAY, &lv_font_montserrat_32, on_play, NULL);
        y += play_size + 10;
        int32_t half = (w - 3 * gap) / 2;
        bar_pad(screen, gap, y, half, 52, 26, UI_COLOR_CARD_HI, LV_SYMBOL_PREV, UI_FONT_ICON, on_prev, NULL);
        bar_pad(screen, 2 * gap + half, y, half, 52, 26, UI_COLOR_CARD_HI, LV_SYMBOL_NEXT, UI_FONT_ICON, on_next, NULL);
        y += 52 + 12;
        s_ui->volume_text = bar_label(screen, gap, y, w - 2 * gap, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
        lv_obj_set_style_text_align(s_ui->volume_text, LV_TEXT_ALIGN_CENTER, 0);
        s_ui->volume = lv_slider_create(screen);
        lv_obj_set_pos(s_ui->volume, 22, y + 28);
        lv_obj_set_size(s_ui->volume, w - 44, 12);
        y += 28 + 12 + 16;
        lv_obj_t *open = bar_pad(screen, gap, y, w - 2 * gap, 50, 25, UI_COLOR_CARD, LV_SYMBOL_LIST "  Stazioni",
                                 UI_FONT_SMALL, on_library_open, NULL);
        lv_obj_set_style_border_width(open, 1, 0);
        lv_obj_set_style_border_color(open, lv_color_hex(UI_COLOR_SEPARATOR), 0);
        y += 50 + 12;
        s_ui->quick = bar_box(screen, gap, y, w - 2 * gap, h - y - gap, 0, 0);
        lv_obj_set_style_bg_opa(s_ui->quick, LV_OPA_TRANSP, 0);
    }
    s_ui->play_label = lv_obj_get_child(s_ui->play, 0);
    lv_obj_set_style_text_color(s_ui->play_label, lv_color_black(), 0);
    /* A soft ring in the station colour around the main control. */
    lv_obj_set_style_outline_width(s_ui->play, 5, 0);
    lv_obj_set_style_outline_pad(s_ui->play, 3, 0);
    lv_obj_set_style_outline_opa(s_ui->play, LV_OPA_30, 0);

    lv_slider_set_range(s_ui->volume, 0, 100);
    lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui->volume, lv_color_hex(UI_COLOR_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui->volume, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui->volume, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui->volume, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_ui->volume, 18);
    lv_obj_add_event_cb(s_ui->volume, on_volume, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- library: full-screen overlay, hidden until asked */
    lv_obj_t *library = bar_box(screen, 0, 0, w, h, 0, 0x000000);
    s_ui->library = library;
    lv_obj_set_clickable(library, true);   /* nothing below reacts while it is open */
    if (landscape)
    {
        bar_pad(library, gap, gap, 50, h - 2 * gap, 20, UI_COLOR_CARD, LV_SYMBOL_LEFT, UI_FONT_ICON, on_library_close,
                NULL);
        s_ui->chips = bar_box(library, 50 + 2 * gap, gap, 132, h - 2 * gap, 0, 0);
        lv_obj_set_flex_flow(s_ui->chips, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_ui->chips, 6, 0);
        lv_obj_set_scroll_dir(s_ui->chips, LV_DIR_VER);
        s_ui->list = bar_box(library, 50 + 132 + 3 * gap, gap, w - (50 + 132 + 4 * gap), h - 2 * gap, 0, 0);
        lv_obj_set_flex_flow(s_ui->list, LV_FLEX_FLOW_COLUMN_WRAP);
        lv_obj_set_style_pad_row(s_ui->list, 8, 0);
        lv_obj_set_style_pad_column(s_ui->list, 8, 0);
        lv_obj_set_scroll_dir(s_ui->list, LV_DIR_HOR);
    }
    else
    {
        bar_pad(library, gap, gap, 50, 44, 20, UI_COLOR_CARD, LV_SYMBOL_LEFT, UI_FONT_ICON, on_library_close, NULL);
        lv_obj_t *title = bar_label(library, 50 + 2 * gap + 4, gap + 11, 0, UI_FONT_SMALL, UI_COLOR_TEXT);
        lv_label_set_text(title, "Stazioni");
        s_ui->chips = bar_box(library, gap, 44 + 2 * gap, w - 2 * gap, 44, 0, 0);
        lv_obj_set_flex_flow(s_ui->chips, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(s_ui->chips, 6, 0);
        lv_obj_set_scroll_dir(s_ui->chips, LV_DIR_HOR);
        s_ui->list = bar_box(library, gap, 88 + 3 * gap, w - 2 * gap, h - (88 + 4 * gap), 0, 0);
        lv_obj_set_flex_flow(s_ui->list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_ui->list, 6, 0);
        lv_obj_set_scroll_dir(s_ui->list, LV_DIR_VER);
    }
    lv_obj_set_style_bg_opa(s_ui->chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_ui->list, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(s_ui->chips, true);
    lv_obj_set_scrollable(s_ui->list, true);
    lv_obj_set_clickable(s_ui->chips, true);   /* drag the gaps between tiles too */
    lv_obj_set_clickable(s_ui->list, true);
    lv_obj_set_scrollbar_mode(s_ui->chips, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollbar_mode(s_ui->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_hidden(library, true);

    s_ui->meter_timer = lv_timer_create(meter_tick, METER_PERIOD_MS, NULL);
}

/* ----------------------------------------------------------------- app */

/* The page of watches and square panels: now playing card, chips, rows. */
static void classic_create(lv_obj_t *page)
{
    /* now playing card */
    lv_obj_t *card = lv_obj_create(page);
    ui_make_card(card);
    lv_obj_remove_style(card, NULL, LV_STATE_PRESSED);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x3A1238), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);

    s_ui->station = lv_label_create(card);
    lv_obj_set_style_text_font(s_ui->station, ui_font_headline, 0);
    lv_label_set_long_mode(s_ui->station, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_ui->station, LV_PCT(100));
    lv_obj_set_style_text_align(s_ui->station, LV_TEXT_ALIGN_CENTER, 0);

    s_ui->song = lv_label_create(card);
    lv_label_set_long_mode(s_ui->song, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_ui->song, LV_PCT(100));
    lv_obj_set_style_text_align(s_ui->song, LV_TEXT_ALIGN_CENTER, 0);

    s_ui->status = lv_label_create(card);
    lv_obj_set_style_text_font(s_ui->status, UI_FONT_SMALL, 0);
    lv_obj_set_width(s_ui->status, LV_PCT(100));
    lv_obj_set_style_text_align(s_ui->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui->status, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(controls, 8, 0);

    round_button(controls, LV_SYMBOL_PREV, 64, UI_COLOR_CARD_HI, on_prev);
    s_ui->play_label = round_button(controls, LV_SYMBOL_PLAY, 92, UI_COLOR_PINK, on_play);
    round_button(controls, LV_SYMBOL_NEXT, 64, UI_COLOR_CARD_HI, on_next);

    lv_obj_t *volume_area = lv_obj_create(card);
    lv_obj_remove_style_all(volume_area);
    lv_obj_set_size(volume_area, LV_PCT(100), 54);
    s_ui->volume_text = lv_label_create(volume_area);
    lv_obj_set_style_text_font(s_ui->volume_text, UI_FONT_SMALL, 0);
    lv_obj_align(s_ui->volume_text, LV_ALIGN_TOP_MID, 0, 0);
    s_ui->volume = lv_slider_create(volume_area);
    lv_obj_align(s_ui->volume, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_width(s_ui->volume, LV_PCT(90));
    lv_obj_set_height(s_ui->volume, 10);
    lv_slider_set_range(s_ui->volume, 0, 100);
    lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui->volume, lv_color_hex(UI_COLOR_PINK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui->volume, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_ui->volume, 16);
    lv_obj_add_event_cb(s_ui->volume, on_volume, LV_EVENT_VALUE_CHANGED, NULL);

    /* genre filter: a row of chips that scrolls sideways */
    s_ui->chips = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui->chips);
    lv_obj_set_size(s_ui->chips, LV_PCT(100), 52);
    lv_obj_set_flex_flow(s_ui->chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui->chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui->chips, 8, 0);
    lv_obj_set_scroll_dir(s_ui->chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_ui->chips, LV_SCROLLBAR_MODE_OFF);

    /* the rows of the chosen group */
    s_ui->list = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui->list);
    lv_obj_set_size(s_ui->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui->list, lv_obj_get_style_pad_row(page, LV_PART_MAIN), 0);
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_ui = calloc(1, sizeof(radio_ui_t));

    if (s_ui == NULL)
    {
        return;
    }

    const board_info_t *view = ui_board_info();
    bool bar_panel = !view->round && ((view->width <= 200 && view->height >= 2 * view->width) ||
                                      (view->height <= 200 && view->width >= 2 * view->height));
    lv_obj_t *page;
    if (bar_panel)
    {
        bar_create(screen, view->width > view->height);
        page = screen;
    }
    else
    {
        page = ui_page(screen, NULL);
        classic_create(page);
    }
    s_ui->page = page;

    collect_genres();
    add_chip(LV_SYMBOL_AUDIO " Preferite", FILTER_FAVORITES);
    add_chip("Tutte", FILTER_ALL);

    for (int g = 0; g < s_ui->genre_count; g++)
    {
        add_chip(s_ui->genres[g], g);
    }

    int favorites[RADIO_FAVORITES_MAX];
    s_ui->filter = radio_favorites(favorites, RADIO_FAVORITES_MAX) > 0 ? FILTER_FAVORITES : FILTER_ALL;
    style_chips();
    fill_list();

    lv_obj_add_event_cb(screen, on_knob, lv_port_knob_event(), NULL);
    lv_subject_add_observer_obj(state_subject(STATE_RADIO), refresh, page, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_RADIO_VERSION), on_list_changed, page, NULL);
}

static void destroy(void)
{
    if (s_ui != NULL && s_ui->meter_timer != NULL)
    {
        lv_timer_delete(s_ui->meter_timer);
    }
    free(s_ui);
    s_ui = NULL;
}

const app_t radio_app = {
    .id = "radio",
    .name = "Radio",
    .icon = LV_SYMBOL_AUDIO,
    .color = UI_COLOR_PINK,
    .flags = APP_FLAG_STAY_ON_WAKE,
    .create = create,
    .destroy = destroy,
    .back = radio_app_back,
};
