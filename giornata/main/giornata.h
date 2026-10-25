#ifndef GIORNATA_H
#define GIORNATA_H

/*
 * Giornata: the day on the wrist - calendars, tasks, focus and reminders.
 *
 *   store.c    tasks, own events, calendar sources and settings in one JSON
 *              file (giornata/data.json); focus sessions in focus.csv.
 *   sync.c     every 20 min downloads the calendars (ICS links of Google,
 *              iCloud, Outlook...), expands them for the next 14 days (ics.c)
 *              and keeps a cache, so the agenda opens offline.
 *   agenda.c   today's merged agenda, reminders (sound + screen) and the
 *              focus timer; runs in the sys worker every few seconds.
 *   ui.c       the watch: day ring (round) / timeline, tasks, focus, reminders.
 *   web.c      the dashboard: week planner, task board, focus stats, calendars.
 *
 * Times are "civil seconds" (ics.h): local wall-clock time as if UTC.
 * Everything below is read under gd_lock()/gd_unlock().
 */

#include "ics.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#ifndef WATCH_LITE
#include "lvgl.h"
#endif

#define GD_MAX_SOURCES   6
#define GD_MAX_TASKS     120
#define GD_MAX_EVENTS    80      /* own events (made in the dashboard) */
#define GD_MAX_ITEMS     400     /* expanded agenda, 14 days */
#define GD_DAYS          14
#define GD_TITLE         72

typedef struct
{
    char name[32];
    char url[256];
    uint32_t color;              /* 0xRRGGBB */
    bool enabled;
    /* status of the last sync */
    int64_t synced;              /* unix time, 0 = never */
    int16_t count;               /* occurrences in the window */
    char error[48];
} gd_source_t;

enum { GD_TODO, GD_TODAY, GD_DONE };   /* task board columns */

typedef struct
{
    uint32_t id;
    char title[GD_TITLE];
    char notes[96];
    uint8_t column;              /* GD_TODO / GD_TODAY / GD_DONE */
    uint8_t priority;            /* 0 normal, 1 important */
    ics_time_t due;              /* civil seconds, 0 = none (a due time rings a reminder) */
    uint16_t estimate;           /* focus sessions planned */
    uint16_t sessions;           /* focus sessions done */
    uint32_t focus_minutes;
    int64_t created;             /* unix time */
    int64_t completed;           /* unix time, 0 = open */
    uint16_t order;              /* position inside the column */
} gd_task_t;

enum { GD_REPEAT_NONE, GD_REPEAT_DAILY, GD_REPEAT_WEEKLY, GD_REPEAT_MONTHLY, GD_REPEAT_YEARLY };

typedef struct
{
    uint32_t id;
    char title[GD_TITLE];
    char location[48];
    ics_time_t start;
    ics_time_t end;
    bool all_day;
    uint8_t repeat;              /* GD_REPEAT_* */
    uint32_t color;
    int16_t remind;              /* minutes before, -1 = default */
} gd_event_t;

typedef struct
{
    ics_time_t start;
    ics_time_t end;
    bool all_day;
    uint32_t color;
    int8_t source;               /* index of the calendar, -1 = own event, -2 = task due */
    uint32_t ref;                /* own event / task id */
    char title[GD_TITLE];
    char location[48];
} gd_item_t;

typedef struct
{
    uint8_t remind_minutes;      /* before calendar events, 0 = off */
    uint8_t focus_minutes;
    uint8_t short_break;
    uint8_t long_break;
    uint8_t sessions_per_long;
    bool sound;
    uint8_t quiet_from;          /* hours: no reminder sound between from and to (from == to: never quiet) */
    uint8_t quiet_to;
    uint8_t day_start;           /* hour the day view starts (rectangular timeline) */
} gd_settings_t;

typedef enum { GD_FOCUS_IDLE, GD_FOCUS_RUNNING, GD_FOCUS_PAUSED, GD_FOCUS_BREAK } gd_focus_state_t;

typedef struct
{
    gd_focus_state_t state;
    uint32_t task;               /* 0 = no task */
    char title[GD_TITLE];
    int64_t ends_at;             /* esp_timer us when running */
    int32_t remaining_s;         /* when paused */
    int32_t length_s;
    uint16_t done_today;         /* sessions completed today */
} gd_focus_t;

typedef struct
{
    bool active;
    char title[GD_TITLE];
    char detail[64];
    uint32_t color;
    uint32_t serial;             /* changes for every new reminder */
} gd_alert_t;

typedef struct
{
    bool syncing;
    int64_t synced;              /* unix time of the last full sync */
    char message[64];
    uint32_t revision;           /* changes with any data change */
    int items;
} gd_status_t;

esp_err_t gd_start(void);
void gd_lock(void);
void gd_unlock(void);

/* Read-only views (locked by the caller). */
const gd_item_t *gd_items(int *count);           /* sorted by start */
const gd_task_t *gd_tasks(int *count);
const gd_source_t *gd_sources(int *count);
const gd_event_t *gd_events(int *count);
gd_settings_t gd_settings(void);
void gd_status(gd_status_t *out);
uint32_t gd_revision(void);

/* Changes (take the lock themselves; saved shortly after). */
uint32_t gd_task_add(const char *title, uint8_t column, ics_time_t due, uint16_t estimate, uint8_t priority);
bool gd_task_update(const gd_task_t *task);
bool gd_task_move(uint32_t id, uint8_t column, int position);
bool gd_task_toggle(uint32_t id);                 /* done <-> today */
bool gd_task_delete(uint32_t id);
uint32_t gd_event_put(const gd_event_t *event);   /* id 0 = new */
bool gd_event_delete(uint32_t id);
bool gd_source_put(int index, const gd_source_t *source);   /* index -1 = new */
bool gd_source_delete(int index);
void gd_set_settings(const gd_settings_t *settings);
void gd_sync_now(void);
/* Internal app (CONFIG_APPKIT_HOSTED): the UI is open / closed. Closed, the
 * reminders become OS notifications and calendars sync every few hours. */
void gd_app_open(bool open);
bool gd_app_is_open(void);
/* From the agenda tick: a background sync when it is due (internal app). */
void gd_sync_background_tick(void);
/* Tick period for the app state (internal app: slower when closed). */
void gd_agenda_retime(void);

/* Focus. */
void gd_focus(gd_focus_t *out);
void gd_focus_start(uint32_t task);              /* 0 = no task */
void gd_focus_pause(void);
void gd_focus_stop(void);
void gd_focus_skip_break(void);
/* Focus minutes per day (oldest first), for the last `days` days. */
int gd_focus_days(int days, uint16_t *minutes);

/* Reminders. */
void gd_alert(gd_alert_t *out);
void gd_alert_dismiss(void);

/* Time helpers. */
ics_time_t gd_now(void);                          /* civil seconds, 0 if the clock is not set */
ics_time_t gd_to_local(int64_t utc);

/* Merge own events + task due times + calendar cache into the agenda (locked). */
void gd_rebuild_agenda(void);

/* sync.c */
void gd_sync_task_start(void);
/* agenda.c */
void gd_agenda_start(void);

/* UI and dashboard. */
#ifndef WATCH_LITE
void giornata_ui_create(lv_obj_t *screen);
#endif
bool giornata_ui_back(void);
/* Internal app closed (widgets already deleted by the app manager). */
void giornata_ui_destroy(void);
/* Internal app: the agenda service, at boot (app.c). */
void giornata_service_start(void);
esp_err_t giornata_web_start(void);

#endif
