#include "services/alarm.h"

#include "core/state.h"
#include "core/sys.h"
#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "alarm";

#define NVS_NAMESPACE "alarms"
#define NVS_KEY       "list"
#define NVS_IMPORTED  "imported"
#define MAX_WAIT_US   (60LL * 1000 * 1000)

static alarm_t s_alarms[ALARM_MAX];
static int s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static StaticSemaphore_t s_mutex_buffer;
static esp_timer_handle_t s_timer = NULL;
static alarm_ring_cb_t s_on_ring = NULL;

/* Snooze: one pending re-ring. */
static uint32_t s_snooze_id = 0;
static time_t s_snooze_at = 0;
static time_t s_last_fired_minute = 0;

/*
 * The lock creates itself on first use: the UI may ask for the next alarm
 * before alarm_service_init() ran, and that must not crash.
 */
static void lock(void)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/* ------------------------------------------------------------ storage */

static void save(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, NVS_KEY, s_alarms, sizeof(alarm_t) * (size_t)s_count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    state_bump(STATE_ALARM_VERSION);
}

static uint32_t new_id(void)
{
    uint32_t max = 0;

    for (int i = 0; i < s_count; i++)
    {
        max = s_alarms[i].id > max ? s_alarms[i].id : max;
    }

    return max + 1;
}

/*
 * Arduino launcher format:
 * [{"id":1,"hour":7,"minute":30,"recurrence":3,"interval":1,"title":"...","enabled":true}]
 * recurrence 0 = once, 3 = days (every `interval` days, imported as every day), 4 = weeks.
 */
static void import_arduino(void)
{
    FILE *file = fopen(SDCARD_MOUNT "/alarms.json", "r");

    if (file == NULL)
    {
        return;
    }

    char *text = calloc(1, 8192);

    if (text != NULL)
    {
        fread(text, 1, 8191, file);
        cJSON *root = cJSON_Parse(text);
        cJSON *item = NULL;

        cJSON_ArrayForEach(item, root)
        {
            if (s_count >= ALARM_MAX)
            {
                break;
            }

            alarm_t a = {0};
            a.id = new_id();
            a.hour = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "hour"));
            a.minute = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "minute"));
            int recurrence = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "recurrence"));
            a.days = recurrence == 0 ? ALARM_DAYS_ONCE : ALARM_DAYS_EVERYDAY;
            a.enabled = !cJSON_IsFalse(cJSON_GetObjectItem(item, "enabled"));
            a.radio = -1;
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
            snprintf(a.label, sizeof(a.label), "%s", title != NULL ? title : "Sveglia");

            if (a.hour < 24 && a.minute < 60)
            {
                s_alarms[s_count++] = a;
            }
        }

        cJSON_Delete(root);
        free(text);
    }

    fclose(file);
    ESP_LOGI(TAG, "Imported %d alarms from the Arduino launcher", s_count);
}

static void load(void)
{
    nvs_handle_t nvs;
    s_count = 0;
    uint8_t imported = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(s_alarms);

        if (nvs_get_blob(nvs, NVS_KEY, s_alarms, &size) == ESP_OK)
        {
            s_count = (int)(size / sizeof(alarm_t));
        }

        nvs_get_u8(nvs, NVS_IMPORTED, &imported);
        nvs_close(nvs);
    }

    if (!imported && s_count == 0 && sdcard_is_mounted())
    {
        import_arduino();

        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
        {
            nvs_set_u8(nvs, NVS_IMPORTED, 1);
            nvs_commit(nvs);
            nvs_close(nvs);
        }

        if (s_count > 0)
        {
            save();
        }
    }
}

/* ----------------------------------------------------------- schedule */

/* Next time (local) `a` rings strictly after `now`. 0 if disabled. */
static time_t next_ring(const alarm_t *a, time_t now)
{
    if (!a->enabled)
    {
        return 0;
    }

    struct tm t;
    localtime_r(&now, &t);
    t.tm_sec = 0;

    for (int day = 0; day < 8; day++)
    {
        struct tm c = t;
        c.tm_mday += day;
        c.tm_hour = a->hour;
        c.tm_min = a->minute;
        c.tm_isdst = -1;
        time_t when = mktime(&c);

        if (when <= now)
        {
            continue;
        }

        int monday_based = (c.tm_wday + 6) % 7;

        if (a->days == ALARM_DAYS_ONCE || (a->days & (1 << monday_based)))
        {
            return when;
        }
    }

    return 0;
}

static void reschedule(void);
static void timer_cb(void *arg);

static void check_job(void *arg)
{
    (void)arg;

    time_t now = time(NULL);
    time_t minute = now / 60;
    alarm_t ringing = {0};
    bool ring = false;

    lock();

    if (minute != s_last_fired_minute)
    {
        if (s_snooze_id != 0 && now >= s_snooze_at)
        {
            for (int i = 0; i < s_count; i++)
            {
                if (s_alarms[i].id == s_snooze_id)
                {
                    ringing = s_alarms[i];
                    ring = true;
                }
            }

            s_snooze_id = 0;
        }

        struct tm t;
        localtime_r(&now, &t);

        for (int i = 0; i < s_count && !ring; i++)
        {
            alarm_t *a = &s_alarms[i];
            int monday_based = (t.tm_wday + 6) % 7;

            if (a->enabled && a->hour == t.tm_hour && a->minute == t.tm_min &&
                (a->days == ALARM_DAYS_ONCE || (a->days & (1 << monday_based))))
            {
                ringing = *a;
                ring = true;

                if (a->days == ALARM_DAYS_ONCE)
                {
                    a->enabled = false;   /* one-shot alarms switch themselves off */
                    save();
                }
            }
        }

        if (ring)
        {
            s_last_fired_minute = minute;
        }
    }

    unlock();

    if (ring)
    {
        ESP_LOGI(TAG, "Ringing: %02u:%02u %s", ringing.hour, ringing.minute, ringing.label);

        if (s_on_ring != NULL)
        {
            s_on_ring(&ringing);
        }
    }

    reschedule();
}

/* esp_timer task: the check (NVS writes, time math, ring callback) runs in the sys worker. */
static void timer_cb(void *arg)
{
    (void)arg;
    sys_post(check_job, NULL);
}

static void reschedule(void)
{
    time_t now = time(NULL);
    time_t next = 0;

    lock();

    for (int i = 0; i < s_count; i++)
    {
        time_t when = next_ring(&s_alarms[i], now);

        if (when != 0 && (next == 0 || when < next))
        {
            next = when;
        }
    }

    if (s_snooze_id != 0 && (next == 0 || s_snooze_at < next))
    {
        next = s_snooze_at;
    }

    unlock();

    int64_t wait_us = next != 0 ? (int64_t)(next - now) * 1000000 + 200000 : MAX_WAIT_US;

    if (wait_us > MAX_WAIT_US)
    {
        wait_us = MAX_WAIT_US;   /* re-check every minute: survives clock changes */
    }

    if (wait_us < 100000)
    {
        wait_us = 100000;
    }

    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)wait_us);
}

/* -------------------------------------------------------------- public */

void alarm_service_init(alarm_ring_cb_t on_ring)
{
    s_on_ring = on_ring;
    load();

    const esp_timer_create_args_t args = {.callback = timer_cb, .name = "alarm"};
    esp_timer_create(&args, &s_timer);

    reschedule();
    ESP_LOGI(TAG, "%d alarms", s_count);
}

int alarm_list(alarm_t *out, int max)
{
    lock();
    int count = s_count < max ? s_count : max;
    memcpy(out, s_alarms, sizeof(alarm_t) * (size_t)count);
    unlock();

    return count;
}

bool alarm_get(uint32_t id, alarm_t *out)
{
    bool found = false;

    lock();

    for (int i = 0; i < s_count && !found; i++)
    {
        if (s_alarms[i].id == id)
        {
            *out = s_alarms[i];
            found = true;
        }
    }

    unlock();

    return found;
}

esp_err_t alarm_save(alarm_t *alarm)
{
    if (alarm->hour > 23 || alarm->minute > 59)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    lock();

    int index = -1;

    for (int i = 0; i < s_count; i++)
    {
        if (alarm->id != 0 && s_alarms[i].id == alarm->id)
        {
            index = i;
        }
    }

    if (index < 0)
    {
        if (s_count >= ALARM_MAX)
        {
            err = ESP_ERR_NO_MEM;
        }
        else
        {
            alarm->id = new_id();
            index = s_count++;
        }
    }

    if (err == ESP_OK)
    {
        alarm->label[ALARM_LABEL_MAX - 1] = '\0';
        s_alarms[index] = *alarm;
        save();
    }

    unlock();

    reschedule();

    return err;
}

esp_err_t alarm_delete(uint32_t id)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    lock();

    for (int i = 0; i < s_count; i++)
    {
        if (s_alarms[i].id == id)
        {
            memmove(&s_alarms[i], &s_alarms[i + 1], sizeof(alarm_t) * (size_t)(s_count - i - 1));
            s_count--;
            save();
            err = ESP_OK;
            break;
        }
    }

    unlock();

    reschedule();

    return err;
}

esp_err_t alarm_set_enabled(uint32_t id, bool enabled)
{
    alarm_t a;

    if (!alarm_get(id, &a))
    {
        return ESP_ERR_NOT_FOUND;
    }

    a.enabled = enabled;

    return alarm_save(&a);
}

bool alarm_next(alarm_t *out, time_t *when)
{
    time_t now = time(NULL);
    time_t best = 0;

    lock();

    for (int i = 0; i < s_count; i++)
    {
        time_t t = next_ring(&s_alarms[i], now);

        if (t != 0 && (best == 0 || t < best))
        {
            best = t;

            if (out != NULL)
            {
                *out = s_alarms[i];
            }
        }
    }

    unlock();

    if (when != NULL)
    {
        *when = best;
    }

    return best != 0;
}

void alarm_snooze(uint32_t id, int minutes)
{
    lock();
    s_snooze_id = id;
    s_snooze_at = time(NULL) + minutes * 60;
    unlock();

    reschedule();
}

void alarm_dismiss(uint32_t id)
{
    lock();

    if (s_snooze_id == id)
    {
        s_snooze_id = 0;
    }

    unlock();

    state_bump(STATE_ALARM_VERSION);
}

void alarm_days_text(uint8_t days, char *out, size_t size)
{
    static const char *const names[] = {"Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom"};

    if (days == ALARM_DAYS_ONCE)
    {
        snprintf(out, size, "Una volta");
        return;
    }

    if (days == ALARM_DAYS_EVERYDAY)
    {
        snprintf(out, size, "Ogni giorno");
        return;
    }

    if (days == ALARM_DAYS_WEEKDAYS)
    {
        snprintf(out, size, "Lun - Ven");
        return;
    }

    if (days == 0x60)
    {
        snprintf(out, size, "Weekend");
        return;
    }

    size_t used = 0;
    out[0] = '\0';

    for (int i = 0; i < 7 && used + 5 < size; i++)
    {
        if (days & (1 << i))
        {
            used += (size_t)snprintf(out + used, size - used, "%s%s", used > 0 ? " " : "", names[i]);
        }
    }
}
