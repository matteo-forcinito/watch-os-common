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
    lv_obj_t *chips;
    lv_obj_t *list;                    /* container of the station rows */
    lv_obj_t *rows[MAX_ROWS];
    int row_station[MAX_ROWS];
    int row_count;
    int filter;                        /* FILTER_xxx or a genre index */
    int genre_count;
    char genres[MAX_GENRES][16];
} radio_ui_t;

static radio_ui_t *s_ui = NULL;

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
            lv_label_set_text_fmt(s_ui->status, "%s  ·  %s", radio_player_state_text(), format);
        }
        else
        {
            lv_label_set_text(s_ui->status, radio_player_state_text());
        }
    }
    else
    {
        lv_label_set_text(s_ui->status, "Ferma");
    }

    lv_obj_set_style_text_color(s_ui->status, lv_color_hex(radio_player_state() == RADIO_STATE_FAILED ? UI_COLOR_RED
                                                                                                        : UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_ui->play_label, on ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);

    /* The volume may have changed elsewhere (knob, web page). */
    if (!lv_obj_has_state(s_ui->volume, LV_STATE_PRESSED))
    {
        lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_OFF);
    }

    /* Highlight the station playing now. */
    for (int i = 0; i < s_ui->row_count; i++)
    {
        bool current = on && s_ui->row_station[i] == radio_current();
        lv_obj_set_style_bg_color(s_ui->rows[i], lv_color_hex(current ? 0x2A1830 : UI_COLOR_CARD), 0);
    }
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
}

/* Boards with a knob: it is the volume knob here. */
static void on_knob(lv_event_t *e)
{
    lv_port_knob_t *knob = lv_event_get_param(e);
    int volume = settings_get()->volume + knob->delta * VOLUME_STEP;

    radio_set_volume(volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    lv_slider_set_value(s_ui->volume, settings_get()->volume, LV_ANIM_ON);
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

    lv_obj_t *row = ui_row(s_ui->list, radio_is_favorite(station) ? LV_SYMBOL_AUDIO : NULL, UI_COLOR_PINK, info.name,
                           s_ui->filter == FILTER_ALL || s_ui->filter == FILTER_FAVORITES ? info.genre : NULL, NULL,
                           NULL);
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
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
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
        fill_list();
    }
}

/* ----------------------------------------------------------------- app */

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_ui = calloc(1, sizeof(radio_ui_t));

    if (s_ui == NULL)
    {
        return;
    }

    lv_obj_t *page = ui_page(screen, "Radio");
    s_ui->page = page;

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

    lv_obj_t *controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(controls, 8, 0);

    round_button(controls, LV_SYMBOL_PREV, 64, UI_COLOR_CARD_HI, on_prev);
    s_ui->play_label = round_button(controls, LV_SYMBOL_PLAY, 92, UI_COLOR_PINK, on_play);
    round_button(controls, LV_SYMBOL_NEXT, 64, UI_COLOR_CARD_HI, on_next);

    s_ui->volume = lv_slider_create(card);
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

    collect_genres();
    add_chip(LV_SYMBOL_AUDIO " Preferite", FILTER_FAVORITES);
    add_chip("Tutte", FILTER_ALL);

    for (int g = 0; g < s_ui->genre_count; g++)
    {
        add_chip(s_ui->genres[g], g);
    }

    /* the rows of the chosen group */
    s_ui->list = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui->list);
    lv_obj_set_size(s_ui->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui->list, lv_obj_get_style_pad_row(page, LV_PART_MAIN), 0);

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
};
