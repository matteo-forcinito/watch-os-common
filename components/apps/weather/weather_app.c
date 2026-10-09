#include "apps_internal.h"

#include "core/clock.h"
#include "core/settings.h"
#include "core/state.h"
#include "services/weather.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/*
 * Weather: big temperature and icon, details, next days.
 * Opening the app refreshes data older than 30 minutes (Wi-Fi on for a few
 * seconds), unless the phone sends the weather already.
 */

static lv_obj_t *s_page = NULL;

static int round_temp(float t)
{
    return (int)(t + (t >= 0 ? 0.5f : -0.5f));
}

static void rebuild(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    lv_obj_clean(s_page);

    weather_t w;
    bool valid = weather_get(&w);

    lv_obj_t *city = lv_label_create(s_page);
    lv_label_set_text(city, valid && w.city[0] != '\0' ? w.city : settings_get()->weather_city);
    lv_obj_set_style_text_font(city, ui_font_title, 0);

    if (!valid)
    {
        const char *text = weather_is_updating() ? "Aggiornamento..." :
                           (wifi_known_any() ? "Nessun dato. Tocca per aggiornare." :
                                               "Configura il Wi-Fi o collega il telefono (Gadgetbridge).");
        ui_text(s_page, text, true);
        return;
    }

    lv_obj_t *hero = lv_obj_create(s_page);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 16, 0);

    ui_weather_icon(hero, w.code, !w.is_day, 110);

    lv_obj_t *temp = lv_label_create(hero);
    lv_label_set_text_fmt(temp, "%d°", round_temp(w.temp));
    lv_obj_set_style_text_font(temp, ui_font_big, 0);

    lv_obj_t *desc = lv_label_create(s_page);
    lv_label_set_text_fmt(desc, "%s  ·  %d° / %d°", ui_weather_text(w.code), round_temp(w.temp_min), round_temp(w.temp_max));
    lv_obj_set_style_text_color(desc, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    char value[24];
    snprintf(value, sizeof(value), "%d%%", w.humidity);
    ui_row(s_page, LV_SYMBOL_TINT, UI_COLOR_BLUE, "Umidità", value, NULL, NULL);
    snprintf(value, sizeof(value), "%d km/h", (int)w.wind_kmh);
    ui_row(s_page, LV_SYMBOL_SHUFFLE, UI_COLOR_TEAL, "Vento", value, NULL, NULL);

    if (w.day_count > 1)
    {
        ui_section(s_page, "PROSSIMI GIORNI");
        struct tm today;
        clock_local(&today);

        for (int i = 1; i < w.day_count; i++)
        {
            lv_obj_t *row = lv_obj_create(s_page);
            ui_make_card(row);
            lv_obj_set_size(row, LV_PCT(100), 64);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t *day = lv_label_create(row);
            lv_label_set_text(day, clock_weekday_name((today.tm_wday + i) % 7, false));
            lv_obj_set_width(day, 130);

            ui_weather_icon(row, w.days[i].code, false, 40);

            lv_obj_t *range = lv_label_create(row);
            lv_label_set_text_fmt(range, "%d° / %d°", round_temp(w.days[i].temp_min), round_temp(w.days[i].temp_max));
        }
    }

    struct tm t;
    char when[12];
    localtime_r(&w.updated, &t);
    clock_format_hm(&t, when, sizeof(when));
    char footer[64];
    snprintf(footer, sizeof(footer), "Aggiornato alle %s%s", when, w.from_phone ? " dal telefono" : " · Open-Meteo");
    ui_text(s_page, footer, true);
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    weather_refresh(0);
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_page = ui_page(screen, NULL);
    lv_obj_set_clickable(s_page, true);
    lv_obj_add_event_cb(s_page, on_tap, LV_EVENT_LONG_PRESSED, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WEATHER_VERSION), rebuild, s_page, NULL);
    weather_refresh(30 * 60);
}

const app_t weather_app = {
    .id = "weather",
    .name = "Meteo",
    .icon = LV_SYMBOL_IMAGE,
    .color = UI_COLOR_TEAL,
    .create = create,
};
