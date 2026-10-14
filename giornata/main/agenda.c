/*
 * Giornata's clockwork (see giornata.h): every 2 seconds, in the sys worker,
 *   - the focus timer: a finished session is credited to its task, logged
 *     (focus.csv: day,minutes,task) and followed by a break;
 *   - reminders: N minutes before a timed event, at a task's due time, at the
 *     end of a session or a break: a card on the watch and a chime (not in
 *     the quiet hours);
 *   - a new day rebuilds the agenda; changes are saved.
 * The esp_timer callback only posts the job (rules of the project).
 */
#include "giornata.h"
#include "internal.h"

#include "core/power.h"
#include "core/sys.h"
#include "services/sound.h"
#include "services/storage.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_US     (2 * 1000 * 1000)
#define SEEN_MAX    48
#define FOCUS_DAYS  120

static esp_timer_handle_t s_timer;
static gd_focus_t s_focus;
static gd_alert_t s_alert;
static uint32_t s_seen[SEEN_MAX];     /* reminders already given (hash of start + title) */
static int s_seen_next;
static int32_t s_day = -1;
static int s_sessions_in_row;
static uint16_t *s_focus_days;        /* minutes per day, last FOCUS_DAYS days, index FOCUS_DAYS-1 = today */
static int32_t s_focus_days_end;      /* day number of the last slot */

/* --------------------------------------------------------------- helpers */

static uint32_t hash(ics_time_t start, const char *title)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++)
    {
        h = (h ^ (uint8_t)(start >> (i * 8))) * 16777619u;
    }
    for (; *title != '\0'; title++)
    {
        h = (h ^ (uint8_t)*title) * 16777619u;
    }
    return h | 1;
}

static bool seen(uint32_t key)
{
    for (int i = 0; i < SEEN_MAX; i++)
    {
        if (s_seen[i] == key)
        {
            return true;
        }
    }
    s_seen[s_seen_next] = key;
    s_seen_next = (s_seen_next + 1) % SEEN_MAX;
    return false;
}

static bool quiet_now(const gd_settings_t *s, ics_time_t now)
{
    if (s->quiet_from == s->quiet_to)
    {
        return false;
    }
    int hour = (int)((now % 86400) / 3600);
    return s->quiet_from < s->quiet_to ? hour >= s->quiet_from && hour < s->quiet_to
                                       : hour >= s->quiet_from || hour < s->quiet_to;
}

/* Locked. */
static void raise_alert(const char *title, const char *detail, uint32_t color, bool chime)
{
    s_alert.active = true;
    gd_copy(s_alert.title, sizeof(s_alert.title), title);
    snprintf(s_alert.detail, sizeof(s_alert.detail), "%s", detail);
    s_alert.color = color;
    s_alert.serial++;
    gd_changed(false);
    gd_settings_t s = gd_settings_unlocked();
    ics_time_t now = gd_now();
    power_wake(POWER_WAKE_NOTIFICATION);
    if (chime && s.sound && !quiet_now(&s, now))
    {
        sound_notification();
    }
}

void gd_alert(gd_alert_t *out)
{
    gd_lock();
    *out = s_alert;
    gd_unlock();
}

void gd_alert_dismiss(void)
{
    gd_lock();
    s_alert.active = false;
    gd_changed(false);
    gd_unlock();
}

/* ------------------------------------------------------------- focus log */

static void focus_days_shift(int32_t today)
{
    if (s_focus_days_end == 0)
    {
        s_focus_days_end = today;
    }
    int32_t shift = today - s_focus_days_end;
    if (shift <= 0)
    {
        return;
    }
    if (shift >= FOCUS_DAYS)
    {
        memset(s_focus_days, 0, sizeof(uint16_t) * FOCUS_DAYS);
    }
    else
    {
        memmove(s_focus_days, s_focus_days + shift, sizeof(uint16_t) * (FOCUS_DAYS - shift));
        memset(s_focus_days + FOCUS_DAYS - shift, 0, sizeof(uint16_t) * shift);
    }
    s_focus_days_end = today;
}

static void focus_log(int32_t day, int minutes, uint32_t task)
{
    char path[80];
    if (!gd_path("focus.csv", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "a");
    if (file != NULL)
    {
        fprintf(file, "%ld,%d,%lu\n", (long)day, minutes, (unsigned long)task);
        fclose(file);
    }
    storage_files_release();
}

static void focus_load(void)
{
    char path[80];
    int32_t today = (int32_t)(gd_now() / 86400);
    if (today <= 0 || !gd_path("focus.csv", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    s_focus_days_end = today;
    FILE *file = fopen(path, "r");
    char line[48];
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
    {
        long day = 0;
        int minutes = 0;
        unsigned long task = 0;
        if (sscanf(line, "%ld,%d,%lu", &day, &minutes, &task) >= 2 && day <= today && day > today - FOCUS_DAYS)
        {
            int index = FOCUS_DAYS - 1 - (int)(today - day);
            uint32_t sum = s_focus_days[index] + (uint32_t)minutes;
            s_focus_days[index] = sum > 65535 ? 65535 : (uint16_t)sum;
        }
    }
    if (file != NULL)
    {
        fclose(file);
    }
    storage_files_release();
}

int gd_focus_days(int days, uint16_t *minutes)
{
    days = days > FOCUS_DAYS ? FOCUS_DAYS : days;
    gd_lock();
    int32_t today = (int32_t)(gd_now() / 86400);
    if (today > 0)
    {
        focus_days_shift(today);
    }
    memcpy(minutes, s_focus_days + FOCUS_DAYS - days, sizeof(uint16_t) * days);
    gd_unlock();
    return days;
}

/* ----------------------------------------------------------------- focus */

void gd_focus(gd_focus_t *out)
{
    gd_lock();
    *out = s_focus;
    if (out->state == GD_FOCUS_RUNNING || out->state == GD_FOCUS_BREAK)
    {
        int64_t left = (s_focus.ends_at - esp_timer_get_time()) / 1000000;
        out->remaining_s = (int32_t)(left > 0 ? left : 0);
    }
    out->done_today = s_focus_days != NULL ? s_focus_days[FOCUS_DAYS - 1] / (gd_settings_unlocked().focus_minutes ? gd_settings_unlocked().focus_minutes : 25) : 0;
    gd_unlock();
}

void gd_focus_start(uint32_t task)
{
    gd_lock();
    gd_settings_t s = gd_settings_unlocked();
    if (s_focus.state == GD_FOCUS_PAUSED && (task == 0 || task == s_focus.task))
    {
        s_focus.state = GD_FOCUS_RUNNING;
        s_focus.ends_at = esp_timer_get_time() + (int64_t)s_focus.remaining_s * 1000000;
    }
    else
    {
        memset(&s_focus, 0, sizeof(s_focus));
        s_focus.state = GD_FOCUS_RUNNING;
        s_focus.task = task;
        s_focus.length_s = s.focus_minutes * 60;
        s_focus.remaining_s = s_focus.length_s;
        s_focus.ends_at = esp_timer_get_time() + (int64_t)s_focus.length_s * 1000000;
        int count = 0;
        const gd_task_t *tasks = gd_tasks(&count);
        for (int i = 0; i < count && task != 0; i++)
        {
            if (tasks[i].id == task)
            {
                gd_copy(s_focus.title, sizeof(s_focus.title), tasks[i].title);
            }
        }
    }
    gd_changed(false);
    gd_unlock();
}

void gd_focus_pause(void)
{
    gd_lock();
    if (s_focus.state == GD_FOCUS_RUNNING)
    {
        int64_t left = (s_focus.ends_at - esp_timer_get_time()) / 1000000;
        s_focus.remaining_s = (int32_t)(left > 0 ? left : 0);
        s_focus.state = GD_FOCUS_PAUSED;
        gd_changed(false);
    }
    gd_unlock();
}

void gd_focus_stop(void)
{
    gd_lock();
    /* Minutes already spent still count (a session cut short is real work). */
    if (s_focus.state == GD_FOCUS_RUNNING || s_focus.state == GD_FOCUS_PAUSED)
    {
        int32_t left = s_focus.state == GD_FOCUS_RUNNING ? (int32_t)((s_focus.ends_at - esp_timer_get_time()) / 1000000)
                                                         : s_focus.remaining_s;
        int minutes = (s_focus.length_s - (left > 0 ? left : 0)) / 60;
        int32_t today = (int32_t)(gd_now() / 86400);
        if (minutes >= 1 && today > 0)
        {
            focus_days_shift(today);
            s_focus_days[FOCUS_DAYS - 1] += (uint16_t)minutes;
            if (s_focus.task != 0)
            {
                gd_task_add_minutes(s_focus.task, minutes);   /* minutes only: not a full session */
            }
            gd_unlock();
            focus_log(today, minutes, s_focus.task);
            gd_lock();
        }
    }
    memset(&s_focus, 0, sizeof(s_focus));
    s_sessions_in_row = 0;
    gd_changed(false);
    gd_unlock();
}

void gd_focus_skip_break(void)
{
    gd_lock();
    if (s_focus.state == GD_FOCUS_BREAK)
    {
        memset(&s_focus, 0, sizeof(s_focus));
        gd_changed(false);
    }
    gd_unlock();
}

/* Locked. */
static void focus_tick(void)
{
    int64_t now_us = esp_timer_get_time();
    if ((s_focus.state != GD_FOCUS_RUNNING && s_focus.state != GD_FOCUS_BREAK) || now_us < s_focus.ends_at)
    {
        return;
    }
    gd_settings_t s = gd_settings_unlocked();
    if (s_focus.state == GD_FOCUS_RUNNING)
    {
        int minutes = s_focus.length_s / 60;
        int32_t today = (int32_t)(gd_now() / 86400);
        if (today > 0)
        {
            focus_days_shift(today);
            uint32_t sum = s_focus_days[FOCUS_DAYS - 1] + (uint32_t)minutes;
            s_focus_days[FOCUS_DAYS - 1] = sum > 65535 ? 65535 : (uint16_t)sum;
        }
        if (s_focus.task != 0)
        {
            gd_task_credit(s_focus.task, minutes);
        }
        uint32_t task = s_focus.task;
        s_sessions_in_row++;
        bool long_break = s_sessions_in_row % s.sessions_per_long == 0;
        int pause = long_break ? s.long_break : s.short_break;
        char detail[64];
        snprintf(detail, sizeof(detail), "%d minuti di focus. Ora %d di pausa.", minutes, pause);
        char title[GD_TITLE];
        gd_copy(title, sizeof(title), s_focus.title[0] ? s_focus.title : "Sessione completata");
        s_focus.state = GD_FOCUS_BREAK;
        s_focus.length_s = pause * 60;
        s_focus.ends_at = now_us + (int64_t)s_focus.length_s * 1000000;
        raise_alert(title, detail, 0x2FCF7F, true);
        gd_unlock();
        focus_log(today, minutes, task);
        gd_lock();
    }
    else
    {
        memset(&s_focus, 0, sizeof(s_focus));
        raise_alert("Pausa finita", "Pronto per un'altra sessione?", 0x5B8CFF, true);
    }
}

/* ------------------------------------------------------------- reminders */

static void reminder_tick(ics_time_t now)
{
    gd_settings_t s = gd_settings_unlocked();
    int count = 0;
    const gd_item_t *items = gd_items(&count);
    for (int i = 0; i < count; i++)
    {
        const gd_item_t *it = &items[i];
        if (it->all_day)
        {
            continue;
        }
        int lead = it->source == -2 ? 0 : s.remind_minutes;
        if (it->source == -1)
        {
            int count_events = 0;
            const gd_event_t *events = gd_events(&count_events);
            for (int e = 0; e < count_events; e++)
            {
                if (events[e].id == it->ref && events[e].remind >= 0)
                {
                    lead = events[e].remind;
                }
            }
        }
        if ((lead == 0 && it->source != -2) || it->start < now - 60 || it->start > now + lead * 60)
        {
            continue;
        }
        if (seen(hash(it->start, it->title)))
        {
            continue;
        }
        char detail[64], where[28];
        int minutes = (int)((it->start - now + 59) / 60);
        int hour = (int)((it->start % 86400) / 3600), minute = (int)((it->start % 3600) / 60);
        gd_copy(where, sizeof(where), it->location);
        if (it->source == -2)
        {
            snprintf(detail, sizeof(detail), "Scadenza alle %02d:%02d", hour, minute);
        }
        else if (minutes <= 0)
        {
            snprintf(detail, sizeof(detail), "Adesso · %02d:%02d%s%s", hour, minute, where[0] ? " · " : "", where);
        }
        else
        {
            snprintf(detail, sizeof(detail), "Tra %d min · %02d:%02d%s%s", minutes, hour, minute, where[0] ? " · " : "",
                     where);
        }
        raise_alert(it->title, detail, it->color, true);
        break;   /* one at a time */
    }
}

/* ------------------------------------------------------------------ tick */

static void tick_job(void *arg)
{
    (void)arg;
    ics_time_t now = gd_now();
    gd_lock();
    if (now > 0)
    {
        int32_t day = (int32_t)(now / 86400);
        if (day != s_day)
        {
            bool first = s_day < 0;
            s_day = day;
            if (!first)
            {
                gd_rebuild_agenda();   /* the 14-day window moves */
            }
            focus_days_shift(day);
        }
        reminder_tick(now);
    }
    focus_tick();
    gd_unlock();
    gd_store_flush(false);
}

static void tick_cb(void *arg)
{
    (void)arg;
    sys_post(tick_job, NULL);
}

void gd_agenda_start(void)
{
    s_focus_days = heap_caps_calloc(FOCUS_DAYS, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    focus_load();
    /* Events already started when the app opens are not announced again. */
    ics_time_t now = gd_now();
    gd_lock();
    int count = 0;
    const gd_item_t *items = gd_items(&count);
    for (int i = 0; i < count; i++)
    {
        if (items[i].start <= now)
        {
            seen(hash(items[i].start, items[i].title));
        }
    }
    gd_unlock();
    const esp_timer_create_args_t args = {.callback = tick_cb, .name = "giornata"};
    if (esp_timer_create(&args, &s_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_timer, TICK_US);
    }
}
