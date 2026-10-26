#include "core/clock.h"
#include "core/settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "giornata.h"
#include "hardware/board.h"
#include "hardware/button.h"
#include "hardware/display.h"
#include "hardware/knob.h"
#include "hardware/pmu.h"
#include "hardware/touch.h"
#include "hardware/usb_console.h"
#include "internal.h"
#include "life_store.h"
#include "lite.h"
#include "content.h"
#include "nvs_flash.h"
#include "services/storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
static atomic_int ui_page, ui_idle;
void lite_ui_snapshot(int *page, bool *idle)
{
    *page = atomic_load(&ui_page); *idle = atomic_load(&ui_idle) != 0;
}
#ifdef LITE_QA
static bool simulated_touch, simulated_down;
static uint16_t simulated_x, simulated_y;
#endif
static bool read_touch(uint16_t *x, uint16_t *y)
{
#ifdef LITE_QA
    if (simulated_touch) { *x = simulated_x; *y = simulated_y; return simulated_down; }
#endif
    return touch_read(x, y);
}

static void *json_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void key(button_id_t button, button_event_t event)
{
    lite_post(LITE_BUTTON, button, event);
}
static void knob(int delta)
{
    lite_post(LITE_KNOB, delta, 0);
}
static void touch(void *ctx)
{
    (void)ctx;
    lite_event_t e = {LITE_TOUCH, 0, 0};
    if (xPortInIsrContext())
    {
        BaseType_t wake = pdFALSE;
        xQueueSendFromISR(lite_events, &e, &wake);
        if (wake)
            portYIELD_FROM_ISR();
    }
    else
        xQueueSend(lite_events, &e, 0);
}
void lite_return(void)
{
    /* A queued care write must finish before selecting the launcher. */
    for (int i = 0; i < 40; i++)
    {
        bool saving = false;
        for (int k = 0; k < LIFE_COUNT; k++)
        {
            cJSON *s = life_snapshot((life_kind_t)k);
            saving |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "saving"));
            cJSON_Delete(s);
        }
        if (!saving)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    gd_store_flush(true);
    settings_flush();
    clock_checkpoint();
    extapp_return_to_launcher();
}
static void action(lite_action_t selected, const lite_navigation_t *nav)
{
    if (selected >= LITE_NAV_TAP_WAKE && selected <= LITE_NAV_TIMEOUT) {
        settings_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s) return;
        *s = *settings_get();
        if (selected == LITE_NAV_TAP_WAKE) s->tap_to_wake = !s->tap_to_wake;
        if (selected == LITE_NAV_TIME_FORMAT) s->time_24h = !s->time_24h;
        if (selected == LITE_NAV_VOLUME) s->volume = s->volume >= 100 ? 0 : s->volume + 20;
        if (selected == LITE_NAV_TIMEOUT) s->screen_timeout_s = s->screen_timeout_s < 15 ? 15 : s->screen_timeout_s < 30 ? 30 : s->screen_timeout_s < 60 ? 60 : 10;
        settings_save(s); free(s);
    }
    switch (selected)
    {
    case LITE_NAV_AOD:
        lite_settings.aod = !lite_settings.aod;
        lite_save();
        break;
    case LITE_NAV_WIFI:
        lite_web_set(!lite_web_running());
        break;
    case LITE_NAV_BRIGHTNESS:
        lite_settings.brightness = lite_settings.brightness >= 100 ? 20 : lite_settings.brightness + 20;
        lite_save();
        break;
    case LITE_NAV_EXIT:
        lite_return();
        break;
    case LITE_NAV_STOP: lite_alert_stop(false); break;
    case LITE_NAV_SNOOZE: lite_alert_stop(true); break;
    case LITE_NAV_PRIMARY: lite_content_activate(nav->page); break;
    default: break;
    }
}
void app_main(void)
{
    /* Do not erase existing user NVS on an initialization error. */
    ESP_ERROR_CHECK(nvs_flash_init());
    lite_runtime_init();
    ESP_ERROR_CHECK(extapp_sdk_init());
    ESP_ERROR_CHECK(board_init());
    storage_init();
    ESP_ERROR_CHECK(settings_init());
    clock_init();
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    alarm_service_init(lite_ring);
    life_services_init();
    ESP_ERROR_CHECK(gd_start());
    bool display_ok = lite_draw_init();
    if (display_ok)
    {
        display_set_brightness(lite_settings.brightness);
        if (board_info()->has_touch)
            touch_init(touch, NULL);
    }
    button_init(key);
    if (board_info()->has_knob)
        knob_init(knob);
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
    esp_pm_configure(&pm);
    esp_pm_lock_handle_t usb = NULL;
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "lite_usb", &usb);
    bool usb_held = false;
    bool idle = false, asleep = false, dirty = true, was_alert = false, touch_low = false,
         manual_sleep = false;
    lite_navigation_t navigation;
    lite_navigation_init(&navigation, board_info()->width, board_info()->height);
    int64_t active = esp_timer_get_time(), battery_at = 0, usb_retry_at = 0;
    int old_battery = -999;
    ESP_LOGI("lite", "Ready without LVGL; internal free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#ifdef LITE_QA
    lite_web_set(true);
    ESP_LOGI("lite", "QA_PAIR %s", lite_web_code());
#endif
    for (;;)
    {
        lite_event_t e = {0};
        /* Fast sampling only for the duration of a finger contact. Idle keeps
         * its slow blocking wait; gestures allocate no buffers or tasks. */
        bool event = xQueueReceive(lite_events, &e, pdMS_TO_TICKS(navigation.down ? 16 : 250)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        button_poll_pmu();
        bool power = pmu_usb_present();
        if (usb && power != usb_held)
        {
            if (power)
                esp_pm_lock_acquire(usb);
            else
                esp_pm_lock_release(usb);
            usb_held = power;
        }
        if (usb_held && !usb_console_connected() && now >= usb_retry_at) {
            usb_retry_at = now + 5000000;
            usb_console_reconnect();
        }
        if (event)
        {
#ifdef LITE_QA
            if (e.kind == LITE_QA_TOUCH) {
                simulated_touch = true; simulated_down = (e.second & 1) != 0;
                simulated_x = e.value; simulated_y = e.second >> 1;
                e.kind = LITE_TOUCH;
            }
#endif
            if (e.kind == LITE_EXIT)
                lite_return();
            if (e.kind == LITE_WEB)
                lite_web_set(e.value != 0);
            if (e.kind == LITE_WAKE)
            {
                active = now;
                idle = false;
                manual_sleep = false;
                dirty = true;
            }
            if (e.kind == LITE_BUTTON)
            {
                lite_action_t a = lite_navigation_key(&navigation, e.value == BUTTON_PWR,
                    e.second, now, idle, lite_ringing(), board_info()->has_knob);
                action(a, &navigation);
                active = now;
                idle = a == LITE_NAV_SLEEP || a == LITE_NAV_IDLE;
                manual_sleep = a == LITE_NAV_SLEEP;
                if (idle) navigation.down = false;
                dirty = true;
            }
            if (e.kind == LITE_KNOB)
            {
                active = now;
                idle = false;
                manual_sleep = false;
                lite_navigation_knob(&navigation, e.value);
                dirty = true;
            }
            if (e.kind == LITE_TOUCH)
            {
                uint16_t x = 0, y = 0;
                bool pressed = read_touch(&x, &y);
                if (idle && !settings_get()->tap_to_wake) {
#ifdef LITE_QA
                    simulated_touch = false;
#endif
                    touch_irq_arm();
                    continue;
                }
                if (!pressed)
                    touch_irq_arm();
                lite_action_t a = lite_navigation_touch(&navigation, pressed, x, y, now, idle, lite_ringing());
                action(a, &navigation);
                if (pressed || a != LITE_NAV_NONE) {
                    active = now;
                    idle = false;
                    manual_sleep = false;
                    dirty |= a != LITE_NAV_NONE;
                }
            }
            if (e.kind == LITE_DIRTY)
                dirty = true;
        }
        bool alert = lite_ringing();
        if (alert != was_alert)
        {
            dirty = true;
            was_alert = alert;
        }
        if (alert)
            active = now;
        if (navigation.down)
        {
            uint16_t x = 0, y = 0;
            bool pressed = read_touch(&x, &y);
            lite_action_t a = lite_navigation_touch(&navigation, pressed, x, y, now, idle, alert);
            action(a, &navigation);
            if (pressed || a != LITE_NAV_NONE)
            {
                active = now;
                idle = false;
                manual_sleep = false;
                dirty |= a != LITE_NAV_NONE;
            }
            if (!navigation.down)
            {
#ifdef LITE_QA
                simulated_touch = false;
#endif
                touch_irq_arm();
            }
        }
        if (!idle && now - active > (int64_t)settings_get()->screen_timeout_s * 1000000)
        {
            idle = true;
            navigation.page = LITE_CLOCK;
            dirty = true;
        }
        bool sleep = manual_sleep || (idle && !lite_settings.aod);
        if (display_ok && sleep != asleep)
        {
            display_set_brightness(0);
            display_sleep(sleep);
            if (board_info()->has_touch)
                touch_set_low_power(idle);
            asleep = sleep;
            dirty = true;
        }
        if (display_ok && board_info()->has_touch && touch_low != idle)
        {
            touch_low = idle;
            touch_set_low_power(idle);
        }
        if (now - battery_at > 120000000 || !battery_at)
        {
            int b = pmu_battery_percent();
            if (b != old_battery)
            {
                old_battery = b;
                dirty = true;
            }
            battery_at = now;
        }
        atomic_store(&ui_page, navigation.page);
        atomic_store(&ui_idle, idle);
        lite_web_tick();
        if (display_ok && dirty && !asleep)
        {
            char address[64];
            lite_web_address(address, sizeof(address));
            lite_draw(&navigation, idle, address, lite_web_code());
            uint8_t dim = board_info()->min_aod_brightness > 8 ? board_info()->min_aod_brightness : 8;
            display_set_brightness(idle ? dim : lite_settings.brightness);
            dirty = false;
        }
    }
}
