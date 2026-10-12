/* Keep the same alarm behavior as the base OS, including UI allocation retry. */
#include "orbit.h"
#include "apps_internal.h"
#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "services/alarm.h"
#include "services/sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>

static SemaphoreHandle_t s_mutex;
static alarm_t s_alarm;
static bool s_pending;

static void ring(const alarm_t *alarm)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_alarm = *alarm;
    s_pending = true;
    xSemaphoreGive(s_mutex);
    power_wake(POWER_WAKE_ALARM);
    sound_alarm_start();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    if (s_pending)
    {
        alarm_t *copy = malloc(sizeof(*copy));
        if (copy != NULL)
        {
            *copy = s_alarm;
            if (app_open_app(&ring_app, copy)) s_pending = false;
            else free(copy);
        }
    }
    xSemaphoreGive(s_mutex);
}

void orbit_alarm_prepare(void)
{
    s_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    sound_service_init();
}

void orbit_alarm_start(void)
{
    lv_port_lock();
    app_register(&ring_app);
    ESP_ERROR_CHECK(lv_timer_create(tick, 100, NULL) == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    lv_port_unlock();
    alarm_service_init(ring);
}
