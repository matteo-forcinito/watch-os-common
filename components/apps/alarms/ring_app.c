#include "apps_internal.h"

#include "core/clock.h"
#include "core/state.h"
#include "services/alarm.h"
#include "services/sound.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * The ringing alarm, full screen: time, label, Snooze / Stop.
 *
 * The melody gets louder over ~20 s (sound service). The screen stays on,
 * and the alarm stops by itself after 10 minutes (then it counts as
 * dismissed). Snooze rings again after 5 minutes.
 *
 * arg = alarm_t * allocated by the caller, owned (freed) here.
 */

#define AUTO_STOP_MS    (10 * 60 * 1000)
#define SNOOZE_MINUTES  5

static alarm_t s_alarm;
static uint32_t s_started_ms;
static lv_timer_t *s_timer = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_bell = NULL;
static bool s_done = false;

static void finish(bool snooze)
{
    if (s_done)
    {
        return;
    }

    s_done = true;
    sound_alarm_stop();

    if (snooze)
    {
        alarm_snooze(s_alarm.id, SNOOZE_MINUTES);
    }
    else
    {
        alarm_dismiss(s_alarm.id);
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    uint32_t elapsed = lv_tick_elaps(s_started_ms);
    struct tm now;
    char text[12];

    clock_local(&now);
    clock_format_hm(&now, text, sizeof(text));
    lv_label_set_text(s_time, text);

    /* the bell swings */
    lv_obj_set_style_transform_rotation(s_bell, (elapsed / 250) % 2 ? 150 : -150, 0);

    /* If the melody stopped for any reason while ringing, start it again. */
    if (!s_done && !sound_alarm_playing())
    {
        sound_alarm_start();
    }

    if (!s_done && elapsed > AUTO_STOP_MS)
    {
        finish(false);
        app_back();
    }
}

static void on_snooze(lv_event_t *e)
{
    (void)e;

    if (!s_done)
    {
        finish(true);
        ui_toast("Posticipata di 5 minuti");
        app_back();
    }
}

static void on_stop(lv_event_t *e)
{
    (void)e;

    if (!s_done)
    {
        finish(false);
        app_back();
    }
}

static void create(lv_obj_t *screen, void *arg)
{
    s_alarm = *(const alarm_t *)arg;
    free(arg);

    s_started_ms = lv_tick_get();
    s_done = false;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A0E00), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 24, 0);
    lv_obj_set_style_pad_row(screen, 10, 0);

    s_bell = lv_label_create(screen);
    lv_label_set_text(s_bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(s_bell, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_bell, lv_color_hex(UI_COLOR_ORANGE), 0);
    lv_obj_set_style_transform_pivot_x(s_bell, LV_PCT(50), 0);

    s_time = lv_label_create(screen);
    lv_obj_set_style_text_font(s_time, ui_font_clock, 0);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, s_alarm.label[0] != '\0' ? s_alarm.label : "Sveglia");
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);

    lv_obj_t *snooze = ui_button(screen, "Posticipa", UI_COLOR_CARD_HI, on_snooze, NULL);
    lv_obj_set_style_margin_top(snooze, 24, 0);
    ui_button(screen, "Stop", UI_COLOR_ORANGE, on_stop, NULL);

    sound_alarm_start();

    s_timer = lv_timer_create(tick, 500, NULL);
    tick(s_timer);
}

static void destroy(void)
{
    if (s_timer != NULL)
    {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    finish(false);   /* closed any other way (home, auto-home): stop ringing */
}

/* Back (BOOT) = stop; the app manager then closes the screen. */
static bool back(void)
{
    finish(false);
    return false;
}

const app_t ring_app = {
    .id = "system.ring",
    .name = "Sveglia",
    .icon = LV_SYMBOL_BELL,
    .color = UI_COLOR_ORANGE,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_STAY_ON_WAKE | APP_FLAG_NO_BACK_GESTURE,
    .create = create,
    .destroy = destroy,
    .back = back,
};
