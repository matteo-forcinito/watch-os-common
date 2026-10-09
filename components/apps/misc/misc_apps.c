#include "apps_internal.h"

#include "core/power.h"
#include "core/state.h"
#include "hardware/display.h"
#include "companion/ble_companion.h"
#include "services/notify.h"
#include "ui/ui.h"

/* ================================================================ torch */

/* White screen at full brightness; the screen stays on while it is open. */
static void torch_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    display_set_brightness(255);
}

static void torch_destroy(void)
{
    power_settings_changed();   /* back to the user's brightness */
}

const app_t flashlight_app = {
    .id = "flashlight",
    .name = "Torcia",
    .icon = LV_SYMBOL_TINT,
    .color = UI_COLOR_YELLOW,
    .flags = APP_FLAG_KEEP_SCREEN_ON,
    .create = torch_create,
    .destroy = torch_destroy,
};

/* ================================================================ power */

static void do_restart(bool yes, void *user_data)
{
    (void)user_data;

    if (yes)
    {
        power_restart();
    }
}

static void do_shutdown(bool yes, void *user_data)
{
    (void)user_data;

    if (yes)
    {
        power_shutdown();
    }
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    ui_confirm("Riavviare?", NULL, "Riavvia", UI_COLOR_ORANGE, do_restart, NULL);
}

static void on_shutdown(lv_event_t *e)
{
    (void)e;
    ui_confirm("Spegnere?", "Per riaccendere premi PWR.", "Spegni", UI_COLOR_RED, do_shutdown, NULL);
}

static void on_sleep(lv_event_t *e)
{
    (void)e;
    app_back();
    power_sleep_now();
}

static void power_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Alimentazione");
    ui_row(page, LV_SYMBOL_EYE_CLOSE, UI_COLOR_INDIGO, "Spegni schermo", NULL, on_sleep, NULL);
    ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_ORANGE, "Riavvia", NULL, on_restart, NULL);
    ui_row(page, LV_SYMBOL_POWER, UI_COLOR_RED, "Spegni", NULL, on_shutdown, NULL);
}

const app_t power_app = {
    .id = "system.power",
    .name = "Alimentazione",
    .icon = LV_SYMBOL_POWER,
    .color = UI_COLOR_RED,
    .flags = APP_FLAG_HIDDEN,
    .create = power_create,
};

/* ================================================================= call */

static lv_obj_t *s_call_name = NULL;

static void close_call(void *arg)
{
    (void)arg;

    if (app_current() == &call_app)
    {
        app_back();
    }
}

static void call_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    notify_call_t call;

    if (!notify_get_call(&call))
    {
        lv_async_call(close_call, NULL);   /* the call is over (not from inside the observer) */
        return;
    }

    lv_label_set_text(s_call_name, call.name[0] != '\0' ? call.name : call.number);
}

static void on_call_command(lv_event_t *e)
{
    ble_companion_call_command(lv_event_get_user_data(e));
    notify_call_t none = {0};
    notify_set_call(&none);
}

static void call_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 24, 0);
    lv_obj_set_style_pad_row(screen, 14, 0);

    ui_icon_bubble(screen, LV_SYMBOL_CALL, UI_COLOR_GREEN, 96);

    lv_obj_t *caption = lv_label_create(screen);
    lv_label_set_text(caption, "Chiamata in arrivo");
    lv_obj_set_style_text_color(caption, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_call_name = lv_label_create(screen);
    lv_obj_set_style_text_font(s_call_name, ui_font_headline, 0);
    lv_label_set_long_mode(s_call_name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_call_name, LV_PCT(100));
    lv_obj_set_style_text_align(s_call_name, LV_TEXT_ALIGN_CENTER, 0);

    ui_button(screen, "Rifiuta", UI_COLOR_RED, on_call_command, (void *)"reject");
    ui_button(screen, "Silenzia", UI_COLOR_CARD_HI, on_call_command, (void *)"ignore");

    lv_subject_add_observer_obj(state_subject(STATE_NOTIF_VERSION), call_changed, s_call_name, NULL);
}

const app_t call_app = {
    .id = "system.call",
    .name = "Chiamata",
    .icon = LV_SYMBOL_CALL,
    .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
    .create = call_create,
};

/* ================================================================ music */

/* Phone music remote: the "now playing" tile as a full app. */
static void music_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    now_playing_create(screen);
}

const app_t music_app = {
    .id = "music",
    .name = "Musica",
    .icon = LV_SYMBOL_PLAY,
    .color = UI_COLOR_RED,
    .create = music_create,
};
