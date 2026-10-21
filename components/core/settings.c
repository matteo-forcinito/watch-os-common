#include "core/settings.h"

#include "core/state.h"
#include "core/sys.h"

#include "hardware/board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";

#define NVS_NAMESPACE  "system"
#define NVS_KEY        "settings"
#define SAVE_DELAY_US  (2 * 1000 * 1000)

static settings_t s_settings;
static esp_timer_handle_t s_save_timer = NULL;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_dirty = false;
static settings_listener_t s_listeners[6];
static int s_listener_count = 0;

static void set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 180;
    s->screen_timeout_s = 15;
    s->always_on = false;
    s->tap_to_wake = true;
    s->wake_on_notification = true;
    s->volume = 60;
    s->key_sounds = false;
    s->time_24h = true;
    strcpy(s->timezone, "CET-1CEST,M3.5.0,M10.5.0/3");   /* Italy */
    s->auto_time = true;
    s->ble_enabled = true;
    /* "Bangle.js" prefix: Gadgetbridge pairs it. Then the watch's own name. */
    snprintf(s->device_name, sizeof(s->device_name), "Bangle.js %s", sys_nickname());
    strcpy(s->weather_city, "Civita Castellana");
    s->weather_lat = 42.2946f;
    s->weather_lon = 12.4103f;
    s->radio_station = -1;
    s->watchface = 0;
    s->accent_color = 0x3D8BFF;
    s->knob_focus = false;
    s->update_channel = 0;
    s->border_progress = false;
    s->launcher_cards = 4;
    memset(s->favorites, 0, sizeof(s->favorites));
}

static void sanitize(settings_t *s)
{
    /* Without touch the paged face cannot be navigated: the app list. */
    if (s->home_preferences_version == 1 && s->homepage <= 1)
    {
        if (!board_info()->has_touch && s->homepage == 0) s->homepage = 2;
        s->home_preferences_version = 2;
    }
    if (s->home_preferences_version != 2 || s->homepage > 2)
    {
        s->home_preferences_version = 2;
        s->homepage = board_info()->has_touch ? 0 : 2;
    }
    if (s->remote_preferences_version != 1)
    {
        s->remote_ui_enabled = false;
        s->remote_preferences_version = 1;
    }
    if (s->ecos_preferences_version != 1 || s->ecos_mode > 2) {
        s->ecos_mode = 0; s->ecos_preferences_version = 1;
    }
    if (s->brightness < 10)
    {
        s->brightness = 10;
    }

    if (s->screen_timeout_s < 5 || s->screen_timeout_s > 120)
    {
        s->screen_timeout_s = 15;
    }

    if (s->volume > 100)
    {
        s->volume = 100;
    }

    s->timezone[sizeof(s->timezone) - 1] = '\0';
    s->device_name[sizeof(s->device_name) - 1] = '\0';
    /* The old default ("Bangle.js AMOLED", same on every watch) becomes this watch's own name. */
    char old_default[sizeof(s->device_name)];
    snprintf(old_default, sizeof(old_default), "Bangle.js %s", board_info()->short_name);
    if (strcmp(s->device_name, old_default) == 0)
    {
        snprintf(s->device_name, sizeof(s->device_name), "Bangle.js %s", sys_nickname());
    }
    s->weather_city[sizeof(s->weather_city) - 1] = '\0';
    s->update_url[sizeof(s->update_url) - 1] = '\0';
    if (s->launcher_cards != 4 && s->launcher_cards != 6 && s->launcher_cards != 9)
    {
        s->launcher_cards = 4;
    }
    for (int i = 0; i < 6; i++)
    {
        s->favorites[i][sizeof(s->favorites[i]) - 1] = '\0';
    }

    if (s->timezone[0] == '\0')
    {
        strcpy(s->timezone, "UTC0");
    }

    if (s->watchface > 2)
    {
        s->watchface = 0;
    }
}

static void write_now(void)
{
    settings_t copy;

    portENTER_CRITICAL(&s_lock);
    copy = s_settings;
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);

    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return;
    }

    esp_err_t err = nvs_set_blob(nvs, NVS_KEY, &copy, sizeof(copy));

    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved (%s)", esp_err_to_name(err));
}

static void write_job(void *arg)
{
    (void)arg;
    write_now();
}

/* esp_timer task: the flash write happens in the sys worker. */
static void save_timer_cb(void *arg)
{
    (void)arg;
    sys_post(write_job, NULL);
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS full or old format: erasing");
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        return err;
    }

    set_defaults(&s_settings);

    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = 0;

        /* An older, shorter blob fills the start; newer fields keep defaults. */
        if (nvs_get_blob(nvs, NVS_KEY, NULL, &size) == ESP_OK && size > 0)
        {
            if (size > sizeof(s_settings))
            {
                size = sizeof(s_settings);
            }

            nvs_get_blob(nvs, NVS_KEY, &s_settings, &size);
        }

        nvs_close(nvs);
    }

    sanitize(&s_settings);

    const esp_timer_create_args_t args = {.callback = save_timer_cb, .name = "settings"};
    esp_timer_create(&args, &s_save_timer);

    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_settings;
}

void settings_save(const settings_t *settings)
{
    settings_t copy = *settings;
    sanitize(&copy);

    portENTER_CRITICAL(&s_lock);
    s_settings = copy;
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);

    if (s_save_timer != NULL)
    {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
    }

    state_bump(STATE_SETTINGS_VERSION);

    for (int i = 0; i < s_listener_count; i++)
    {
        s_listeners[i](&copy);
    }
}

void settings_add_listener(settings_listener_t listener)
{
    if (s_listener_count < (int)(sizeof(s_listeners) / sizeof(s_listeners[0])))
    {
        s_listeners[s_listener_count++] = listener;
    }
}

void settings_flush(void)
{
    if (s_dirty)
    {
        esp_timer_stop(s_save_timer);
        write_now();
    }
}

void settings_reset(void)
{
    settings_t defaults;
    set_defaults(&defaults);
    settings_save(&defaults);
    settings_flush();
}
