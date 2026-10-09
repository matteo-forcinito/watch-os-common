#include "core/clock.h"

#include "core/settings.h"
#include "core/state.h"

#include "hardware/rtc.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "clock";

/* Anything before this is "the RTC was never set". */
#define VALID_AFTER 1735689600   /* 2025-01-01 */

static esp_timer_handle_t s_tick = NULL;

static void schedule_next_minute(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int64_t us_into_minute = (int64_t)(tv.tv_sec % 60) * 1000000 + tv.tv_usec;
    int64_t wait_us = 60 * 1000000LL - us_into_minute + 20000;   /* +20 ms: land safely in the new minute */

    esp_timer_stop(s_tick);
    esp_timer_start_once(s_tick, (uint64_t)wait_us);
}

static void publish_minute(void)
{
    state_set(STATE_MINUTE, (int32_t)(time(NULL) / 60));
}

static void tick_cb(void *arg)
{
    (void)arg;
    publish_minute();
    schedule_next_minute();
}

void clock_init(void)
{
    clock_set_timezone(settings_get()->timezone);

    time_t rtc_time = 0;

    if (rtc_chip_get_time(&rtc_time) == ESP_OK && rtc_time > VALID_AFTER)
    {
        struct timeval tv = {.tv_sec = rtc_time, .tv_usec = 0};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Time from RTC");
    }
    else
    {
        ESP_LOGW(TAG, "RTC not set: waiting for NTP / phone");
    }

    const esp_timer_create_args_t args = {.callback = tick_cb, .name = "minute"};
    esp_timer_create(&args, &s_tick);

    publish_minute();
    schedule_next_minute();
}

void clock_set_utc(time_t utc)
{
    struct timeval tv = {.tv_sec = utc, .tv_usec = 0};
    settimeofday(&tv, NULL);
    rtc_chip_set_time(utc);

    publish_minute();
    schedule_next_minute();
}

void clock_set_timezone(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();

    if (s_tick != NULL)
    {
        publish_minute();
    }
}

void clock_local(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
}

bool clock_is_valid(void)
{
    return time(NULL) > VALID_AFTER;
}

void clock_format_hm(const struct tm *t, char *out, size_t size)
{
    if (settings_get()->time_24h)
    {
        snprintf(out, size, "%02d:%02d", t->tm_hour, t->tm_min);
    }
    else
    {
        int hour = t->tm_hour % 12;
        snprintf(out, size, "%d:%02d", hour == 0 ? 12 : hour, t->tm_min);
    }
}

const char *clock_weekday_name(int wday, bool short_name)
{
    static const char *const names[] = {"Domenica", "Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato"};
    static const char *const shorts[] = {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"};

    if (wday < 0 || wday > 6)
    {
        return "";
    }

    return short_name ? shorts[wday] : names[wday];
}

const char *clock_month_name(int mon, bool short_name)
{
    static const char *const names[] = {"Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno",
                                        "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"};
    static const char *const shorts[] = {"GEN", "FEB", "MAR", "APR", "MAG", "GIU",
                                         "LUG", "AGO", "SET", "OTT", "NOV", "DIC"};

    if (mon < 0 || mon > 11)
    {
        return "";
    }

    return short_name ? shorts[mon] : names[mon];
}
