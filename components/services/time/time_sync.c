#include "services/time_sync.h"

#include "core/clock.h"
#include "core/sys.h"
#include "core/settings.h"
#include "services/wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_sync";

#define RESYNC_WHEN_ONLINE_S  (6 * 3600)
#define DAILY_US              (24LL * 3600 * 1000 * 1000)

static volatile time_t s_last_sync = 0;
static volatile bool s_running = false;
static esp_timer_handle_t s_daily = NULL;

/* NTP on an already connected network. */
static esp_err_t run_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;

    if (esp_netif_sntp_init(&config) != ESP_OK)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    esp_netif_sntp_deinit();

    if (err == ESP_OK)
    {
        clock_set_utc(time(NULL));   /* also writes the RTC */
        s_last_sync = time(NULL);
        ESP_LOGI(TAG, "Synced with NTP");
    }
    else
    {
        ESP_LOGW(TAG, "NTP failed: %s", esp_err_to_name(err));
    }

    return err;
}

static void sync_task(void *arg)
{
    bool need_wifi = arg != NULL;

    if (!need_wifi || wifi_acquire(15000) == ESP_OK)
    {
        run_sntp();

        if (need_wifi)
        {
            wifi_release();
        }
    }

    s_running = false;
    vTaskDelete(NULL);
}

static void start_task(bool need_wifi)
{
    if (s_running || !settings_get()->auto_time)
    {
        return;
    }

    s_running = true;

    if (!sys_task_create(sync_task, "time_sync", 4096, need_wifi ? (void *)1 : NULL, 3, SYS_CORE_ANY))
    {
        s_running = false;
    }
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    if (s_last_sync == 0 || time(NULL) - s_last_sync > RESYNC_WHEN_ONLINE_S || !clock_is_valid())
    {
        start_task(false);
    }
}

static void daily_cb(void *arg)
{
    (void)arg;

    if (wifi_known_any() && (s_last_sync == 0 || time(NULL) - s_last_sync > 20 * 3600))
    {
        start_task(true);
    }
}

void time_sync_init(void)
{
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL);

    const esp_timer_create_args_t args = {.callback = daily_cb, .name = "ntp_daily"};
    esp_timer_create(&args, &s_daily);
    esp_timer_start_periodic(s_daily, DAILY_US);

    /* Clock never set (new board, RTC battery flat): try right away. */
    if (!clock_is_valid() && wifi_known_any())
    {
        start_task(true);
    }
}

esp_err_t time_sync_now(void)
{
    esp_err_t err = wifi_acquire(15000);

    if (err == ESP_OK)
    {
        err = run_sntp();
        wifi_release();
    }

    return err;
}

void time_sync_mark(void)
{
    s_last_sync = time(NULL);
}

time_t time_sync_last(void)
{
    return s_last_sync;
}
