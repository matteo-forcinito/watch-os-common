/*
 * Giornata data (see giornata.h): tasks, own events, calendar sources and
 * settings, kept in PSRAM and saved as one JSON file a moment after a change
 * (the agenda tick calls gd_store_flush()). The merged agenda is rebuilt from
 * the calendar cache (sync.c), own events and task due times.
 */
#include "esp_attr.h"
#include "giornata.h"
#include "internal.h"

#include "services/storage.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "giornata";

static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static gd_source_t s_sources[GD_MAX_SOURCES];
static int s_source_count;
static gd_task_t *s_tasks;
static int s_task_count;
static gd_event_t *s_events;
static int s_event_count;
static gd_settings_t s_settings;
static gd_item_t *s_items;           /* merged agenda */
static int s_item_count;
gd_item_t *gd_cache;                 /* calendars only, written by sync.c (locked) */
int gd_cache_count;
static uint32_t s_revision;
static uint32_t s_next_id = 1;
static bool s_dirty;
static int64_t s_dirty_since;
gd_status_t gd_state;

/* ---------------------------------------------------------------- base */

void gd_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void gd_unlock(void) { xSemaphoreGive(s_lock); }

const gd_item_t *gd_items(int *count) { *count = s_item_count; return s_items; }
const gd_task_t *gd_tasks(int *count) { *count = s_task_count; return s_tasks; }
const gd_source_t *gd_sources(int *count) { *count = s_source_count; return s_sources; }
const gd_event_t *gd_events(int *count) { *count = s_event_count; return s_events; }

gd_settings_t gd_settings(void)
{
    gd_lock();
    gd_settings_t s = s_settings;
    gd_unlock();
    return s;
}

uint32_t gd_revision(void)
{
    return __atomic_load_n(&s_revision, __ATOMIC_RELAXED);
}

void gd_status(gd_status_t *out)
{
    gd_lock();
    *out = gd_state;
    out->revision = s_revision;
    out->items = s_item_count;
    gd_unlock();
}

/* Locked. */
void gd_changed(bool save)
{
    __atomic_add_fetch(&s_revision, 1, __ATOMIC_RELAXED);
    if (save && !s_dirty)
    {
        s_dirty = true;
        s_dirty_since = time(NULL);
    }
}

void gd_copy(char *out, size_t size, const char *in)
{
    /* Whole, valid UTF-8 characters only: a bad byte (a Latin-1 client) is dropped,
     * the text is never cut inside a character. */
    size_t n = 0;
    const unsigned char *c = (const unsigned char *)in;
    while (*c != '\0')
    {
        size_t length = *c < 0x80 ? 1 : (*c & 0xE0) == 0xC0 ? 2 : (*c & 0xF0) == 0xE0 ? 3 : (*c & 0xF8) == 0xF0 ? 4 : 0;
        bool valid = length > 0 && !(length == 2 && *c < 0xC2);
        for (size_t i = 1; valid && i < length; i++)
        {
            valid = (c[i] & 0xC0) == 0x80;
        }
        if (!valid)
        {
            c++;
            continue;
        }
        if (n + length >= size)
        {
            break;
        }
        memcpy(out + n, c, length);
        n += length;
        c += length;
    }
    out[n] = '\0';
}

ics_time_t gd_to_local(int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tm;
    localtime_r(&t, &tm);
    return ics_days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 +
           tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

ics_time_t gd_now(void)
{
    time_t now = time(NULL);
    return now < 1700000000 ? 0 : gd_to_local(now);
}

/* -------------------------------------------------------------- agenda */

static int by_start(const void *a, const void *b)
{
    const gd_item_t *x = a, *y = b;
    int64_t dx = x->start / 86400, dy = y->start / 86400;
    if (dx != dy)
    {
        return dx < dy ? -1 : 1;
    }
    if (x->all_day != y->all_day)
    {
        return x->all_day ? -1 : 1;   /* all-day first in a day */
    }
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

static void add_item(const gd_item_t *item)
{
    if (s_item_count < GD_MAX_ITEMS)
    {
        s_items[s_item_count++] = *item;
    }
}

/* Own events repeat simply: every day/week/month/year at the same time. */
static void expand_event(const gd_event_t *e, ics_time_t from, ics_time_t to)
{
    ics_event_t r;
    memset(&r, 0, sizeof(r));
    r.start = e->start;
    r.end = e->end > e->start ? e->end : e->start;
    r.all_day = e->all_day;
    r.duration = -1;
    r.freq = e->repeat == GD_REPEAT_DAILY ? ICS_DAILY : e->repeat == GD_REPEAT_WEEKLY ? ICS_WEEKLY :
             e->repeat == GD_REPEAT_MONTHLY ? ICS_MONTHLY : e->repeat == GD_REPEAT_YEARLY ? ICS_YEARLY : ICS_NONE;
    r.interval = 1;
    snprintf(r.uid, sizeof(r.uid), "own-%lu", (unsigned long)e->id);
    gd_copy(r.title, sizeof(r.title), e->title);
    gd_copy(r.location, sizeof(r.location), e->location);
    gd_item_t item = {.all_day = e->all_day, .color = e->color, .source = -1, .ref = e->id};
    gd_copy(item.title, sizeof(item.title), e->title);
    gd_copy(item.location, sizeof(item.location), e->location);
    if (r.freq == ICS_NONE)
    {
        if (r.end > from && r.start < to)
        {
            item.start = r.start;
            item.end = r.end;
            add_item(&item);
        }
        return;
    }
    /* Reuse the calendar expansion for the repeats. */
    ics_reader_t *reader = heap_caps_malloc(sizeof(*reader), MALLOC_CAP_SPIRAM);
    if (reader == NULL)
    {
        return;
    }
    gd_item_t *base = &item;
    ics_init(reader, from, to, NULL, gd_collect_own, NULL, base);
    ics_expand(reader, &r);
    free(reader);
}

bool gd_collect_own(const ics_occurrence_t *o, void *ctx)
{
    gd_item_t item = *(const gd_item_t *)ctx;
    item.start = o->start;
    item.end = o->end;
    add_item(&item);
    return s_item_count < GD_MAX_ITEMS;
}

void gd_rebuild_agenda(void)
{
    ics_time_t now = gd_now();
    ics_time_t from = (now / 86400 - 1) * 86400, to = from + (GD_DAYS + 1) * 86400LL;
    s_item_count = 0;
    for (int i = 0; i < gd_cache_count; i++)
    {
        int source = gd_cache[i].source;
        if (source >= 0 && source < s_source_count && s_sources[source].enabled)
        {
            gd_item_t item = gd_cache[i];
            item.color = s_sources[source].color;
            add_item(&item);
        }
    }
    for (int i = 0; i < s_event_count; i++)
    {
        expand_event(&s_events[i], from, to);
    }
    for (int i = 0; i < s_task_count; i++)
    {
        const gd_task_t *t = &s_tasks[i];
        if (t->due != 0 && t->column != GD_DONE && t->due >= from && t->due < to)
        {
            gd_item_t item = {.start = t->due, .end = t->due, .color = 0xFFB547, .source = -2, .ref = t->id};
            gd_copy(item.title, sizeof(item.title), t->title);
            add_item(&item);
        }
    }
    qsort(s_items, s_item_count, sizeof(gd_item_t), by_start);
    gd_changed(false);
}

/* ---------------------------------------------------------------- tasks */

static gd_task_t *task_by_id(uint32_t id)
{
    for (int i = 0; i < s_task_count; i++)
    {
        if (s_tasks[i].id == id)
        {
            return &s_tasks[i];
        }
    }
    return NULL;
}

static void renumber(uint8_t column)
{
    /* Orders 0..n-1 inside a column, keeping the current sequence. */
    for (uint16_t next = 0;; next++)
    {
        gd_task_t *best = NULL;
        for (int i = 0; i < s_task_count; i++)
        {
            gd_task_t *t = &s_tasks[i];
            if (t->column == column && t->order >= next && (best == NULL || t->order < best->order))
            {
                best = t;
            }
        }
        if (best == NULL)
        {
            return;
        }
        best->order = next;
    }
}

uint32_t gd_task_add(const char *title, uint8_t column, ics_time_t due, uint16_t estimate, uint8_t priority)
{
    gd_lock();
    uint32_t id = 0;
    if (s_task_count < GD_MAX_TASKS && title != NULL && title[0] != '\0')
    {
        gd_task_t *t = &s_tasks[s_task_count++];
        memset(t, 0, sizeof(*t));
        t->id = id = s_next_id++;
        gd_copy(t->title, sizeof(t->title), title);
        t->column = column > GD_DONE ? GD_TODO : column;
        t->due = due;
        t->estimate = estimate;
        t->priority = priority ? 1 : 0;
        t->created = time(NULL);
        t->order = 0xFFFF;   /* at the end */
        renumber(t->column);
        if (due != 0)
        {
            gd_rebuild_agenda();
        }
        gd_changed(true);
    }
    gd_unlock();
    return id;
}

bool gd_task_update(const gd_task_t *task)
{
    gd_lock();
    gd_task_t *t = task_by_id(task->id);
    if (t != NULL)
    {
        uint8_t column = t->column;
        *t = *task;
        if (t->column > GD_DONE)
        {
            t->column = column;
        }
        if (t->column == GD_DONE && t->completed == 0)
        {
            t->completed = time(NULL);
        }
        else if (t->column != GD_DONE)
        {
            t->completed = 0;
        }
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return t != NULL;
}

bool gd_task_move(uint32_t id, uint8_t column, int position)
{
    if (column > GD_DONE)
    {
        return false;
    }
    gd_lock();
    gd_task_t *t = task_by_id(id);
    if (t != NULL)
    {
        uint8_t from = t->column;
        t->column = column;
        t->completed = column == GD_DONE ? (t->completed ? t->completed : time(NULL)) : 0;
        /* Make room at `position`, then pack both columns. */
        for (int i = 0; i < s_task_count; i++)
        {
            if (s_tasks[i].column == column && &s_tasks[i] != t && s_tasks[i].order >= position)
            {
                s_tasks[i].order++;
            }
        }
        t->order = (uint16_t)(position < 0 ? 0xFFFF : position);
        renumber(column);
        if (from != column)
        {
            renumber(from);
        }
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return t != NULL;
}

bool gd_task_toggle(uint32_t id)
{
    gd_lock();
    gd_task_t *t = task_by_id(id);
    uint8_t column = t != NULL && t->column == GD_DONE ? GD_TODAY : GD_DONE;
    gd_unlock();
    return t != NULL && gd_task_move(id, column, column == GD_DONE ? 0 : -1);
}

bool gd_task_delete(uint32_t id)
{
    gd_lock();
    bool found = false;
    for (int i = 0; i < s_task_count; i++)
    {
        if (s_tasks[i].id == id)
        {
            uint8_t column = s_tasks[i].column;
            memmove(&s_tasks[i], &s_tasks[i + 1], sizeof(gd_task_t) * (s_task_count - i - 1));
            s_task_count--;
            renumber(column);
            found = true;
            break;
        }
    }
    if (found)
    {
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return found;
}

/* Locked: the focus timer credits a finished session to its task. */
void gd_task_credit(uint32_t id, int minutes)
{
    gd_task_t *t = task_by_id(id);
    if (t != NULL)
    {
        t->sessions++;
        t->focus_minutes += (uint32_t)minutes;
        gd_changed(true);
    }
}

/* Locked: a session stopped early still counts its minutes. */
void gd_task_add_minutes(uint32_t id, int minutes)
{
    gd_task_t *t = task_by_id(id);
    if (t != NULL)
    {
        t->focus_minutes += (uint32_t)minutes;
        gd_changed(true);
    }
}

gd_settings_t gd_settings_unlocked(void)
{
    return s_settings;
}

/* --------------------------------------------------------------- events */

uint32_t gd_event_put(const gd_event_t *event)
{
    gd_lock();
    uint32_t id = 0;
    gd_event_t *slot = NULL;
    for (int i = 0; i < s_event_count && event->id != 0; i++)
    {
        if (s_events[i].id == event->id)
        {
            slot = &s_events[i];
        }
    }
    if (slot == NULL && event->id == 0 && s_event_count < GD_MAX_EVENTS)
    {
        slot = &s_events[s_event_count++];
        *slot = *event;
        slot->id = s_next_id++;
    }
    else if (slot != NULL)
    {
        *slot = *event;
    }
    if (slot != NULL)
    {
        id = slot->id;
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return id;
}

bool gd_event_delete(uint32_t id)
{
    gd_lock();
    bool found = false;
    for (int i = 0; i < s_event_count; i++)
    {
        if (s_events[i].id == id)
        {
            memmove(&s_events[i], &s_events[i + 1], sizeof(gd_event_t) * (s_event_count - i - 1));
            s_event_count--;
            found = true;
            break;
        }
    }
    if (found)
    {
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return found;
}

/* -------------------------------------------------------------- sources */

bool gd_source_put(int index, const gd_source_t *source)
{
    gd_lock();
    bool ok = false;
    if (index < 0 && s_source_count < GD_MAX_SOURCES)
    {
        index = s_source_count++;
        memset(&s_sources[index], 0, sizeof(gd_source_t));
    }
    if (index >= 0 && index < s_source_count)
    {
        gd_source_t *s = &s_sources[index];
        bool new_url = strcmp(s->url, source->url) != 0;
        gd_copy(s->name, sizeof(s->name), source->name);
        snprintf(s->url, sizeof(s->url), "%s", source->url);
        s->color = source->color;
        s->enabled = source->enabled;
        if (new_url)
        {
            s->synced = 0;
            s->count = 0;
            s->error[0] = '\0';
        }
        gd_rebuild_agenda();
        gd_changed(true);
        ok = true;
    }
    gd_unlock();
    if (ok)
    {
        gd_sync_now();
    }
    return ok;
}

bool gd_source_delete(int index)
{
    gd_lock();
    bool ok = index >= 0 && index < s_source_count;
    if (ok)
    {
        memmove(&s_sources[index], &s_sources[index + 1], sizeof(gd_source_t) * (s_source_count - index - 1));
        s_source_count--;
        /* Cached items of later sources shift down; this one's go. */
        int kept = 0;
        for (int i = 0; i < gd_cache_count; i++)
        {
            if (gd_cache[i].source == index)
            {
                continue;
            }
            gd_cache[kept] = gd_cache[i];
            if (gd_cache[kept].source > index)
            {
                gd_cache[kept].source--;
            }
            kept++;
        }
        gd_cache_count = kept;
        gd_rebuild_agenda();
        gd_changed(true);
    }
    gd_unlock();
    return ok;
}

/* Locked: sync.c records each source's result. */
gd_source_t *gd_source_at(int index)
{
    return index >= 0 && index < s_source_count ? &s_sources[index] : NULL;
}

int gd_source_count(void)
{
    return s_source_count;
}

/* ------------------------------------------------------------- settings */

static void defaults(gd_settings_t *s)
{
    s->remind_minutes = 10;
    s->focus_minutes = 25;
    s->short_break = 5;
    s->long_break = 15;
    s->sessions_per_long = 4;
    s->sound = true;
    s->quiet_from = 23;
    s->quiet_to = 7;
    s->day_start = 7;
}

void gd_set_settings(const gd_settings_t *settings)
{
    gd_settings_t s = *settings;
    s.focus_minutes = s.focus_minutes < 5 ? 5 : s.focus_minutes > 90 ? 90 : s.focus_minutes;
    s.short_break = s.short_break < 1 ? 1 : s.short_break > 30 ? 30 : s.short_break;
    s.long_break = s.long_break < 5 ? 5 : s.long_break > 60 ? 60 : s.long_break;
    s.sessions_per_long = s.sessions_per_long < 2 ? 2 : s.sessions_per_long > 8 ? 8 : s.sessions_per_long;
    s.remind_minutes = s.remind_minutes > 120 ? 120 : s.remind_minutes;
    s.quiet_from %= 24;
    s.quiet_to %= 24;
    s.day_start %= 24;
    gd_lock();
    s_settings = s;
    gd_changed(true);
    gd_unlock();
}

/* --------------------------------------------------------- persistence */

bool gd_path(const char *name, char *out, size_t size)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/giornata", root);
    mkdir(folder, 0775);
    snprintf(out, size, "%s/%s", folder, name);
    return true;
}

static cJSON *to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "next_id", s_next_id);
    cJSON *set = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddNumberToObject(set, "remind", s_settings.remind_minutes);
    cJSON_AddNumberToObject(set, "focus", s_settings.focus_minutes);
    cJSON_AddNumberToObject(set, "short", s_settings.short_break);
    cJSON_AddNumberToObject(set, "long", s_settings.long_break);
    cJSON_AddNumberToObject(set, "per_long", s_settings.sessions_per_long);
    cJSON_AddBoolToObject(set, "sound", s_settings.sound);
    cJSON_AddNumberToObject(set, "quiet_from", s_settings.quiet_from);
    cJSON_AddNumberToObject(set, "quiet_to", s_settings.quiet_to);
    cJSON_AddNumberToObject(set, "day_start", s_settings.day_start);
    cJSON *sources = cJSON_AddArrayToObject(root, "sources");
    for (int i = 0; i < s_source_count; i++)
    {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", s_sources[i].name);
        cJSON_AddStringToObject(j, "url", s_sources[i].url);
        cJSON_AddNumberToObject(j, "color", s_sources[i].color);
        cJSON_AddBoolToObject(j, "enabled", s_sources[i].enabled);
        cJSON_AddItemToArray(sources, j);
    }
    cJSON *tasks = cJSON_AddArrayToObject(root, "tasks");
    for (int i = 0; i < s_task_count; i++)
    {
        const gd_task_t *t = &s_tasks[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", t->id);
        cJSON_AddStringToObject(j, "title", t->title);
        cJSON_AddStringToObject(j, "notes", t->notes);
        cJSON_AddNumberToObject(j, "col", t->column);
        cJSON_AddNumberToObject(j, "pri", t->priority);
        cJSON_AddNumberToObject(j, "due", (double)t->due);
        cJSON_AddNumberToObject(j, "est", t->estimate);
        cJSON_AddNumberToObject(j, "ses", t->sessions);
        cJSON_AddNumberToObject(j, "fm", t->focus_minutes);
        cJSON_AddNumberToObject(j, "created", (double)t->created);
        cJSON_AddNumberToObject(j, "completed", (double)t->completed);
        cJSON_AddNumberToObject(j, "order", t->order);
        cJSON_AddItemToArray(tasks, j);
    }
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    for (int i = 0; i < s_event_count; i++)
    {
        const gd_event_t *e = &s_events[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", e->id);
        cJSON_AddStringToObject(j, "title", e->title);
        cJSON_AddStringToObject(j, "loc", e->location);
        cJSON_AddNumberToObject(j, "start", (double)e->start);
        cJSON_AddNumberToObject(j, "end", (double)e->end);
        cJSON_AddBoolToObject(j, "all_day", e->all_day);
        cJSON_AddNumberToObject(j, "repeat", e->repeat);
        cJSON_AddNumberToObject(j, "color", e->color);
        cJSON_AddNumberToObject(j, "remind", e->remind);
        cJSON_AddItemToArray(events, j);
    }
    return root;
}

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

static void str(const cJSON *o, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(out, size, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static void from_json(const cJSON *root)
{
    s_next_id = (uint32_t)num(root, "next_id", 1);
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(set))
    {
        s_settings.remind_minutes = (uint8_t)num(set, "remind", s_settings.remind_minutes);
        s_settings.focus_minutes = (uint8_t)num(set, "focus", s_settings.focus_minutes);
        s_settings.short_break = (uint8_t)num(set, "short", s_settings.short_break);
        s_settings.long_break = (uint8_t)num(set, "long", s_settings.long_break);
        s_settings.sessions_per_long = (uint8_t)num(set, "per_long", s_settings.sessions_per_long);
        s_settings.sound = num(set, "sound", s_settings.sound) != 0;
        s_settings.quiet_from = (uint8_t)num(set, "quiet_from", s_settings.quiet_from);
        s_settings.quiet_to = (uint8_t)num(set, "quiet_to", s_settings.quiet_to);
        s_settings.day_start = (uint8_t)num(set, "day_start", s_settings.day_start);
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "sources"))
    {
        if (s_source_count == GD_MAX_SOURCES)
        {
            break;
        }
        gd_source_t *s = &s_sources[s_source_count++];
        memset(s, 0, sizeof(*s));
        str(item, "name", s->name, sizeof(s->name));
        str(item, "url", s->url, sizeof(s->url));
        s->color = (uint32_t)num(item, "color", 0x5B8CFF);
        s->enabled = num(item, "enabled", 1) != 0;
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "tasks"))
    {
        if (s_task_count == GD_MAX_TASKS)
        {
            break;
        }
        gd_task_t *t = &s_tasks[s_task_count++];
        memset(t, 0, sizeof(*t));
        t->id = (uint32_t)num(item, "id", 0);
        str(item, "title", t->title, sizeof(t->title));
        str(item, "notes", t->notes, sizeof(t->notes));
        t->column = (uint8_t)num(item, "col", GD_TODO);
        t->column = t->column > GD_DONE ? GD_TODO : t->column;
        t->priority = (uint8_t)num(item, "pri", 0);
        t->due = (ics_time_t)num(item, "due", 0);
        t->estimate = (uint16_t)num(item, "est", 0);
        t->sessions = (uint16_t)num(item, "ses", 0);
        t->focus_minutes = (uint32_t)num(item, "fm", 0);
        t->created = (int64_t)num(item, "created", 0);
        t->completed = (int64_t)num(item, "completed", 0);
        t->order = (uint16_t)num(item, "order", 0);
        if (t->id >= s_next_id)
        {
            s_next_id = t->id + 1;
        }
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "events"))
    {
        if (s_event_count == GD_MAX_EVENTS)
        {
            break;
        }
        gd_event_t *e = &s_events[s_event_count++];
        memset(e, 0, sizeof(*e));
        e->id = (uint32_t)num(item, "id", 0);
        str(item, "title", e->title, sizeof(e->title));
        str(item, "loc", e->location, sizeof(e->location));
        e->start = (ics_time_t)num(item, "start", 0);
        e->end = (ics_time_t)num(item, "end", 0);
        e->all_day = num(item, "all_day", 0) != 0;
        e->repeat = (uint8_t)num(item, "repeat", 0);
        e->color = (uint32_t)num(item, "color", 0x2FCF7F);
        e->remind = (int16_t)num(item, "remind", -1);
        if (e->id >= s_next_id)
        {
            s_next_id = e->id + 1;
        }
    }
}

static void load(void)
{
    char path[80];
    if (!gd_path("data.json", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    if (file != NULL)
    {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size > 0 && size < 512 * 1024)
        {
            text = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
            if (text != NULL && fread(text, 1, size, file) == (size_t)size)
            {
                text[size] = '\0';
            }
            else
            {
                free(text);
                text = NULL;
            }
        }
        fclose(file);
    }
    storage_files_release();
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (root != NULL)
    {
        from_json(root);
        cJSON_Delete(root);
        ESP_LOGI(TAG, "%d tasks, %d events, %d calendars", s_task_count, s_event_count, s_source_count);
    }
}

/* Called from the agenda tick (sys worker): save 2 s after the last change. */
void gd_store_flush(bool force)
{
    gd_lock();
    bool due = s_dirty && (force || time(NULL) - s_dirty_since >= 2);
    char *text = NULL;
    if (due)
    {
        cJSON *root = to_json();
        text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        s_dirty = false;
    }
    gd_unlock();
    if (text == NULL)
    {
        return;
    }
    char path[80], temporary[88];
    if (gd_path("data.json", path, sizeof(path)) && storage_files_acquire())
    {
        snprintf(temporary, sizeof(temporary), "%s.new", path);
        FILE *file = fopen(temporary, "wb");
        bool ok = file != NULL && fwrite(text, 1, strlen(text), file) == strlen(text);
        if (file != NULL)
        {
            ok = fclose(file) == 0 && ok;
        }
        if (ok)
        {
            unlink(path);
            ok = rename(temporary, path) == 0;
        }
        if (!ok)
        {
            ESP_LOGW(TAG, "Not saved");
            gd_lock();
            gd_changed(true);   /* try again later */
            gd_unlock();
        }
        storage_files_release();
    }
    else {
        gd_lock();
        gd_changed(true); /* A concurrent file transfer must not discard a save. */
        gd_unlock();
    }
    cJSON_free(text);
}

esp_err_t gd_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tasks = heap_caps_calloc(GD_MAX_TASKS, sizeof(gd_task_t), MALLOC_CAP_SPIRAM);
    s_events = heap_caps_calloc(GD_MAX_EVENTS, sizeof(gd_event_t), MALLOC_CAP_SPIRAM);
    s_items = heap_caps_calloc(GD_MAX_ITEMS, sizeof(gd_item_t), MALLOC_CAP_SPIRAM);
    gd_cache = heap_caps_calloc(GD_MAX_ITEMS, sizeof(gd_item_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_tasks == NULL || s_events == NULL || s_items == NULL || gd_cache == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    defaults(&s_settings);
    load();
    gd_cache_load();
    gd_lock();
    gd_rebuild_agenda();
    gd_unlock();
    gd_sync_task_start();
    gd_agenda_start();
    return ESP_OK;
}
