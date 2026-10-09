#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "companion/ble_companion.h"
#include "services/wifi.h"
#include "ui/ui.h"

/*
 * Control center (swipe down from the watch face): quick toggles in round
 * buttons, brightness and volume sliders, power.
 *
 *    ( ᛒ )  ( ◐ )  ( wifi )  ( torch )
 *    [ ☀ brightness ─────●── ]
 *    [ ♪ volume     ───●──── ]
 *    ( ⚙ settings )  ( ⏻ )
 */

typedef struct
{
    lv_obj_t *ble;
    lv_obj_t *aod;
    lv_obj_t *wifi;
    lv_obj_t *battery;
} cc_t;

static cc_t s_cc;

static lv_obj_t *round_toggle(lv_obj_t *parent, const char *icon, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_obj_create(parent);
    ui_make_card(button);
    lv_obj_set_clickable(button, true);
    lv_obj_set_size(button, 72, 72);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_port_set_focusable(button);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_obj_center(label);

    return button;
}

static void paint(lv_obj_t *button, bool on, uint32_t color)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(on ? color : UI_COLOR_CARD), 0);
}

static void refresh(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    paint(s_cc.ble, state_get(STATE_BLE) != STATE_BLE_OFF, UI_COLOR_BLUE);
    paint(s_cc.aod, settings_get()->always_on, UI_COLOR_INDIGO);
    paint(s_cc.wifi, state_get(STATE_WIFI) == STATE_WIFI_CONNECTED, UI_COLOR_BLUE);

    int percent = state_get(STATE_BATTERY);

    if (percent >= 0)
    {
        lv_label_set_text_fmt(s_cc.battery, "%s  %d%%%s", ui_battery_symbol(percent), percent,
                              state_get(STATE_CHARGING) ? "  in carica" : "");
    }
    else
    {
        lv_label_set_text(s_cc.battery, LV_SYMBOL_USB "  alimentato da USB");
    }
}

static void toggle_ble(lv_event_t *e)
{
    (void)e;

    settings_t s = *settings_get();
    s.ble_enabled = !s.ble_enabled;
    settings_save(&s);   /* the companion follows the setting */
}

static void toggle_aod(lv_event_t *e)
{
    (void)e;

    settings_t s = *settings_get();
    s.always_on = !s.always_on;
    settings_save(&s);
    ui_toast(s.always_on ? "Always on attivo" : "Always on disattivato");
}



static void brightness_changed(lv_event_t *e)
{
    settings_t s = *settings_get();
    s.brightness = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    settings_save(&s);
    power_settings_changed();
}

static void volume_changed(lv_event_t *e)
{
    settings_t s = *settings_get();
    s.volume = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    settings_save(&s);
}

void control_center_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_page(parent, NULL);
    lv_obj_set_style_pad_top(page, 26, 0);

    s_cc.battery = lv_label_create(page);
    lv_obj_set_style_text_color(s_cc.battery, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_cc.battery, UI_FONT_SMALL, 0);

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 8, 0);

    s_cc.ble = round_toggle(row, LV_SYMBOL_BLUETOOTH, toggle_ble, NULL);
    s_cc.aod = round_toggle(row, LV_SYMBOL_EYE_OPEN, toggle_aod, NULL);
    s_cc.wifi = round_toggle(row, LV_SYMBOL_WIFI, apps_open_cb, (void *)"settings.wifi");
    round_toggle(row, LV_SYMBOL_TINT, apps_open_cb, (void *)"flashlight");

    ui_slider_row(page, LV_SYMBOL_IMAGE, "Luminosità", 10, 255, settings_get()->brightness, brightness_changed, NULL);
    ui_slider_row(page, LV_SYMBOL_VOLUME_MAX, "Volume", 0, 100, settings_get()->volume, volume_changed, NULL);

    ui_row(page, LV_SYMBOL_SETTINGS, UI_COLOR_GRAY, "Impostazioni", NULL, apps_open_cb, (void *)"settings");
    ui_row(page, LV_SYMBOL_POWER, UI_COLOR_RED, "Spegni / riavvia", NULL, apps_open_cb, (void *)"system.power");

    lv_subject_add_observer_obj(state_subject(STATE_BLE), refresh, page, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_SETTINGS_VERSION), refresh, page, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_WIFI), refresh, page, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_BATTERY), refresh, page, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_CHARGING), refresh, page, NULL);
}
