/*
 * Casa alerts (OS images with hosted apps).
 *
 * While the device runs on USB power and the alerts are enabled (Casa >
 * settings), Casa keeps its Home Assistant session open (the WebSocket only,
 * no setup page) and watches the thermostat and the alarm:
 *
 *   thermostat starts / stops heating   notification + short sound
 *   alarm armed / disarmed              notification + short sound
 *   alarm triggered                     full screen, alarm sound until touched
 *
 * On battery the session is released after a minute: nothing keeps Wi-Fi on.
 * The first states after a (re)connection only set the baseline, they never
 * alert.
 */
#include "casa.h"

#include "core/app.h"
#include "core/power.h"
#include "core/state.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "hardware/board.h"
#include "nvs.h"
#include "services/notify.h"
#include "services/sound.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define ALERT_PERIOD_MS 1000
#define BATTERY_GRACE_MS 60000

static lv_timer_t *s_timer;
static bool s_holding;          /* this module owns a Casa session */
static uint32_t s_battery_since;
static bool s_baseline;
static char s_climate_action[16], s_alarm_state[24];
static char s_alarm_name[CASA_NAME];
static int s_enabled = -1;      /* -1 = not read yet */
static uint32_t s_cycling_told; /* ms of the last short cycling warning, 0 = never */

bool casa_alerts_enabled(void)
{
    if (s_enabled < 0)
    {
        /* Default: on for panels that live on a desk (wide or tall bars). */
        const board_info_t *b = board_info();
        uint8_t value = !b->round && (b->width >= 2 * b->height || b->height >= 2 * b->width);
        nvs_handle_t nvs;
        if (nvs_open("casa", NVS_READONLY, &nvs) == ESP_OK)
        {
            nvs_get_u8(nvs, "alerts", &value);
            nvs_close(nvs);
        }
        s_enabled = value;
    }
    return s_enabled == 1;
}

void casa_alerts_set(bool on)
{
    s_enabled = on;
    nvs_handle_t nvs;
    if (nvs_open("casa", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u8(nvs, "alerts", on);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ---- full-screen alarm alert */

static lv_timer_t *s_ring;

static void ring_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!sound_alarm_playing()) sound_alarm_start();
}

static void ring_dismiss(lv_event_t *event)
{
    (void)event;
    sound_alarm_stop();
    app_back();
}

static void alert_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x3A0710), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0A0204), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, ring_dismiss, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(screen);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFF5A6E), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ALLARME");
    lv_obj_set_style_text_font(title, ui_font_headline, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF5A6E), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *name = lv_label_create(screen);
    lv_label_set_text(name, s_alarm_name[0] ? s_alarm_name : "Casa");
    lv_obj_set_style_text_font(name, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Tocca per silenziare");
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xC9A0A8), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    sound_alarm_start();
    s_ring = lv_timer_create(ring_tick, 1000, NULL);
}

static void alert_destroy(void)
{
    if (s_ring) lv_timer_delete(s_ring);
    s_ring = NULL;
    sound_alarm_stop();
}

const app_t casa_alert_app = {
    .id = "casa.alert",
    .name = "Allarme casa",
    .icon = LV_SYMBOL_WARNING,
    .color = 0xFF5A6E,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
    .create = alert_create,
    .destroy = alert_destroy,
};

/* ---- watching */

static void tell(const char *title, const char *body)
{
    notify_t n = {0};
    n.time = time(NULL);
    snprintf(n.app, sizeof(n.app), "Casa");
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.body, sizeof(n.body), "%s", body);
    notify_add(&n);
    sound_notification();
    if (power_screen() == POWER_SCREEN_ON) ui_toast(title);
}

static const char *alarm_words(const char *state)
{
    return !strcmp(state, "disarmed") ? "disinserito" : !strcmp(state, "armed_away") ? "inserito (fuori casa)"
         : !strcmp(state, "armed_home") ? "inserito (in casa)" : !strcmp(state, "armed_night") ? "inserito (notte)"
         : !strncmp(state, "armed_", 6) ? "inserito" : NULL;
}

static void watch(void)
{
    casa_status_t status;
    casa_status(&status);
    if (!status.connected || !status.count)
    {
        s_baseline = false;   /* after a reconnection, the first states are the new baseline */
        return;
    }
    casa_entity_t *list = heap_caps_malloc(sizeof(casa_entity_t) * (size_t)status.count, MALLOC_CAP_SPIRAM);
    if (list == NULL) return;
    int count = casa_entities(list, status.count);
    const casa_entity_t *climate = NULL, *alarm = NULL;
    for (int i = 0; i < count; i++)
    {
        const casa_entity_t *e = &list[i];
        if (!e->known || !e->available) continue;
        if (e->kind == CASA_CLIMATE && !climate) climate = e;
        if (e->kind == CASA_ALARM && (!alarm || strstr(e->id, "alarmo"))) alarm = e;
    }
    char climate_action[16] = "", alarm_state[24] = "";
    if (climate) snprintf(climate_action, sizeof(climate_action), "%s", !strcmp(climate->state, "off") ? "off" : climate->hvac_action);
    if (alarm)
    {
        snprintf(alarm_state, sizeof(alarm_state), "%s", alarm->state);
        snprintf(s_alarm_name, sizeof(s_alarm_name), "%s", alarm->name);
    }

    if (s_baseline)
    {
        bool was_heating = !strcmp(s_climate_action, "heating"), heating = !strcmp(climate_action, "heating");
        int starts = climate ? casa_heat_starts(climate, CASA_HOUR_MS) : 0;
        if (climate && heating != was_heating && starts >= CASA_SHORT_CYCLE_STARTS)
        {
            /* A boiler cycling too fast: one warning an hour instead of a stream of on/off. */
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (!s_cycling_told || now - s_cycling_told >= CASA_HOUR_MS)
            {
                char body[CASA_NAME + 48];
                snprintf(body, sizeof(body), "%d accensioni nell'ultima ora - %s", starts, climate->name);
                tell("Caldaia: cicli troppo brevi", body);
                s_cycling_told = now ? now : 1;
            }
        }
        else if (climate && heating != was_heating)
            tell(heating ? "Caldaia accesa" : "Caldaia spenta", climate->name);
        if (alarm && strcmp(alarm_state, s_alarm_state))
        {
            if (!strcmp(alarm_state, "triggered"))
            {
                power_wake(POWER_WAKE_OTHER);
                app_open_app(&casa_alert_app, NULL);
                tell("ALLARME", alarm->name);
            }
            else if (alarm_words(alarm_state) && (alarm_words(s_alarm_state) || !strcmp(s_alarm_state, "triggered")))
            {
                char title[48];
                snprintf(title, sizeof(title), "Allarme %s", alarm_words(alarm_state));
                tell(title, alarm->name);
            }
        }
    }
    snprintf(s_climate_action, sizeof(s_climate_action), "%s", climate_action);
    snprintf(s_alarm_state, sizeof(s_alarm_state), "%s", alarm_state);
    s_baseline = true;
    heap_caps_free(list);
}

static void alerts_tick(lv_timer_t *timer)
{
    (void)timer;
    bool usb = state_get(STATE_USB) != 0;
    if (usb) s_battery_since = 0;
    else if (s_battery_since == 0) s_battery_since = lv_tick_get();
    bool battery_long = !usb && lv_tick_elaps(s_battery_since) >= BATTERY_GRACE_MS;
    bool want = casa_alerts_enabled() && !battery_long;
    if (want && !s_holding)
    {
        s_holding = casa_background(true) == ESP_OK;
        s_baseline = false;
    }
    else if (!want && s_holding)
    {
        casa_background(false);
        s_holding = false;
        s_baseline = false;
    }
    if (s_holding) watch();
}

void casa_alerts_init(void)
{
    if (s_timer == NULL) s_timer = lv_timer_create(alerts_tick, ALERT_PERIOD_MS, NULL);
}
