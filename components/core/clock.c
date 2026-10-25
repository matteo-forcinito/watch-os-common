#include "core/clock.h"

#include "core/settings.h"
#include "core/state.h"

#include "hardware/rtc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "clock";

/* Anything before this is "the RTC was never set". */
#define VALID_AFTER 1735689600   /* 2025-01-01 */

static esp_timer_handle_t s_tick = NULL;
static void (*s_listeners[4])(void);
static unsigned s_listener_count;
typedef struct { uint32_t magic, reserved; uint64_t rtc_us; int64_t utc_us; } clock_keep_t;
void clock_checkpoint(void)
{
    struct timeval tv;gettimeofday(&tv,NULL);
    if(tv.tv_sec <= VALID_AFTER) return;
    clock_keep_t keep={.magic=0x434C4B31,.rtc_us=esp_rtc_get_time_us(),.utc_us=(int64_t)tv.tv_sec*1000000+tv.tv_usec};
    nvs_handle_t h;
    if(nvs_open("clock_keep",NVS_READWRITE,&h)==ESP_OK) {
        esp_err_t err=nvs_set_blob(h,"v1",&keep,sizeof(keep));
        if(err==ESP_OK)err=nvs_commit(h);
        if(err!=ESP_OK)ESP_LOGW(TAG,"Clock handover checkpoint: %s",esp_err_to_name(err));
        nvs_close(h);
    }
}
static bool restore_checkpoint(void)
{
    /* RTC ticks cannot establish elapsed wall time after power loss. */
    if(esp_reset_reason()!=ESP_RST_SW) return false;
    nvs_handle_t h;clock_keep_t keep={0};size_t size=sizeof(keep);
    if(nvs_open("clock_keep",NVS_READONLY,&h)!=ESP_OK)return false;
    esp_err_t err=nvs_get_blob(h,"v1",&keep,&size);nvs_close(h);
    uint64_t now=esp_rtc_get_time_us();
    if(err!=ESP_OK||size!=sizeof(keep)||keep.magic!=0x434C4B31||now<keep.rtc_us||now-keep.rtc_us>7ULL*86400*1000000||keep.utc_us<(int64_t)VALID_AFTER*1000000)return false;
    int64_t utc=keep.utc_us+(int64_t)(now-keep.rtc_us);
    struct timeval tv={.tv_sec=(time_t)(utc/1000000),.tv_usec=(suseconds_t)(utc%1000000)};
    settimeofday(&tv,NULL);return true;
}
void clock_add_listener(void (*changed)(void))
{
    if(changed && s_listener_count < 4) s_listeners[s_listener_count++] = changed;
}
static void changed(void)
{
    for(unsigned i=0;i<s_listener_count;i++)s_listeners[i]();
}

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
    else if (restore_checkpoint())
    {
        ESP_LOGI(TAG,"Time retained across software handover");
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
    clock_checkpoint();

    publish_minute();
    schedule_next_minute();
    changed();
}

void clock_set_timezone(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();

    if (s_tick != NULL)
    {
        publish_minute();
    }
    changed();
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
