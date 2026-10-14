/*
 * Giornata dashboard API (served by appkit; every route needs the paired token).
 *
 *   GET  /api/state        status, settings, focus, calendars (links masked)
 *   GET  /api/agenda       merged agenda for the next 14 days
 *   GET  /api/tasks        all tasks
 *   GET  /api/events       own events
 *   GET  /api/focus/days   ?days=112 -> minutes per day, oldest first
 *   POST /api/task         {id?, title, notes, column, priority, due, estimate}
 *   POST /api/task/move    {id, column, position}
 *   POST /api/task/delete  {id}
 *   POST /api/event        {id?, title, location, start, end, all_day, repeat, color, remind}
 *   POST /api/event/delete {id}
 *   POST /api/source       {index (-1 new), name, url ("" keeps the old one), color, enabled}
 *   POST /api/source/delete {index}
 *   POST /api/settings     {...gd_settings_t}
 *   POST /api/sync         download the calendars now
 *   POST /api/focus        {action: start|pause|stop|skip, task}
 *
 * Calendar links are secrets (anyone with a Google "secret address" reads the
 * calendar): they are stored on the watch and never sent back, only a hint.
 */
#include "giornata.h"
#include "internal.h"

#include "appkit.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_dashboard_html_end");

#define GUARD() do { if (!appkit_authorized(request)) return appkit_unauthorized(request); } while (0)

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

static const char *text(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static cJSON *body_json(httpd_req_t *request)
{
    char *raw = appkit_body(request, 4096);
    cJSON *root = raw != NULL ? cJSON_Parse(raw) : NULL;
    free(raw);
    return root;
}

static esp_err_t ok(httpd_req_t *request)
{
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t bad(httpd_req_t *request, const char *why)
{
    return appkit_error(request, "400 Bad Request", why);
}

/* "https://calendar.google.com/calendar/ical/abc…" -> host + start, never the secret part. */
static void hint(const char *url, char *out, size_t size)
{
    const char *host = strstr(url, "://");
    host = host != NULL ? host + 3 : url;
    size_t n = 0;
    while (host[n] != '\0' && host[n] != '/' && n < 40)
    {
        n++;
    }
    snprintf(out, size, "%.*s/…", (int)n, host);
}

static esp_err_t state(httpd_req_t *request)
{
    GUARD();
    gd_status_t st;
    gd_status(&st);
    gd_settings_t s = gd_settings();
    gd_focus_t f;
    gd_focus(&f);
    cJSON *out = cJSON_CreateObject();
    cJSON *status = cJSON_AddObjectToObject(out, "status");
    cJSON_AddBoolToObject(status, "syncing", st.syncing);
    cJSON_AddNumberToObject(status, "synced", (double)st.synced);
    cJSON_AddStringToObject(status, "message", st.message);
    cJSON_AddNumberToObject(status, "revision", st.revision);
    cJSON_AddNumberToObject(status, "now", (double)gd_now());
    cJSON *set = cJSON_AddObjectToObject(out, "settings");
    cJSON_AddNumberToObject(set, "remind_minutes", s.remind_minutes);
    cJSON_AddNumberToObject(set, "focus_minutes", s.focus_minutes);
    cJSON_AddNumberToObject(set, "short_break", s.short_break);
    cJSON_AddNumberToObject(set, "long_break", s.long_break);
    cJSON_AddNumberToObject(set, "sessions_per_long", s.sessions_per_long);
    cJSON_AddBoolToObject(set, "sound", s.sound);
    cJSON_AddNumberToObject(set, "quiet_from", s.quiet_from);
    cJSON_AddNumberToObject(set, "quiet_to", s.quiet_to);
    cJSON *focus = cJSON_AddObjectToObject(out, "focus");
    cJSON_AddNumberToObject(focus, "state", f.state);
    cJSON_AddNumberToObject(focus, "task", f.task);
    cJSON_AddStringToObject(focus, "title", f.title);
    cJSON_AddNumberToObject(focus, "remaining", f.state == GD_FOCUS_IDLE ? 0 : f.remaining_s);
    cJSON_AddNumberToObject(focus, "length", f.length_s);
    cJSON_AddNumberToObject(focus, "done_today", f.done_today);
    cJSON *sources = cJSON_AddArrayToObject(out, "sources");
    gd_lock();
    int count = 0;
    const gd_source_t *list = gd_sources(&count);
    for (int i = 0; i < count; i++)
    {
        cJSON *j = cJSON_CreateObject();
        char h[64];
        hint(list[i].url, h, sizeof(h));
        cJSON_AddStringToObject(j, "name", list[i].name);
        cJSON_AddStringToObject(j, "hint", h);
        cJSON_AddNumberToObject(j, "color", list[i].color);
        cJSON_AddBoolToObject(j, "enabled", list[i].enabled);
        cJSON_AddNumberToObject(j, "synced", (double)list[i].synced);
        cJSON_AddNumberToObject(j, "count", list[i].count);
        cJSON_AddStringToObject(j, "error", list[i].error);
        cJSON_AddItemToArray(sources, j);
    }
    gd_unlock();
    return appkit_json(request, out);
}

static esp_err_t agenda(httpd_req_t *request)
{
    GUARD();
    gd_item_t *items = heap_caps_malloc(sizeof(gd_item_t) * GD_MAX_ITEMS, MALLOC_CAP_SPIRAM);
    if (items == NULL)
    {
        return appkit_json(request, NULL);
    }
    gd_lock();
    int count = 0;
    memcpy(items, gd_items(&count), sizeof(gd_item_t) * GD_MAX_ITEMS);
    gd_items(&count);
    gd_unlock();
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "start", (double)items[i].start);
        cJSON_AddNumberToObject(j, "end", (double)items[i].end);
        cJSON_AddBoolToObject(j, "all_day", items[i].all_day);
        cJSON_AddNumberToObject(j, "color", items[i].color);
        cJSON_AddNumberToObject(j, "source", items[i].source);
        cJSON_AddNumberToObject(j, "ref", items[i].ref);
        cJSON_AddStringToObject(j, "title", items[i].title);
        cJSON_AddStringToObject(j, "location", items[i].location);
        cJSON_AddItemToArray(out, j);
    }
    free(items);
    return appkit_json(request, out);
}

static esp_err_t tasks(httpd_req_t *request)
{
    GUARD();
    gd_task_t *copy = heap_caps_malloc(sizeof(gd_task_t) * GD_MAX_TASKS, MALLOC_CAP_SPIRAM);
    if (copy == NULL)
    {
        return appkit_json(request, NULL);
    }
    gd_lock();
    int count = 0;
    memcpy(copy, gd_tasks(&count), sizeof(gd_task_t) * GD_MAX_TASKS);
    gd_tasks(&count);
    gd_unlock();
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        const gd_task_t *t = &copy[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", t->id);
        cJSON_AddStringToObject(j, "title", t->title);
        cJSON_AddStringToObject(j, "notes", t->notes);
        cJSON_AddNumberToObject(j, "column", t->column);
        cJSON_AddNumberToObject(j, "priority", t->priority);
        cJSON_AddNumberToObject(j, "due", (double)t->due);
        cJSON_AddNumberToObject(j, "estimate", t->estimate);
        cJSON_AddNumberToObject(j, "sessions", t->sessions);
        cJSON_AddNumberToObject(j, "focus_minutes", t->focus_minutes);
        cJSON_AddNumberToObject(j, "created", (double)t->created);
        cJSON_AddNumberToObject(j, "completed", (double)t->completed);
        cJSON_AddNumberToObject(j, "order", t->order);
        cJSON_AddItemToArray(out, j);
    }
    free(copy);
    return appkit_json(request, out);
}

static esp_err_t events(httpd_req_t *request)
{
    GUARD();
    cJSON *out = cJSON_CreateArray();
    gd_lock();
    int count = 0;
    const gd_event_t *list = gd_events(&count);
    for (int i = 0; i < count; i++)
    {
        const gd_event_t *e = &list[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", e->id);
        cJSON_AddStringToObject(j, "title", e->title);
        cJSON_AddStringToObject(j, "location", e->location);
        cJSON_AddNumberToObject(j, "start", (double)e->start);
        cJSON_AddNumberToObject(j, "end", (double)e->end);
        cJSON_AddBoolToObject(j, "all_day", e->all_day);
        cJSON_AddNumberToObject(j, "repeat", e->repeat);
        cJSON_AddNumberToObject(j, "color", e->color);
        cJSON_AddNumberToObject(j, "remind", e->remind);
        cJSON_AddItemToArray(out, j);
    }
    gd_unlock();
    return appkit_json(request, out);
}

static esp_err_t focus_days(httpd_req_t *request)
{
    GUARD();
    char value[8];
    appkit_query(request, "days", value, sizeof(value));
    int days = atoi(value);
    days = days < 7 ? 112 : days > 120 ? 120 : days;
    uint16_t minutes[120];
    days = gd_focus_days(days, minutes);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < days; i++)
    {
        cJSON_AddItemToArray(out, cJSON_CreateNumber(minutes[i]));
    }
    return appkit_json(request, out);
}

static esp_err_t task_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    const char *title = text(root, "title");
    if (!cJSON_IsObject(root) || title == NULL || title[0] == '\0')
    {
        cJSON_Delete(root);
        return bad(request, "Serve un titolo");
    }
    uint32_t id = (uint32_t)num(root, "id", 0);
    uint32_t result = 0;
    if (id == 0)
    {
        result = gd_task_add(title, (uint8_t)num(root, "column", GD_TODO), (ics_time_t)num(root, "due", 0),
                             (uint16_t)num(root, "estimate", 0), (uint8_t)num(root, "priority", 0));
        const char *notes = text(root, "notes");
        if (result != 0 && notes != NULL && notes[0] != '\0')
        {
            id = result;
        }
    }
    if (id != 0)
    {
        gd_task_t t;
        bool found = false;
        gd_lock();
        int count = 0;
        const gd_task_t *list = gd_tasks(&count);
        for (int i = 0; i < count; i++)
        {
            if (list[i].id == id)
            {
                t = list[i];
                found = true;
            }
        }
        gd_unlock();
        if (found)
        {
            gd_copy(t.title, sizeof(t.title), title);
            const char *notes = text(root, "notes");
            if (notes != NULL)
            {
                gd_copy(t.notes, sizeof(t.notes), notes);
            }
            t.column = (uint8_t)num(root, "column", t.column);
            t.priority = (uint8_t)num(root, "priority", t.priority);
            t.due = (ics_time_t)num(root, "due", (double)t.due);
            t.estimate = (uint16_t)num(root, "estimate", t.estimate);
            result = gd_task_update(&t) ? id : 0;
        }
    }
    cJSON_Delete(root);
    if (result == 0)
    {
        return bad(request, "Attività non salvata (massimo 120)");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", result);
    return appkit_json(request, out);
}

static esp_err_t task_move(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = gd_task_move((uint32_t)num(root, "id", 0), (uint8_t)num(root, "column", 0), (int)num(root, "position", -1));
    cJSON_Delete(root);
    return done ? ok(request) : bad(request, "Attività non trovata");
}

static esp_err_t task_delete(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = gd_task_delete((uint32_t)num(root, "id", 0));
    cJSON_Delete(root);
    return done ? ok(request) : bad(request, "Attività non trovata");
}

static esp_err_t event_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    const char *title = text(root, "title");
    if (!cJSON_IsObject(root) || title == NULL || title[0] == '\0')
    {
        cJSON_Delete(root);
        return bad(request, "Serve un titolo");
    }
    gd_event_t e = {0};
    e.id = (uint32_t)num(root, "id", 0);
    gd_copy(e.title, sizeof(e.title), title);
    const char *location = text(root, "location");
    gd_copy(e.location, sizeof(e.location), location != NULL ? location : "");
    e.start = (ics_time_t)num(root, "start", 0);
    e.end = (ics_time_t)num(root, "end", 0);
    e.all_day = num(root, "all_day", 0) != 0;
    e.repeat = (uint8_t)num(root, "repeat", 0);
    e.color = (uint32_t)num(root, "color", 0x2FCF7F);
    e.remind = (int16_t)num(root, "remind", -1);
    cJSON_Delete(root);
    if (e.start <= 0 || e.end < e.start || e.repeat > GD_REPEAT_YEARLY)
    {
        return bad(request, "Orari non validi");
    }
    uint32_t id = gd_event_put(&e);
    if (id == 0)
    {
        return bad(request, "Evento non salvato (massimo 80)");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", id);
    return appkit_json(request, out);
}

static esp_err_t event_delete(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = gd_event_delete((uint32_t)num(root, "id", 0));
    cJSON_Delete(root);
    return done ? ok(request) : bad(request, "Evento non trovato");
}

static esp_err_t source_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    int index = (int)num(root, "index", -1);
    gd_source_t s = {0};
    const char *name = text(root, "name"), *url = text(root, "url");
    gd_copy(s.name, sizeof(s.name), name != NULL && name[0] ? name : "Calendario");
    if (url != NULL && url[0] != '\0')
    {
        snprintf(s.url, sizeof(s.url), "%s", url);
    }
    else if (index >= 0)
    {
        gd_lock();
        int count = 0;
        const gd_source_t *list = gd_sources(&count);
        if (index < count)
        {
            snprintf(s.url, sizeof(s.url), "%s", list[index].url);   /* keep the secret link */
        }
        gd_unlock();
    }
    s.color = (uint32_t)num(root, "color", 0x5B8CFF);
    s.enabled = num(root, "enabled", 1) != 0;
    cJSON_Delete(root);
    if (strncmp(s.url, "https://", 8) != 0 && strncmp(s.url, "http://", 7) != 0 && strncmp(s.url, "webcal://", 9) != 0)
    {
        return bad(request, "Serve un indirizzo https:// o webcal:// di un calendario ICS");
    }
    return gd_source_put(index, &s) ? ok(request) : bad(request, "Massimo 6 calendari");
}

static esp_err_t source_delete(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = gd_source_delete((int)num(root, "index", -1));
    cJSON_Delete(root);
    return done ? ok(request) : bad(request, "Calendario non trovato");
}

static esp_err_t settings_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    gd_settings_t s = gd_settings();
    s.remind_minutes = (uint8_t)num(root, "remind_minutes", s.remind_minutes);
    s.focus_minutes = (uint8_t)num(root, "focus_minutes", s.focus_minutes);
    s.short_break = (uint8_t)num(root, "short_break", s.short_break);
    s.long_break = (uint8_t)num(root, "long_break", s.long_break);
    s.sessions_per_long = (uint8_t)num(root, "sessions_per_long", s.sessions_per_long);
    s.sound = num(root, "sound", s.sound) != 0;
    s.quiet_from = (uint8_t)num(root, "quiet_from", s.quiet_from);
    s.quiet_to = (uint8_t)num(root, "quiet_to", s.quiet_to);
    cJSON_Delete(root);
    gd_set_settings(&s);
    return ok(request);
}

static esp_err_t sync_post(httpd_req_t *request)
{
    GUARD();
    gd_sync_now();
    return ok(request);
}

static esp_err_t focus_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    const char *action = text(root, "action");
    uint32_t task = (uint32_t)num(root, "task", 0);
    if (action != NULL && strcmp(action, "start") == 0)
    {
        gd_focus_start(task);
    }
    else if (action != NULL && strcmp(action, "pause") == 0)
    {
        gd_focus_pause();
    }
    else if (action != NULL && strcmp(action, "stop") == 0)
    {
        gd_focus_stop();
    }
    else if (action != NULL && strcmp(action, "skip") == 0)
    {
        gd_focus_skip_break();
    }
    cJSON_Delete(root);
    return ok(request);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/agenda", .method = HTTP_GET, .handler = agenda},
    {.uri = "/api/tasks", .method = HTTP_GET, .handler = tasks},
    {.uri = "/api/events", .method = HTTP_GET, .handler = events},
    {.uri = "/api/focus/days", .method = HTTP_GET, .handler = focus_days},
    {.uri = "/api/task", .method = HTTP_POST, .handler = task_post},
    {.uri = "/api/task/move", .method = HTTP_POST, .handler = task_move},
    {.uri = "/api/task/delete", .method = HTTP_POST, .handler = task_delete},
    {.uri = "/api/event", .method = HTTP_POST, .handler = event_post},
    {.uri = "/api/event/delete", .method = HTTP_POST, .handler = event_delete},
    {.uri = "/api/source", .method = HTTP_POST, .handler = source_post},
    {.uri = "/api/source/delete", .method = HTTP_POST, .handler = source_delete},
    {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
    {.uri = "/api/sync", .method = HTTP_POST, .handler = sync_post},
    {.uri = "/api/focus", .method = HTTP_POST, .handler = focus_post},
};

static const appkit_web_config_t s_config = {
    .name = "Giornata",
    .page_start = dashboard_html_start,
    .page_end = dashboard_html_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t giornata_web_start(void)
{
    return appkit_web_start(&s_config);
}
