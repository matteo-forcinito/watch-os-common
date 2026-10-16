#include "apps/apps.h"

#include "apps_internal.h"

#if CONFIG_APPKIT_HOSTED
#include "appkit.h"
#endif

#include "core/lv_port.h"
#include "core/power.h"
#include "core/state.h"
#include "core/settings.h"
#include "ui/ui.h"
#include "services/fw_update.h"
#include "services/market.h"
#include "services/alarm.h"
#if CONFIG_WATCH_ECOS_PAIRING
#include "services/ecos.h"
#endif

#include "hardware/board.h"
#include "hardware/microphone.h"
#include "hardware/imu.h"

#include <stdlib.h>
#include <string.h>

void shell_create(void);

/* Launcher order. Hidden apps (sub-pages) are registered too. */
static const app_t *const s_apps[] = {
    &alarms_app,
    &weather_app,
    &timer_app,
    &voice_memo_app,
    &sensors_app,
    &music_app,
    &flashlight_app,
    &web_app,
#if CONFIG_WATCH_ECOS_PAIRING
    &ecos_app,
    &ecos_device_app,
    &ecos_pair_app,
    &ecos_find_app,
#endif
    &usb_drive_app,
    &sdcard_app,
    &battery_app,
    &market_app,
#if CONFIG_APPKIT_HOSTED
    &news_app,
    &giornata_app,
    &pieno_app,
    &copertura_app,
    &bilancio_app,
    &sentinel_app,
    &ombra_app,
#endif
    &settings_app,
    /* hidden */
    &alarm_edit_app,
    &ring_app,
    &extapps_run_app,
    &extapps_open_app,
    &settings_display_app,
    &settings_sound_app,
    &settings_wifi_app,
    &settings_bluetooth_app,
    &settings_time_app,
    &settings_about_app,
    &power_app,
    &call_app,
    &update_app,
    &versions_app,
    &folder_app,
    &launcher_all_app,
    &launcher_apps_app,
    &launcher_actions_app,
    &launcher_details_app,
};

void apps_open_cb(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    app_open(id, NULL);
}

/* ---- alarms ring from the esp_timer task: hop to the lvgl task */

static void open_ring(void *arg)
{
    alarm_t *alarm = arg;

    if (app_current() == &ring_app)
    {
        app_back();   /* a second alarm replaces the first */
    }

    /* ring_app owns `alarm` from here (the open may be queued). */
    if (!app_open_app(&ring_app, alarm))
    {
        free(alarm);
    }
}

static void on_alarm(const alarm_t *alarm)
{
    alarm_t *copy = malloc(sizeof(alarm_t));

    if (copy == NULL)
    {
        return;
    }

    *copy = *alarm;
    power_wake(POWER_WAKE_ALARM);

    if (!ui_async(open_ring, copy))
    {
        free(copy);
    }
}

/*
 * While a firmware is written: one still screen over everything, no
 * animation. Flash writes pause the chip's cache many times a second, so
 * anything that moves would stutter and slow the install down. The progress
 * (bar + percent) is redrawn at most every UPDATE_DRAW_MS: a few ms of
 * drawing every 2 s is nothing next to a download limited by the network.
 * On failure the screen goes away and a toast says why.
 */
#define UPDATE_DRAW_MS  2000

static lv_obj_t *s_update_panel = NULL;
static lv_obj_t *s_update_title = NULL;
static lv_obj_t *s_update_text = NULL;
static lv_obj_t *s_update_bar = NULL;
static lv_obj_t *s_update_border = NULL;
static lv_obj_t *s_update_percent = NULL;
static bool s_update_titled = false;
static uint32_t s_update_drawn_ms = 0;

static void update_panel_show(void)
{
    if (s_update_panel != NULL)
    {
        return;
    }

    s_update_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_update_panel);
    lv_obj_set_size(s_update_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_update_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_update_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_update_panel, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(s_update_panel, 48, 0);
    lv_obj_set_flex_flow(s_update_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_update_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_update_panel, 14, 0);
    lv_obj_set_clickable(s_update_panel, true);   /* swallows touches meanwhile */
    lv_obj_set_scrollable(s_update_panel, false);

    ui_icon_bubble(s_update_panel, LV_SYMBOL_DOWNLOAD, UI_COLOR_BLUE, 88);

    s_update_title = lv_label_create(s_update_panel);
    lv_obj_set_style_text_font(s_update_title, ui_font_title, 0);
    lv_label_set_text(s_update_title, "Aggiornamento");

    s_update_text = lv_label_create(s_update_panel);
    lv_obj_set_width(s_update_text, LV_PCT(100));
    lv_label_set_long_mode(s_update_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_update_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_update_text, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_update_text, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_update_text, "Installazione in corso.\nNon spegnere l'orologio.");

    s_update_bar = lv_bar_create(s_update_panel);
    lv_obj_set_size(s_update_bar, LV_PCT(80), 10);
    lv_bar_set_range(s_update_bar, 0, 100);
    lv_obj_set_style_anim_duration(s_update_bar, 0, 0);   /* jumps: no animation frames */
    lv_obj_set_style_bg_color(s_update_bar, lv_color_hex(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_bg_color(s_update_bar, ui_accent(), LV_PART_INDICATOR);

    s_update_percent = lv_label_create(s_update_panel);
    lv_obj_set_style_text_font(s_update_percent, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_update_percent, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_update_percent, "0%");

    if (settings_get()->border_progress)
    {
        s_update_border = ui_border_progress_create(s_update_panel, UI_COLOR_BLUE);
        lv_obj_set_hidden(s_update_bar, s_update_border != NULL);
    }
    s_update_titled = false;
    s_update_drawn_ms = lv_tick_get();
}

static void update_panel_hide(void)
{
    if (s_update_panel != NULL)
    {
        lv_obj_delete(s_update_panel);
        s_update_panel = NULL;
        s_update_title = NULL;
        s_update_text = NULL;
        s_update_bar = NULL;
        s_update_border = NULL;
        s_update_percent = NULL;
    }
}

static void on_update_phase(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;

    fw_update_phase_t phase = (fw_update_phase_t)lv_subject_get_int(subject);

    if (phase == FW_UPDATE_WRITING)
    {
        power_wake(POWER_WAKE_OTHER);
        update_panel_show();
    }
    else if (phase == FW_UPDATE_RESTARTING && s_update_text != NULL)
    {
        lv_label_set_text(s_update_text, "Verificato. Riavvio...");
        lv_bar_set_value(s_update_bar, 100, LV_ANIM_OFF);
        ui_border_progress_set(s_update_border, 100, 100);
        lv_label_set_text(s_update_percent, "100%");
    }
    else if (phase == FW_UPDATE_FAILED && s_update_panel != NULL)
    {
        update_panel_hide();
        ui_toast(fw_update_message()[0] != '\0' ? fw_update_message() : "Aggiornamento non riuscito");
    }
}

/* Progress: the version as soon as the file header tells it, then the percent every UPDATE_DRAW_MS. */
static void on_update_percent(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;

    int percent = lv_subject_get_int(subject);

    if (s_update_panel == NULL || percent <= 0)
    {
        return;
    }

    if (!s_update_titled && fw_update_new_version()[0] != '\0')
    {
        lv_label_set_text_fmt(s_update_title, "Versione %s", fw_update_new_version());
        s_update_titled = true;
    }

    if (lv_tick_elaps(s_update_drawn_ms) < UPDATE_DRAW_MS)
    {
        return;
    }

    s_update_drawn_ms = lv_tick_get();
    lv_bar_set_value(s_update_bar, percent, LV_ANIM_OFF);
    ui_border_progress_set(s_update_border, percent, 100);
    lv_label_set_text_fmt(s_update_percent, "%d%%", percent);
}

static void on_focus_changed(bool on)
{
    ui_toast(on ? "Focus: gira per scegliere; tocca per aprire" : "Rotella: scorrimento");
}

/* "Back" out of an app asks first (sub-pages close at once). */
static bool s_asking_close;

static void close_answer(bool confirmed, void *arg)
{
    s_asking_close = false;
    /* Only the app asked about (an alarm may have opened on top meanwhile). */
    if (confirmed && app_current() == (const app_t *)arg)
    {
        app_close_top();
    }
}

static void ask_close(const app_t *app)
{
    if (s_asking_close)
    {
        return;   /* the question is on screen already */
    }
    s_asking_close = true;
    static char title[48];
    snprintf(title, sizeof(title), "Chiudere %s?", app->name);
    ui_confirm(title, NULL, "Chiudi", UI_COLOR_RED, close_answer, (void *)app);
}

void apps_init(void)
{
#if CONFIG_WATCH_ECOS_PAIRING
    ecos_init();
    ecos_apps_init();   /* another device can ring this one */
#endif
    app_manager_set_close_confirm(ask_close);
    market_init();
#if CONFIG_APPKIT_HOSTED
    giornata_service_start();   /* reminders ring with the app closed too */
    news_service_start();       /* background rounds keep the archive growing */
    /* Dashboard folders exist from boot: opening one opens its app (no cost until then). */
    appkit_hosted_register("News", "news");
    appkit_hosted_register("Giornata", "giornata");
    appkit_hosted_register("Pieno", "pieno");
    appkit_hosted_register("Copertura", "copertura");
    appkit_hosted_register("Bilancio", "bilancio");
    appkit_hosted_register("Sentinel", "sentinel");
#endif
    for (size_t i = 0; i < sizeof(s_apps) / sizeof(s_apps[0]); i++)
    {
        /* The USB drive lends the card to the PC: it needs both the ESP32's own USB port and a card slot. */
        if (s_apps[i] == &usb_drive_app && (!board_info()->has_usb_otg || !board_info()->has_sdcard))
        {
            continue;
        }

        if (s_apps[i] == &sdcard_app && !board_info()->has_sdcard)
        {
            continue;
        }

        if (s_apps[i] == &voice_memo_app && !microphone_supported())
        {
            continue;
        }

        if (s_apps[i] == &sensors_app && !imu_supported())
        {
            continue;
        }

        app_register(s_apps[i]);
    }

    /* Before the shell: the watch face asks for the next alarm as soon as it exists. */
    alarm_service_init(on_alarm);
    shell_create();

    lv_subject_add_observer(state_subject(STATE_UPDATE), on_update_phase, NULL);
    lv_subject_add_observer(state_subject(STATE_UPDATE_PERCENT), on_update_percent, NULL);
    app_manager_set_focus_cb(on_focus_changed);
}
