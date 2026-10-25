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
#include "nvs_flash.h"
#include "services/storage.h"
#include <stdlib.h>
#include <string.h>

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
static void setting(int selected)
{
    switch (selected)
    {
    case 0:
        lite_settings.aod = !lite_settings.aod;
        lite_save();
        break;
    case 1:
        lite_web_set(!lite_web_running());
        break;
    case 2:
        lite_settings.brightness = lite_settings.brightness >= 100 ? 20 : lite_settings.brightness + 20;
        lite_save();
        break;
    case 3:
        lite_return();
        break;
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
    bool menu = false, idle = false, asleep = false, dirty = true, was_alert = false, touch_down = false,
         touch_low = false;
    int selected = 0;
    int64_t active = esp_timer_get_time(), battery_at = 0;
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
        bool event = xQueueReceive(lite_events, &e, pdMS_TO_TICKS(250)) == pdTRUE;
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
        if (usb_held && !usb_console_connected())
            usb_console_reconnect();
        if (event)
        {
            if (e.kind == LITE_EXIT)
                lite_return();
            if (e.kind == LITE_WEB)
                lite_web_set(e.value != 0);
            if (e.kind == LITE_WAKE)
            {
                active = now;
                idle = false;
                dirty = true;
            }
            if (e.kind == LITE_BUTTON)
            {
                active = now;
                idle = false;
                dirty = true;
                if (e.second == BUTTON_LONG_PRESS || e.second == BUTTON_RECOVERY)
                    lite_return();
                if (e.second == BUTTON_CLICK)
                {
                    if (lite_ringing())
                        lite_alert_stop(false);
                    else if (menu)
                        setting(selected);
                    else
                        menu = true;
                }
            }
            if (e.kind == LITE_KNOB)
            {
                active = now;
                idle = false;
                menu = true;
                selected = (selected + e.value % 4 + 4) % 4;
                dirty = true;
            }
            if (e.kind == LITE_TOUCH)
            {
                uint16_t x, y;
                bool pressed = touch_read(&x, &y);
                if (!pressed)
                    touch_irq_arm();
                if (pressed && !touch_down)
                {
                    touch_down = true;
                    if (lite_ringing())
                        lite_alert_stop(x < board_info()->width / 3);
                    else if (menu && !idle)
                    {
                        int i = ((int)y - ((int)board_info()->height / 2 - 85)) / 42;
                        if (i >= 0 && i < 4)
                        {
                            selected = i;
                            setting(i);
                        }
                    }
                    else
                        menu = true;
                    active = now;
                    idle = false;
                    dirty = true;
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
        if (touch_down)
        {
            uint16_t x, y;
            if (!touch_read(&x, &y))
            {
                touch_down = false;
                touch_irq_arm();
            }
        }
        if (!idle && now - active > 10000000)
        {
            idle = true;
            menu = false;
            dirty = true;
        }
        bool sleep = idle && !lite_settings.aod;
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
        lite_web_tick();
        if (display_ok && dirty && !asleep)
        {
            char address[64];
            lite_web_address(address, sizeof(address));
            lite_draw(menu, selected, idle, address, lite_web_code());
            display_set_brightness(idle ? 8 : lite_settings.brightness);
            dirty = false;
        }
    }
}
