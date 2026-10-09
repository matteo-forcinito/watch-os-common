#include "apps_internal.h"

#include "companion/ble_companion.h"
#include "core/lv_port.h"
#include "core/state.h"
#include "ui/ui.h"

#include <stdlib.h>

/*
 * Music remote (swipe right from the watch face, and the Music app): shows
 * what the phone is playing and sends play / pause / next / previous over
 * Bluetooth (Gadgetbridge).
 *
 * Used in two places at once, so each instance has its own np_t, freed with
 * its widgets.
 */

typedef struct
{
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *play_label;
    bool playing;
} np_t;

static void refresh(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;

    np_t *np = lv_observer_get_user_data(observer);
    ble_music_t music;
    bool connected = ble_companion_connected();
    bool known = connected && ble_companion_music(&music);

    np->playing = known && music.playing;

    if (!connected)
    {
        lv_label_set_text(np->title, "Musica");
        lv_label_set_text(np->subtitle, "Collega il telefono (Gadgetbridge)");
    }
    else if (!known)
    {
        lv_label_set_text(np->title, "Musica");
        lv_label_set_text(np->subtitle, "Avvia la musica sul telefono");
    }
    else
    {
        lv_label_set_text(np->title, music.track[0] != '\0' ? music.track : "Musica");
        lv_label_set_text(np->subtitle, music.artist);
    }

    lv_label_set_text(np->play_label, np->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    ble_companion_music_command("previous");
}

static void on_next(lv_event_t *e)
{
    (void)e;
    ble_companion_music_command("next");
}

static void on_play(lv_event_t *e)
{
    np_t *np = lv_event_get_user_data(e);
    ble_companion_music_command(np->playing ? "pause" : "play");
}

static void on_volume(lv_event_t *e)
{
    ble_companion_music_command(lv_event_get_user_data(e));
}

static lv_obj_t *control(lv_obj_t *parent, const char *icon, int32_t size, uint32_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *button = lv_obj_create(parent);
    ui_make_card(button);
    lv_obj_set_clickable(button, true);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_port_set_focusable(button);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, size > 90 ? &lv_font_montserrat_32 : UI_FONT_ICON, 0);
    lv_obj_center(label);

    return label;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static void free_np(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

void now_playing_create(lv_obj_t *parent)
{
    np_t *np = calloc(1, sizeof(np_t));

    if (np == NULL)
    {
        return;
    }

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_row(root, 12, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_add_event_cb(root, free_np, LV_EVENT_DELETE, np);

    lv_obj_t *source = lv_label_create(root);
    lv_label_set_text(source, LV_SYMBOL_BLUETOOTH "  TELEFONO");
    lv_obj_set_style_text_font(source, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(source, ui_accent(), 0);

    np->title = lv_label_create(root);
    lv_obj_set_style_text_font(np->title, ui_font_headline, 0);
    lv_label_set_long_mode(np->title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(np->title, LV_PCT(100));
    lv_obj_set_style_text_align(np->title, LV_TEXT_ALIGN_CENTER, 0);

    np->subtitle = lv_label_create(root);
    lv_label_set_long_mode(np->subtitle, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(np->subtitle, LV_PCT(100));
    lv_obj_set_style_text_align(np->subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(np->subtitle, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_pad_bottom(np->subtitle, 16, 0);

    lv_obj_t *transport = row(root);
    control(transport, LV_SYMBOL_PREV, 72, UI_COLOR_CARD, on_prev, np);
    np->play_label = control(transport, LV_SYMBOL_PLAY, 104, UI_COLOR_PINK, on_play, np);
    control(transport, LV_SYMBOL_NEXT, 72, UI_COLOR_CARD, on_next, np);

    lv_obj_t *volume = row(root);
    control(volume, LV_SYMBOL_VOLUME_MID, 60, UI_COLOR_CARD, on_volume, (void *)"volumedown");
    control(volume, LV_SYMBOL_VOLUME_MAX, 60, UI_COLOR_CARD, on_volume, (void *)"volumeup");

    lv_subject_add_observer_obj(state_subject(STATE_MUSIC_VERSION), refresh, root, np);
    lv_subject_add_observer_obj(state_subject(STATE_BLE), refresh, root, np);
}
