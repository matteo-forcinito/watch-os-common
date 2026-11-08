/* Minimal Wi-Fi radio owner for the standalone Solco app. */
#include "services/wifi.h"
#include "../wifi/wifi_channel.h"

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/sys.h"

#include <stdbool.h>

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_retry_mutex_storage;
static SemaphoreHandle_t s_retry_mutex;
static portMUX_TYPE s_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_retry_init_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_retry_timer;
static unsigned s_references;
static bool s_owned;
static bool s_initialized;
static bool s_started;
static wifi_channel_policy_t s_channels;

static bool lock_owner(void)
{
    portENTER_CRITICAL(&s_mutex_init_mux);
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
    portEXIT_CRITICAL(&s_mutex_init_mux);
    return s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static bool lock_retry(void)
{
    portENTER_CRITICAL(&s_retry_init_mux);
    if (s_retry_mutex == NULL)
    {
        s_retry_mutex = xSemaphoreCreateMutexStatic(&s_retry_mutex_storage);
    }
    portEXIT_CRITICAL(&s_retry_init_mux);
    return s_retry_mutex != NULL && xSemaphoreTake(s_retry_mutex, 0) == pdTRUE;
}

static void retry_timer(void *arg);
static void retry_work(void *arg);

static void schedule_retry(void)
{
    if (!lock_retry())
    {
        return;
    }
    if (s_retry_timer == NULL)
    {
        const esp_timer_create_args_t args = {
            .callback = retry_timer,
            .name = "solco_radio_retry",
        };
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK)
        {
            xSemaphoreGive(s_retry_mutex);
            return;
        }
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, 100000);
    xSemaphoreGive(s_retry_mutex);
}

static esp_err_t teardown_driver(void)
{
    if (s_started)
    {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK)
        {
            return err;
        }
        s_started = false;
    }
    if (s_initialized)
    {
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK)
        {
            return err;
        }
        s_initialized = false;
    }
    s_owned = false;
    return ESP_OK;
}

static void retry_timer(void *arg)
{
    (void)arg;
    if (!sys_post(retry_work, NULL))
    {
        schedule_retry();
    }
}

static void retry_work(void *arg)
{
    (void)arg;
    if (!lock_owner())
    {
        schedule_retry();
        return;
    }
    esp_err_t err = ESP_OK;
    if (s_references == 0 && s_owned)
    {
        err = teardown_driver();
    }
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK)
    {
        schedule_retry();
    }
}

esp_err_t wifi_radio_acquire(void)
{
    if (!lock_owner())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_references != 0)
    {
        ++s_references;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (s_owned)
    {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE)
    {
        err = ESP_OK;
    }
    if (err == ESP_OK)
    {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = 0;
        err = esp_wifi_init(&config);
        if (err == ESP_OK)
        {
            s_initialized = true;
            s_owned = true;
        }
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
        if (err == ESP_OK)
        {
            s_started = true;
        }
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err == ESP_OK)
    {
        s_references = 1;
    }
    else if (s_owned)
    {
        esp_err_t cleanup = teardown_driver();
        if (cleanup != ESP_OK)
        {
            schedule_retry();
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void wifi_radio_release(void)
{
    if (!lock_owner())
    {
        return;
    }
    if (s_references > 0)
    {
        --s_references;
    }
    esp_err_t err = ESP_OK;
    if (s_references == 0 && s_owned)
    {
        err = teardown_driver();
    }
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK)
    {
        schedule_retry();
    }
}

bool wifi_radio_set_channel(uint8_t channel)
{
    if (channel < 1 || channel > 13 || !lock_owner())
    {
        return false;
    }
    uint8_t current = 0;
    wifi_second_chan_t secondary;
    bool ok = s_references > 0 && s_started &&
              esp_wifi_get_channel(&current, &secondary) == ESP_OK &&
              wifi_channel_allowed(&s_channels, channel, false, current) &&
              (current == channel || esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK);
    xSemaphoreGive(s_mutex);
    return ok;
}

bool wifi_radio_channel_reserve(const char owner[4], uint8_t channel)
{
    if (!lock_owner()) return false;
    uint8_t current = 0;
    wifi_second_chan_t secondary;
    wifi_channel_policy_t before = s_channels;
    bool ok = s_references > 0 && s_started &&
              esp_wifi_get_channel(&current, &secondary) == ESP_OK &&
              wifi_channel_reserve(&s_channels, (const uint8_t *)owner, channel, false, current);
    if (ok && current != channel)
    {
        ok = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
        if (!ok) s_channels = before;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void wifi_radio_channel_release(const char owner[4])
{
    if (!lock_owner()) return;
    wifi_channel_release(&s_channels, (const uint8_t *)owner);
    xSemaphoreGive(s_mutex);
}

uint8_t wifi_radio_pinned_channel(void)
{
    if (!lock_owner()) return 0;
    uint8_t channel = wifi_channel_pinned(&s_channels);
    xSemaphoreGive(s_mutex);
    return channel;
}

uint8_t wifi_radio_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t secondary;
    if (!lock_owner())
    {
        return 0;
    }
    if (s_started && esp_wifi_get_channel(&primary, &secondary) == ESP_OK)
    {
        xSemaphoreGive(s_mutex);
        return primary;
    }
    xSemaphoreGive(s_mutex);
    return 0;
}

uint8_t wifi_home_channel(void)
{
    return 0;
}
