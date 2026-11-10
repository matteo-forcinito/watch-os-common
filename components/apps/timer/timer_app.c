#include "apps_internal.h"

#include "core/power.h"
#include "core/settings.h"
#include "services/sound.h"
#include "ui/ui.h"

#include "esp_timer.h"

#include <string.h>

/*
 * Stopwatch and countdown timer.
 *
 * Time is measured with esp_timer_get_time() (keeps counting in light
 * sleep), so the stopwatch is right even if the screen went off. The
 * countdown uses an esp_timer that fires even with the screen off and rings.
 */

typedef struct
{
    bool running;
    int64_t started_us;
    int64_t accumulated_us;
} stopwatch_t;

static stopwatch_t s_watch;
static esp_timer_handle_t s_countdown = NULL;
static int64_t s_countdown_end_us = 0;
static int s_countdown_minutes = 5;
static int64_t s_countdown_duration_us = 0;
static lv_obj_t *s_border = NULL;

static lv_obj_t *s_watch_label = NULL;
static lv_obj_t *s_watch_button = NULL;
static lv_obj_t *s_count_label = NULL;
static lv_timer_t *s_tick = NULL;

static int64_t watch_elapsed(void)
{
    return s_watch.accumulated_us + (s_watch.running ? esp_timer_get_time() - s_watch.started_us : 0);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_border != NULL)
    {
        int64_t remaining = s_countdown_end_us - esp_timer_get_time();
        int32_t fraction = s_countdown_end_us != 0 && s_countdown_duration_us > 0 && remaining > 0 ?
            (int32_t)(remaining * 1000 / s_countdown_duration_us) : 0;
        lv_obj_set_hidden(s_border, s_countdown_end_us == 0);
        ui_border_progress_set(s_border, fraction, 1000);
    }
    int64_t ms = watch_elapsed() / 1000;
    lv_label_set_text_fmt(s_watch_label, "%02d:%02d.%d", (int)(ms / 60000), (int)(ms / 1000 % 60), (int)(ms / 100 % 10));

    if (s_countdown_end_us != 0)
    {
        int64_t left = (s_countdown_end_us - esp_timer_get_time()) / 1000000;
        left = left < 0 ? 0 : left;
        lv_label_set_text_fmt(s_count_label, "%02d:%02d", (int)(left / 60), (int)(left % 60));
    }
    else
    {
        lv_label_set_text_fmt(s_count_label, "%02d:00", s_countdown_minutes);
    }
}

static void countdown_done(void *arg)
{
    (void)arg;
    s_countdown_end_us = 0;
    power_wake(POWER_WAKE_ALARM);
    sound_alarm_start();
}

static void on_watch_toggle(lv_event_t *e)
{
    (void)e;

    if (s_watch.running)
    {
        s_watch.accumulated_us += esp_timer_get_time() - s_watch.started_us;
        s_watch.running = false;
    }
    else
    {
        s_watch.started_us = esp_timer_get_time();
        s_watch.running = true;
    }

    if (s_watch_button != NULL)
    {
        lv_label_set_text(lv_obj_get_child(s_watch_button, 0), s_watch.running ? "Pausa" : "Avvia");
    }
}

static void on_watch_reset(lv_event_t *e)
{
    (void)e;
    s_watch = (stopwatch_t){0};
    lv_label_set_text(lv_obj_get_child(s_watch_button, 0), "Avvia");
}

static void on_minutes(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_countdown_end_us == 0)
    {
        s_countdown_minutes += delta;
        s_countdown_minutes = s_countdown_minutes < 1 ? 1 : (s_countdown_minutes > 120 ? 120 : s_countdown_minutes);
    }
}

static void on_countdown(lv_event_t *e)
{
    (void)e;

    sound_alarm_stop();

    if (s_countdown_end_us != 0)
    {
        esp_timer_stop(s_countdown);
        s_countdown_end_us = 0;
        return;
    }

    uint64_t us = (uint64_t)s_countdown_minutes * 60 * 1000000;
    s_countdown_duration_us = (int64_t)us;
    s_countdown_end_us = esp_timer_get_time() + (int64_t)us;
    esp_timer_start_once(s_countdown, us);
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = ui_button(parent, text, UI_COLOR_CARD_HI, cb, user_data);
    lv_obj_set_size(b, 96, 56);
    return b;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    if (s_countdown == NULL)
    {
        const esp_timer_create_args_t args = {.callback = countdown_done, .name = "countdown"};
        esp_timer_create(&args, &s_countdown);
    }

    lv_obj_t *page = ui_page(screen, "Timer");
    bool bar = ui_board_info()->height <= 200 && ui_board_info()->width >= 2 * ui_board_info()->height;
    bool narrow = ui_board_info()->width < 260;
    lv_obj_t *watch_page = page;
    lv_obj_t *count_page = page;
    if (bar)
    {
        /* Two independent panels keep the time and its controls together. */
        watch_page = lv_obj_create(page);
        count_page = lv_obj_create(page);
        lv_obj_t *panels[] = {watch_page, count_page};
        for (unsigned i = 0; i < 2; ++i)
        {
            lv_obj_remove_style_all(panels[i]);
            lv_obj_set_size(panels[i], LV_PCT(49), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(panels[i], LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(panels[i], 4, 0);
        }
    }

    ui_section(watch_page, "CRONOMETRO");
    s_watch_label = lv_label_create(watch_page);
    lv_obj_set_style_text_font(s_watch_label, bar || narrow ? UI_FONT_LARGE : ui_font_big, 0);

    lv_obj_t *row = lv_obj_create(watch_page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, narrow ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    if (narrow) lv_obj_set_style_pad_row(row, 8, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_watch_button = ui_button(row, s_watch.running ? "Pausa" : "Avvia", UI_COLOR_GREEN, on_watch_toggle, NULL);
    lv_obj_set_size(s_watch_button, bar ? 116 : 150, 56);
    if (bar)
        lv_obj_set_style_min_height(s_watch_button, 56, 0);
    lv_obj_t *reset = small_button(row, "Reset", on_watch_reset, NULL);
    if (narrow)
    {
        lv_obj_set_width(s_watch_button, LV_PCT(100));
        lv_obj_set_width(reset, LV_PCT(100));
    }

    ui_section(count_page, "CONTO ALLA ROVESCIA");
    s_count_label = lv_label_create(count_page);
    lv_obj_set_style_text_font(s_count_label, bar || narrow ? UI_FONT_LARGE : ui_font_big, 0);

    lv_obj_t *row2 = lv_obj_create(count_page);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *minus = small_button(row2, "-1", on_minutes, (void *)(intptr_t)-1);
    lv_obj_t *plus = small_button(row2, "+1", on_minutes, (void *)(intptr_t)1);
    lv_obj_t *plus5 = small_button(row2, "+5", on_minutes, (void *)(intptr_t)5);
    if (narrow)
    {
        lv_obj_set_width(minus, LV_PCT(30));
        lv_obj_set_width(plus, LV_PCT(30));
        lv_obj_set_width(plus5, LV_PCT(30));
        lv_obj_set_style_pad_hor(minus, 0, 0);
        lv_obj_set_style_pad_hor(plus, 0, 0);
        lv_obj_set_style_pad_hor(plus5, 0, 0);
    }
    if (bar)
    {
        lv_obj_set_size(minus, 72, 40);
        lv_obj_set_size(plus, 72, 40);
        lv_obj_set_size(plus5, 72, 40);
        lv_obj_set_style_min_height(minus, 40, 0);
        lv_obj_set_style_min_height(plus, 40, 0);
        lv_obj_set_style_min_height(plus5, 40, 0);
    }

    lv_obj_t *count_button = ui_button(count_page, "Avvia / Stop", UI_COLOR_ORANGE, on_countdown, NULL);
    if (bar)
    {
        lv_obj_set_height(count_button, 40);
        lv_obj_set_style_min_height(count_button, 40, 0);
    }

    if (settings_get()->border_progress)
    {
        s_border = ui_border_progress_create(screen, UI_COLOR_ORANGE);
    }
    s_tick = lv_timer_create(tick, 100, NULL);
    tick(s_tick);
}

static void destroy(void)
{
    lv_timer_delete(s_tick);
    s_border = NULL;
    s_tick = NULL;
    s_watch_button = NULL;
}

const app_t timer_app = {
    .id = "timer",
    .name = "Timer",
    .icon = LV_SYMBOL_LOOP,
    .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_STAY_ON_WAKE,
    .create = create,
    .destroy = destroy,
};

/* ------------------------------------------------------------ remote API */

/* Orbit Link: the same countdown and stopwatch, driven from the companion,
 * with the app open or closed. LVGL locked by the caller. */
void timer_remote_state(timer_remote_t *out)
{
    int64_t now = esp_timer_get_time();
    out->countdown_running = s_countdown_end_us != 0;
    out->countdown_total_s = (int32_t)(s_countdown_duration_us / 1000000);
    out->countdown_remaining_s = s_countdown_end_us != 0 ? (int32_t)((s_countdown_end_us - now) / 1000000) : 0;
    out->countdown_ringing = s_countdown_end_us == 0 && s_countdown_duration_us != 0 && sound_alarm_playing();
    out->watch_running = s_watch.running;
    out->watch_elapsed_ms = watch_elapsed() / 1000;
}

bool timer_remote(const char *action, int seconds)
{
    if (strcmp(action, "start") == 0)
    {
        if (seconds < 1 || seconds > 24 * 3600)
        {
            return false;
        }
        if (s_countdown == NULL)
        {
            const esp_timer_create_args_t args = {.callback = countdown_done, .name = "countdown"};
            if (esp_timer_create(&args, &s_countdown) != ESP_OK)
            {
                return false;
            }
        }
        esp_err_t stop_err = esp_timer_stop(s_countdown);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE)
        {
            return false;
        }
        sound_alarm_stop();
        s_countdown_minutes = seconds / 60 > 0 ? (seconds / 60 > 120 ? 120 : seconds / 60) : 1;
        s_countdown_duration_us = (int64_t)seconds * 1000000;
        s_countdown_end_us = esp_timer_get_time() + s_countdown_duration_us;
        if (esp_timer_start_once(s_countdown, (uint64_t)s_countdown_duration_us) != ESP_OK)
        {
            s_countdown_end_us = 0;
            s_countdown_duration_us = 0;
            return false;
        }
        return true;
    }
    if (strcmp(action, "stop") == 0)
    {
        esp_err_t stop_err = ESP_ERR_INVALID_STATE;
        if (s_countdown != NULL)
        {
            stop_err = esp_timer_stop(s_countdown);
        }
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) return false;
        sound_alarm_stop();
        s_countdown_end_us = 0;
        s_countdown_duration_us = 0;
        return true;
    }
    if (strcmp(action, "watch") == 0)
    {
        on_watch_toggle(NULL);
        return true;
    }
    if (strcmp(action, "watch_reset") == 0)
    {
        s_watch = (stopwatch_t){0};
        return true;
    }
    return false;
}
