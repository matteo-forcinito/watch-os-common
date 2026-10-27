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
#include <string.h>

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
 *
 * NEON NOIR "Chrono HUD": hours in cold white, minutes in the accent, a colon
 * that blinks at 1 Hz and a seconds "comet" (three small dots: neon head, two
 * glow tails) gliding once per second along the edge (round) or a hairline
 * track (rectangular). Each step invalidates a few 14 x 14 px areas, the CPU
 * rests between steps, and nothing moves in AOD, while the face is off
 * screen or during slides and drags (ui_motion_paused).
 */

#define COMET_DOTS       3
#define COMET_TAIL_STEP  4      /* tenths of a second between comet dots */
#define LIVE_PERIOD_MS   200    /* poll for the second change; cheap (no redraw) */

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *hours;
    lv_obj_t *colon;
    lv_obj_t *minutes;
    lv_obj_t *date;
    lv_obj_t *comet[COMET_DOTS];
    lv_timer_t *live;
    int32_t comet_value;        /* tenths of a second in the minute, 0..599 */
    int last_second;
    int32_t orbit_radius;       /* round: comet radius; rectangular: 0 */
    int32_t track_x;
    int32_t track_y;
    int32_t track_width;
    lv_obj_t *battery_arc;
    lv_obj_t *battery_border;
    lv_obj_t *battery_box;
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

#if CONFIG_APPKIT_HOSTED
static void open_casa(lv_event_t *event)
{
    (void)event;
    app_open("casa", NULL);
}
#endif

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
        char *colon = strchr(text, ':');

        if (colon != NULL)
        {
            *colon = '\0';
            lv_label_set_text(s_face.hours, text);
            lv_label_set_text(s_face.minutes, colon + 1);
        }
        lv_label_set_text_fmt(s_face.date, "%s %d %s", clock_weekday_name(now.tm_wday, true), now.tm_mday,
                              clock_month_name(now.tm_mon, true));
    }
    else
    {
        lv_label_set_text(s_face.hours, "--");
        lv_label_set_text(s_face.minutes, "--");
        lv_label_set_text(s_face.date, "ORA NON IMPOSTATA");
    }
}

static void update_battery(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    int percent = state_get(STATE_BATTERY);
    bool charging = state_get(STATE_CHARGING) != 0;
    uint32_t color = TK_LIME;

    if (!charging && percent >= 0 && percent < 15)
    {
        color = TK_RED;
    }
    else if (!charging && percent >= 0 && percent < 30)
    {
        color = TK_AMBER;
    }

    if (!ui_board_info()->round && settings_get()->border_progress && !s_face.battery_border && s_face.root)
        s_face.battery_border = ui_border_progress_create(s_face.root, color);

    if (s_face.battery_border != NULL)
    {
        ui_border_progress_set(s_face.battery_border, percent < 0 ? 100 : percent, 100);
        ui_border_progress_color(s_face.battery_border, color);
        lv_obj_set_hidden(s_face.battery_border, !settings_get()->border_progress);
    }
    if (s_face.battery_box != NULL)
    {
        /* Keep the card as the fallback when the optional border cannot allocate. */
        lv_obj_set_hidden(s_face.battery_box,
                          s_face.battery_border != NULL && settings_get()->border_progress);
    }
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

/* The face displays cached/phone weather. Opening Weather explicitly refreshes
 * it; waking the clock must not start a network request. */

/* ------------------------------------------------------------- layout */

#define COMPLICATION_SIZE 104

static lv_obj_t *complication(lv_obj_t *parent, const char *open_app, int32_t size)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_make_card(c);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(TK_VOID), 0);
    lv_obj_set_style_border_width(c, TK_LW, 0);
    lv_obj_set_style_border_color(c, lv_color_mix(ui_accent(), lv_color_black(), TK_GLOW_G1), 0);
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
    lv_obj_set_style_pad_column(status, TK_SP_3, 0);
    lv_obj_set_style_text_font(status, UI_FONT_SMALL, 0);
    lv_obj_set_style_radius(status, TK_R_PILL, 0);

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
    lv_obj_set_style_pad_hor(s_face.music_pill, TK_SP_4 + TK_SP_1, 0);
    lv_obj_set_style_bg_color(s_face.music_pill, lv_color_hex(TK_VOID), 0);
    lv_obj_set_style_border_color(s_face.music_pill, lv_color_mix(ui_accent(), lv_color_black(), TK_GLOW_G1), 0);
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

static void weather_and_alarm(lv_obj_t *row, int32_t size)
{
    s_face.weather_box = complication(row, "weather", size);
    s_face.weather_label = lv_label_create(s_face.weather_box);
    lv_obj_set_style_text_font(s_face.weather_label, UI_FONT_BODY, 0);

    lv_obj_t *alarm = complication(row, "alarms", size);
    lv_obj_t *bell = lv_label_create(alarm);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bell, lv_color_hex(UI_COLOR_ORANGE), 0);
    lv_obj_set_style_text_font(bell, UI_FONT_LARGE, 0);
    s_face.alarm_label = lv_label_create(alarm);
    lv_obj_set_style_text_font(s_face.alarm_label, UI_FONT_SMALL, 0);
}

/* Hours, blinking colon, minutes: one flex row, so it centres like the old label. */
static lv_obj_t *time_block(lv_obj_t *root)
{
    lv_obj_t *time = lv_obj_create(root);
    lv_obj_remove_style_all(time);
    lv_obj_set_size(time, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(time, false);
    lv_obj_set_style_text_font(time, ui_font_clock, 0);
    lv_obj_set_style_text_letter_space(time, -4, 0);

    s_face.hours = lv_label_create(time);
    lv_obj_set_style_text_color(s_face.hours, lv_color_hex(TK_TEXT), 0);
    s_face.colon = lv_label_create(time);
    lv_label_set_text_static(s_face.colon, ":");
    lv_obj_set_style_text_color(s_face.colon, ui_accent(), 0);
    s_face.minutes = lv_label_create(time);
    lv_obj_set_style_text_color(s_face.minutes, ui_accent(), 0);
    return time;
}

/* Position of a comet dot `value` tenths of a second into the minute. */
static void comet_place(lv_obj_t *dot, int32_t value)
{
    int32_t size = lv_obj_get_width(dot);
    value = ((value % 600) + 600) % 600;

    if (s_face.orbit_radius > 0)
    {
        const board_info_t *board = ui_board_info();
        int32_t angle = (270 + value * 360 / 600) % 360;
        int32_t x = board->width / 2 + ((s_face.orbit_radius * lv_trigo_cos((int16_t)angle)) >> LV_TRIGO_SHIFT);
        int32_t y = board->height / 2 + ((s_face.orbit_radius * lv_trigo_sin((int16_t)angle)) >> LV_TRIGO_SHIFT);
        lv_obj_set_pos(dot, x - size / 2, y - size / 2);
    }
    else
    {
        lv_obj_set_pos(dot, s_face.track_x + value * s_face.track_width / 600 - size / 2,
                       s_face.track_y - size / 2);
    }
}

static void comet_set(void *head, int32_t value)
{
    (void)head;

    for (int i = 0; i < COMET_DOTS; i++)
    {
        comet_place(s_face.comet[i], value - i * COMET_TAIL_STEP);
    }
}

static void comet_create(lv_obj_t *root)
{
    static const int32_t sizes[COMET_DOTS] = {8, 6, 4};
    static const uint8_t mix[COMET_DOTS] = {255, TK_GLOW_G1, TK_GLOW_G2};

    /* Tails first, so the head draws on top of them. */
    for (int i = COMET_DOTS - 1; i >= 0; i--)
    {
        lv_obj_t *dot = lv_obj_create(root);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, sizes[i], sizes[i]);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_mix(ui_accent(), lv_color_black(), mix[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_clickable(dot, false);
        lv_obj_set_ignore_layout(dot, true);
        s_face.comet[i] = dot;
    }

    s_face.last_second = -1;

    struct tm now;
    clock_local(&now);
    s_face.comet_value = now.tm_sec * 10;
    comet_set(NULL, s_face.comet_value);
}

static bool face_live(void)
{
    return power_screen() < POWER_SCREEN_AOD && lv_screen_active() == lv_obj_get_screen(s_face.root) &&
           !ui_motion_paused() && lv_obj_is_visible(s_face.hours);
}

static void live_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_face.root == NULL || !face_live())
    {
        s_face.last_second = -1;   /* resume by jumping, not by replaying the missed seconds */
        return;
    }

    struct tm now;
    clock_local(&now);

    if (now.tm_sec == s_face.last_second)
    {
        return;
    }

    bool jump = s_face.last_second < 0;
    s_face.last_second = now.tm_sec;

    /* Colon: lit on even seconds, dim on odd ones (a 24 x 90 px redraw). */
    lv_obj_set_style_text_color(s_face.colon, (now.tm_sec % 2) == 0 ? ui_accent()
                                : lv_color_mix(ui_accent(), lv_color_black(), TK_GLOW_G2), 0);

    if (s_face.comet[0] == NULL)
    {
        return;
    }

    int32_t target = now.tm_sec * 10;
    int32_t from = s_face.comet_value;
    s_face.comet_value = target;

    if (jump)
    {
        comet_set(NULL, target);
        return;
    }

    if (target < from)
    {
        target += 600;   /* across the minute: keep moving forward */
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face.comet[0]);
    lv_anim_set_exec_cb(&a, comet_set);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_duration(&a, TK_T_FAST);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void face_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_face.root)
    {
        return;   /* an older face deleted after a new one was built */
    }

    if (s_face.live != NULL)
    {
        lv_timer_delete(s_face.live);
        s_face.live = NULL;
    }

    s_face.root = NULL;
}

static void live_start(lv_obj_t *root)
{
    s_face.live = lv_timer_create(live_tick, LIVE_PERIOD_MS, NULL);
    lv_obj_add_event_cb(root, face_deleted, LV_EVENT_DELETE, NULL);
}

/*
 * Round layout: 412 x 412 on the Watcher, 360 x 360 on the Knob, 466 x 466 on the AMOLED 1.75. Vertical
 * positions keep every element inside the circle: at y the visible width is
 * 2 * sqrt(r^2 - (r - y)^2). On the small panel the same layout would put
 * the complications into the cut-off corners, so it is tighter there.
 */
typedef struct
{
    int32_t status_y;
    int32_t date_y;
    int32_t time_y;
    int32_t complication;
    int32_t row_width;
    int32_t row_y;
    int32_t pill_y;
    int32_t pill_width;
} round_layout_t;

static const round_layout_t s_round_large = {26, 70, 98, COMPLICATION_SIZE, 240, 244, 352, 180};
static const round_layout_t s_round_small = {20, 54, 72, 84, 200, 208, 298, 150};

static void build_round(lv_obj_t *root)
{
    const int32_t size = ui_board_info()->width;
    const bool small = size < 400;
    const round_layout_t *l = small ? &s_round_small : &s_round_large;

    /* The large layout is drawn for 412 px: on bigger panels (466 px) it moves down to stay centred. */
    const int32_t dy = small ? 0 : (size - 412) / 2;

    /* battery: a ring along the edge of the glass */
    s_face.battery_arc = lv_arc_create(root);
    lv_obj_set_size(s_face.battery_arc, size - 6, size - 6);
    lv_obj_center(s_face.battery_arc);
    lv_arc_set_bg_angles(s_face.battery_arc, 0, 360);
    lv_arc_set_rotation(s_face.battery_arc, 270);
    lv_arc_set_range(s_face.battery_arc, 0, 100);
    lv_obj_remove_style(s_face.battery_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_face.battery_arc, false);
    lv_obj_set_style_arc_width(s_face.battery_arc, TK_LW_RING, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_face.battery_arc, TK_LW_RING, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_face.battery_arc, lv_color_hex(TK_LINE), LV_PART_MAIN);

    lv_obj_t *status = status_row(root);
    lv_obj_set_size(status, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(status, 14, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, l->status_y + dy);
    s_face.status_battery = lv_label_create(status);
    lv_obj_set_style_text_color(s_face.status_battery, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_face.date = lv_label_create(root);
    lv_obj_set_style_text_font(s_face.date, small ? UI_FONT_BODY : UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_face.date, lv_color_hex(TK_TEXT_2), 0);
    lv_obj_set_style_text_letter_space(s_face.date, small ? 2 : 3, 0);
    lv_obj_align(s_face.date, LV_ALIGN_TOP_MID, 0, l->date_y + dy);

    s_face.time = time_block(root);
    lv_obj_align(s_face.time, LV_ALIGN_TOP_MID, 0, l->time_y + dy);

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, l->row_width, l->complication);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, l->row_y + dy);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    weather_and_alarm(row, l->complication);

    music_pill(root, l->pill_width);
    lv_obj_align(s_face.music_pill, LV_ALIGN_TOP_MID, 0, l->pill_y + dy);

    /* Seconds comet: just inside the battery ring. */
    s_face.orbit_radius = size / 2 - 3 - TK_LW_RING - TK_SP_2;
    comet_create(root);
}

void watchface_create(lv_obj_t *parent)
{
    s_face = (face_t){0};

    lv_obj_t *root = lv_obj_create(parent);
    s_face.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);

    if (ui_board_info()->round)
    {
        build_round(root);
        watchface_observe(root);
        live_start(root);
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
    lv_obj_set_style_text_color(s_face.date, lv_color_hex(TK_TEXT_2), 0);
    lv_obj_set_style_text_letter_space(s_face.date, 3, 0);
    lv_obj_align(s_face.date, LV_ALIGN_TOP_MID, 0, 62);

    s_face.time = time_block(root);
    lv_obj_align(s_face.time, LV_ALIGN_TOP_MID, 0, 92);

    /* Seconds track: a hairline under the digits, the comet runs along it. */
    const board_info_t *board = ui_board_info();
    s_face.track_width = board->width * 3 / 5;
    s_face.track_x = (board->width - s_face.track_width) / 2;
    s_face.track_y = 234;
    lv_obj_t *track = lv_obj_create(root);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, s_face.track_width, TK_LW_HAIR);
    lv_obj_set_pos(track, s_face.track_x, s_face.track_y);
    lv_obj_set_style_bg_color(track, lv_color_hex(TK_LINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_clickable(track, false);

    /* complications */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 112);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *battery = complication(row, "battery", COMPLICATION_SIZE);
    s_face.battery_box = battery;
    s_face.battery_arc = lv_arc_create(battery);
    lv_obj_set_size(s_face.battery_arc, 92, 92);
    lv_arc_set_bg_angles(s_face.battery_arc, 0, 360);
    lv_arc_set_rotation(s_face.battery_arc, 270);
    lv_arc_set_range(s_face.battery_arc, 0, 100);
    lv_obj_remove_style(s_face.battery_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_face.battery_arc, false);
    lv_obj_set_style_arc_width(s_face.battery_arc, TK_LW_RING, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_face.battery_arc, TK_LW_RING, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_face.battery_arc, lv_color_hex(TK_LINE), LV_PART_MAIN);
    lv_obj_set_ignore_layout(s_face.battery_arc, true);
    lv_obj_center(s_face.battery_arc);
    s_face.battery_label = lv_label_create(battery);
    lv_obj_set_style_text_font(s_face.battery_label, UI_FONT_SMALL, 0);

    weather_and_alarm(row, COMPLICATION_SIZE);

    /* phone music pill */
    music_pill(root, 300);
    lv_obj_align(s_face.music_pill, LV_ALIGN_BOTTOM_MID, 0, -26);

    comet_create(root);
    watchface_observe(root);
    live_start(root);
}

/* Observers: removed automatically when `root` is deleted. */
static void watchface_observe(lv_obj_t *root)
{
#if CONFIG_APPKIT_HOSTED
    /* Hold the clock to open Home Assistant; swipes keep their shell meaning. */
    lv_obj_set_clickable(s_face.time, true);
    lv_obj_add_event_cb(s_face.time, open_casa, LV_EVENT_LONG_PRESSED, NULL);
#endif
    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), update_time, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_SETTINGS_VERSION), update_time, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BATTERY), update_battery, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_CHARGING), update_battery, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_SETTINGS_VERSION), update_battery, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BLE), update_links, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WIFI), update_links, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WEATHER_VERSION), update_weather, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_ALARM_VERSION), update_alarm, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), update_alarm, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_MUSIC_VERSION), update_music, root, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BLE), update_music, root, NULL);
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
    lv_obj_set_style_text_color(s_aod.date, lv_color_hex(TK_TEXT_3), 0);

    s_aod.time = lv_label_create(s_aod.box);
    lv_obj_set_style_text_font(s_aod.time, ui_font_clock, 0);
    lv_obj_set_style_text_color(s_aod.time, lv_color_hex(TK_TEXT_2), 0);

    lv_subject_add_observer_obj(state_subject(STATE_MINUTE), aod_update, s_aod.box, NULL);
}
