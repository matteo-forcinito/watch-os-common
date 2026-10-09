#include "apps_internal.h"

#include "core/power.h"
#include "services/sound.h"
#include "ui/ui.h"

#include "esp_timer.h"

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

    lv_label_set_text(lv_obj_get_child(s_watch_button, 0), s_watch.running ? "Pausa" : "Avvia");
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

    ui_section(page, "CRONOMETRO");
    s_watch_label = lv_label_create(page);
    lv_obj_set_style_text_font(s_watch_label, ui_font_big, 0);

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_watch_button = ui_button(row, s_watch.running ? "Pausa" : "Avvia", UI_COLOR_GREEN, on_watch_toggle, NULL);
    lv_obj_set_size(s_watch_button, 150, 60);
    small_button(row, "Reset", on_watch_reset, NULL);

    ui_section(page, "CONTO ALLA ROVESCIA");
    s_count_label = lv_label_create(page);
    lv_obj_set_style_text_font(s_count_label, ui_font_big, 0);

    lv_obj_t *row2 = lv_obj_create(page);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    small_button(row2, "-1", on_minutes, (void *)(intptr_t)-1);
    small_button(row2, "+1", on_minutes, (void *)(intptr_t)1);
    small_button(row2, "+5", on_minutes, (void *)(intptr_t)5);

    ui_button(page, "Avvia / Stop", UI_COLOR_ORANGE, on_countdown, NULL);

    s_tick = lv_timer_create(tick, 100, NULL);
    tick(s_tick);
}

static void destroy(void)
{
    lv_timer_delete(s_tick);
    s_tick = NULL;
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
