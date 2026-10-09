#include "apps_internal.h"

#include "core/clock.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "services/alarm.h"
#include "companion/ble_companion.h"
#include "hardware/board.h"
#include "services/weather.h"
#include "ui/ui.h"

#include <stdio.h>

/*
 * Digital watch face.
 *
 *        ᛒ  ⌔             82% ▮      status
 *             MER 8 OTT               date (accent)
 *              14:05                  huge light digits
 *     ( 82% )  ( ☁ 18° )  ( ⏰ 7:30 )  complications (tap to open)
 *          ♪ song                     shown while the phone plays music
 *
 * Round panels get their own layout (build_round): the battery is a ring
 * along the edge of the glass, everything else is centred inside the circle.
 *
 *            ᛒ ⌔ 82%                  status (tap: control center)
 *            MER 8 OTT
 *              14:05
 *        ( ☁ 18° )  ( ⏰ 7:30 )
 *             ♪ song
 *    ((  battery ring around it all  ))
 *
 * Everything redraws from state observers: once a minute for the time, and
 * only when something actually changes for the rest.
 */

typedef struct
{
    lv_obj_t *time;
    lv_obj_t *date;
    lv_obj_t *battery_arc;
    lv_obj_t *battery_label;
    lv_obj_t *status_battery;
    lv_obj_t *status_ble;
    lv_obj_t *status_wifi;
    lv_obj_t *weather_box;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_label;
    lv_obj_t *alarm_label;
    lv_obj_t *music_pill;
    lv_obj_t *music_label;
} face_t;

static face_t s_face;

static void watchface_observe(lv_obj_t *root);

/* ------------------------------------------------------------ updates */

static void on_status(lv_event_t *e)
{
    (void)e;
    shell_show_control_center();
}

static void update_time(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    struct tm now;
    char text[16];

    clock_local(&now);

    if (clock_is_valid())
    {
        clock_format_hm(&now, text, sizeof(text));
        lv_label_set_text(s_face.time, text);
        lv_label_set_text_fmt(s_face.date, "%s %d %s", clock_weekday_name(now.tm_wday, true), now.tm_mday,
                              clock_month_name(now.tm_mon, true));
    }
    else
    {
        lv_label_set_text(s_face.time, "--:--");
        lv_label_set_text(s_face.date, "ORA NON IMPOSTATA");
    }
}

static void update_battery(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    int percent = state_get(STATE_BATTERY);
    bool charging = state_get(STATE_CHARGING) != 0;
    uint32_t color = charging ? UI_COLOR_GREEN : (percent >= 0 && percent < 20 ? UI_COLOR_RED : UI_COLOR_GREEN);

    lv_arc_set_value(s_face.battery_arc, percent < 0 ? 100 : percent);
    lv_obj_set_style_arc_color(s_face.battery_arc, lv_color_hex(color), LV_PART_INDICATOR);

    if (percent < 0)
    {
        if (s_face.battery_label != NULL)
        {
            lv_label_set_text(s_face.battery_label, LV_SYMBOL_USB);
        }

        lv_label_set_text(s_face.status_battery, LV_SYMBOL_USB);
    }
    else
    {
        if (s_face.battery_label != NULL)
        {
            lv_label_set_text_fmt(s_face.battery_label, "%s%d%%", charging ? LV_SYMBOL_CHARGE : "", percent);
            lv_label_set_text_fmt(s_face.status_battery, "%d%% %s", percent, ui_battery_symbol(percent));
        }
        else
        {
            lv_label_set_text_fmt(s_face.status_battery, "%s%d%%", charging ? LV_SYMBOL_CHARGE " " : "", percent);
        }
    }
}

static void update_links(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    int ble = state_get(STATE_BLE);
    int wifi = state_get(STATE_WIFI);

    lv_obj_set_hidden(s_face.status_ble, ble == STATE_BLE_OFF);
    lv_obj_set_style_text_color(s_face.status_ble,
                                lv_color_hex(ble == STATE_BLE_CONNECTED ? UI_COLOR_BLUE : UI_COLOR_GRAY), 0);
    lv_obj_set_hidden(s_face.status_wifi, wifi == STATE_WIFI_OFF);
    lv_obj_set_style_text_color(s_face.status_wifi,
                                lv_color_hex(wifi == STATE_WIFI_CONNECTED ? UI_COLOR_TEXT : UI_COLOR_GRAY), 0);
}

static void update_weather(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    weather_t w;

    if (s_face.weather_icon != NULL)
    {
        lv_obj_delete(s_face.weather_icon);
        s_face.weather_icon = NULL;
    }

    if (weather_get(&w))
    {
        s_face.weather_icon = ui_weather_icon(s_face.weather_box, w.code, !w.is_day, 40);
        lv_obj_move_to_index(s_face.weather_icon, 0);
        lv_label_set_text_fmt(s_face.weather_label, "%d°", (int)(w.temp + (w.temp >= 0 ? 0.5f : -0.5f)));
    }
    else
    {
        s_face.weather_icon = ui_weather_icon(s_face.weather_box, 3, false, 40);
        lv_obj_move_to_index(s_face.weather_icon, 0);
        lv_label_set_text(s_face.weather_label, "--");
    }
}

static void update_alarm(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    alarm_t next;
    time_t when;

    if (alarm_next(&next, &when))
    {
        struct tm t;
        char text[12];
        localtime_r(&when, &t);
        clock_format_hm(&t, text, sizeof(text));
        lv_label_set_text(s_face.alarm_label, text);
    }
    else
    {
        lv_label_set_text(s_face.alarm_label, "Off");
    }
}

static void update_music(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    ble_music_t music;
    bool on = ble_companion_connected() && ble_companion_music(&music) && music.playing;

    lv_obj_set_hidden(s_face.music_pill, !on);

    if (on)
    {
        lv_label_set_text_fmt(s_face.music_label, LV_SYMBOL_AUDIO "  %s", music.track);
    }
}

/* Coming back to the face: refresh old weather (cheap no-op if recent). */
static void screen_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;

    if (lv_subject_get_int(subject) == POWER_SCREEN_ON)
    {
        weather_refresh(2 * 3600);
    }
}

/* ------------------------------------------------------------- layout */

static lv_obj_t *complication(lv_obj_t *parent, const char *open_app)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_make_card(c);
    lv_obj_set_size(c, 104, 104);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 2, 0);

    if (open_app != NULL)
    {
        lv_obj_set_clickable(c, true);
        lv_port_set_focusable(c);
        lv_obj_add_event_cb(c, apps_open_cb, LV_EVENT_CLICKED, (void *)open_app);
    }

    return c;
}

static lv_obj_t *status_row(lv_obj_t *root)
{
    lv_obj_t *status = lv_obj_create(root);
    lv_obj_remove_style_all(status);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 12, 0);
    lv_obj_set_style_text_font(status, UI_FONT_SMALL, 0);
    lv_obj_set_style_radius(status, 20, 0);

    /* Tap (or knob focus + click): the control center, with the settings. */
    lv_obj_set_clickable(status, true);
    lv_obj_set_scrollable(status, false);
    lv_port_set_focusable(status);
    lv_obj_add_event_cb(status, on_status, LV_EVENT_CLICKED, NULL);

    s_face.status_ble = lv_label_create(status);
    lv_label_set_text(s_face.status_ble, LV_SYMBOL_BLUETOOTH);
    s_face.status_wifi = lv_label_create(status);
    lv_label_set_text(s_face.status_wifi, LV_SYMBOL_WIFI);

    return status;
}

static void music_pill(lv_obj_t *root, int32_t max_width)
{
    s_face.music_pill = lv_obj_create(root);
    ui_make_card(s_face.music_pill);
    lv_obj_set_clickable(s_face.music_pill, true);
    lv_obj_set_style_radius(s_face.music_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_face.music_pill, 20, 0);
    lv_obj_set_size(s_face.music_pill, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_max_width(s_face.music_pill, max_width, 0);
    lv_port_set_focusable(s_face.music_pill);
    lv_obj_add_event_cb(s_face.music_pill, apps_open_cb, LV_EVENT_CLICKED, (void *)"music");
    s_face.music_label = lv_label_create(s_face.music_pill);
    lv_label_set_long_mode(s_face.music_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(s_face.music_label, max_width - 40, 0);
    lv_obj_set_style_text_color(s_face.music_label, ui_accent(), 0);
    lv_obj_center(s_face.music_label);
}

static void weather_and_alarm(lv_obj_t *row)
{
    s_face.weather_box = complication(row, "weather");
    s_face.weather_label = lv_label_create(s_face.weather_box);
    lv_obj_set_style_text_font(s_face.weather_label, UI_FONT_BODY, 0);

    lv_obj_t *alarm = complication(row, "alarms");
    lv_obj_t *bell = lv_label_create(alarm);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bell, lv_color_hex(UI_COLOR_ORANGE), 0);
    lv_obj_set_style_text_font(bell, UI_FONT_LARGE, 0);
    s_face.alarm_label = lv_label_create(alarm);
    lv_obj_set_style_text_font(s_face.alarm_label, UI_FONT_SMALL, 0);
}

/*
 * Round layout (412 x 412 on the Watcher). Vertical positions keep every
 * element inside the circle: at y the visible width is 2 * sqrt(r^2 - (r - y)^2).
 */
static void build_round(lv_obj_t *root)
{
    const int32_t size = board_info()->width;

    /* battery: a ring along the edge of the glass */
    s_face.battery_arc = lv_arc_create(root);
    lv_obj_set_size(s_face.battery_arc, size - 6, size - 6);
    lv_obj_center(s_face.battery_arc);
    lv_arc_set_bg_angles(s_face.battery_arc, 0, 360);
    lv_arc_set_rotation(s_face.battery_arc, 270);
    lv_arc_set_range(s_face.battery_arc, 0, 100);
    lv_obj_remove_style(s_face.battery_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_face.battery_arc, false);
    lv_obj_set_style_arc_width(s_face.battery_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_face.battery_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_face.battery_arc, lv_color_hex(UI_COLOR_CARD), LV_PART_MAIN);

    lv_obj_t *status = status_row(root);
    lv_obj_set_size(status, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(status, 14, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 26);
    s_face.status_battery = lv_label_create(status);
    lv_obj_set_style_text_color(s_face.status_battery, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_face.date = lv_label_create(root);
    lv_obj_set_style_text_font(s_face.date, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_face.date, ui_accent(), 0);
    lv_obj_set_style_text_letter_space(s_face.date, 2, 0);
    lv_obj_align(s_face.date, LV_ALIGN_TOP_MID, 0, 70);

    s_face.time = lv_label_create(root);
    lv_obj_set_style_text_font(s_face.time, ui_font_clock, 0);
    lv_obj_set_style_text_letter_space(s_face.time, -4, 0);
    lv_obj_align(s_face.time, LV_ALIGN_TOP_MID, 0, 98);

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 240, 104);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    weather_and_alarm(row);

    music_pill(root, 180);
    lv_obj_align(s_face.music_pill, LV_ALIGN_TOP_MID, 0, 352);
}

void watchface_create(lv_obj_t *parent)
{
    s_face = (face_t){0};

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);

    if (board_info()->round)
    {
        build_round(root);
        watchface_observe(root);
        return;
    }

    /* status row */
    lv_obj_t *status = status_row(root);
    lv_obj_set_size(status, LV_PCT(100), 40);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_pad_hor(status, 22, 0);

    lv_obj_t *spacer = lv_obj_create(status);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    s_face.status_battery = lv_label_create(status);
    lv_obj_set_style_text_color(s_face.status_battery, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    /* date + time */
    s_face.date = lv_label_create(root);
    lv_obj_set_style_text_font(s_face.date, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_face.date, ui_accent(), 0);
    lv_obj_set_style_text_letter_space(s_face.date, 2, 0);
    lv_obj_align(s_face.date, LV_ALIGN_TOP_MID, 0, 62);

    s_face.time = lv_label_create(root);
    lv_obj_set_style_text_font(s_face.time, ui_font_clock, 0);
    lv_obj_set_style_text_letter_space(s_face.time, -4, 0);
    lv_obj_align(s_face.time, LV_ALIGN_TOP_MID, 0, 92);

    /* complications */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 112);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *battery = complication(row, "battery");
    s_face.battery_arc = lv_arc_create(battery);
    lv_obj_set_size(s_face.battery_arc, 92, 92);
    lv_arc_set_bg_angles(s_face.battery_arc, 0, 360);
    lv_arc_set_rotation(s_face.battery_arc, 270);
    lv_arc_set_range(s_face.battery_arc, 0, 100);
    lv_obj_remove_style(s_face.battery_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_face.battery_arc, false);
    lv_obj_set_style_arc_width(s_face.battery_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_face.battery_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_face.battery_arc, lv_color_hex(UI_COLOR_CARD_HI), LV_PART_MAIN);
    lv_obj_set_ignore_layout(s_face.battery_arc, true);
    lv_obj_center(s_face.battery_arc);
    s_face.battery_label = lv_label_create(battery);
    lv_obj_set_style_text_font(s_face.battery_label, UI_FONT_SMALL, 0);

    weather_and_alarm(row);

    /* phone music pill */
    music_pill(root, 300);
    lv_obj_align(s_face.music_pill, LV_ALIGN_BOTTOM_MID, 0, -26);

    watchface_observe(root);
}

/* Observers: removed automatically when `root` is deleted. */
static void watchface_observe(lv_obj_t *root)
{
    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), update_time, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_SETTINGS_VERSION), update_time, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BATTERY), update_battery, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_CHARGING), update_battery, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BLE), update_links, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WIFI), update_links, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WEATHER_VERSION), update_weather, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_ALARM_VERSION), update_alarm, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), update_alarm, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_MUSIC_VERSION), update_music, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BLE), update_music, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_SCREEN), screen_changed, root, NULL);
}

/* ================================================================ AOD */

/*
 * Always-on face: very few lit pixels (AMOLED power ~ lit area), no
 * seconds, gray digits. The whole block moves a few pixels every minute so
 * no pixel stays lit in the same place for hours (burn-in protection).
 */

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *time;
    lv_obj_t *date;
} aod_t;

static aod_t s_aod;

static void aod_update(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    struct tm now;
    char text[16];
    clock_local(&now);
    clock_format_hm(&now, text, sizeof(text));
    lv_label_set_text(s_aod.time, text);
    lv_label_set_text_fmt(s_aod.date, "%s %d", clock_weekday_name(now.tm_wday, true), now.tm_mday);

    static const int8_t shift[][2] = {{0, 0}, {6, 4}, {-6, 8}, {4, -6}, {-4, -4}, {8, 0}, {-8, 6}, {0, -8}};
    int i = now.tm_min % 8;
    lv_obj_align(s_aod.box, LV_ALIGN_CENTER, shift[i][0], shift[i][1] - 20);
}

void watchface_aod_create(lv_obj_t *screen)
{
    s_aod.box = lv_obj_create(screen);
    lv_obj_remove_style_all(s_aod.box);
    lv_obj_set_size(s_aod.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_aod.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_aod.box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_aod.date = lv_label_create(s_aod.box);
    lv_obj_set_style_text_font(s_aod.date, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_aod.date, lv_color_hex(0x7A7A80), 0);

    s_aod.time = lv_label_create(s_aod.box);
    lv_obj_set_style_text_font(s_aod.time, ui_font_clock, 0);
    lv_obj_set_style_text_color(s_aod.time, lv_color_hex(0xB0B0B5), 0);

    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), aod_update, s_aod.box, NULL);
}
