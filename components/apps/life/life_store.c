#include "life_store.h"
#include "core/clock.h"
#include "core/sys.h"
#include "core/state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "services/notify.h"
#include "services/storage.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FILE_LIMIT (512 * 1024)
static const char *const FILES[] = {"raccolta-v1.json", "zampe-v1.json"};
typedef struct
{
    SemaphoreHandle_t mutex;
    cJSON *doc;
    unsigned users;
    char *pending;
    bool saving;
    atomic_uint revision;
    char error[96];
} book_t;
static book_t s_books[LIFE_COUNT];
static esp_timer_handle_t s_due_timer;
static SemaphoreHandle_t s_plan_lock;
static StaticSemaphore_t s_plan_storage;
static atomic_bool s_hint;
static void due_job(void *arg);

int life_today(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char s[11];
    strftime(s, sizeof(s), "%Y-%m-%d", &tm);
    return life_day(s);
}
static void set_text(cJSON *o, const char *key, const char *value)
{
    cJSON *v = cJSON_CreateString(value);
    if (!cJSON_ReplaceItemInObjectCaseSensitive(o, key, v))
        cJSON_AddItemToObject(o, key, v);
}
static bool load_files(book_t *b, life_kind_t kind)
{
    if (b->doc)
        return true;
    char path[192];
    char *raw = NULL;
    if (storage_config_path(FILES[kind], path, sizeof(path)))
    {
        FILE *f = fopen(path, "rb");
        if (!f && errno != ENOENT)
        {
            snprintf(b->error, sizeof(b->error), "Archivio non accessibile: dati non sovrascritti");
            return false;
        }
        if (!f)
        {
            char fallback[208];
            snprintf(fallback, sizeof(fallback), "%s.bak", path);
            f = fopen(fallback, "rb");
            if (!f && errno != ENOENT)
                return false;
            if (!f)
            {
                snprintf(fallback, sizeof(fallback), "%s.tmp", path);
                f = fopen(fallback, "rb");
                if (!f && errno != ENOENT)
                    return false;
            }
        }
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long length = ftell(f);
            rewind(f);
            if (length > 0 && length <= FILE_LIMIT)
            {
                raw = heap_caps_malloc((size_t)length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (raw)
                {
                    size_t got = fread(raw, 1, (size_t)length, f);
                    raw[got] = 0;
                    if (got != (size_t)length)
                    {
                        free(raw);
                        raw = NULL;
                    }
                }
            }
            fclose(f);
            if (!raw)
            {
                snprintf(b->error, sizeof(b->error), "Archivio non leggibile: dati non sovrascritti");
                return false;
            }
            b->doc = cJSON_Parse(raw);
            free(raw);
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(b->doc, kind == LIFE_RACCOLTA ? "cities" : "pets");
            bool arrays_ok =
                kind == LIFE_RACCOLTA || (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(b->doc, "tasks")) &&
                                          cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(b->doc, "weights")) &&
                                          cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(b->doc, "logs")));
            if (!cJSON_IsObject(b->doc) || life_number(b->doc, "schema", 0) != 1 || !cJSON_IsArray(a) || !arrays_ok)
            {
                cJSON_Delete(b->doc);
                b->doc = NULL;
                snprintf(b->error, sizeof(b->error), "Archivio non valido: dati non sovrascritti");
                return false;
            }
        }
    }
    else
    {
        snprintf(b->error, sizeof(b->error), "Archivio file non disponibile");
        return false;
    }
    if (!b->doc)
        b->doc = life_defaults(kind);
    return b->doc != NULL;
}
static bool load(book_t *b, life_kind_t kind)
{
    if (b->doc)
        return true;
    if (!storage_files_acquire())
    {
        snprintf(b->error, sizeof(b->error), "Archivio occupato: riprova tra poco");
        return false;
    }
    bool ok = load_files(b, kind);
    storage_files_release();
    if (ok)
        b->error[0] = 0;
    return ok;
}
static void lock(book_t *b)
{
    xSemaphoreTake(b->mutex, portMAX_DELAY);
}
static void unlock(book_t *b)
{
    xSemaphoreGive(b->mutex);
}
static void release_cache(book_t *b)
{
    if (!b->users && !b->saving && !b->pending)
    {
        cJSON_Delete(b->doc);
        b->doc = NULL;
    }
}
esp_err_t life_open(life_kind_t kind)
{
    if ((unsigned)kind >= LIFE_COUNT || !s_books[kind].mutex)
        return ESP_ERR_INVALID_STATE;
    book_t *b = &s_books[kind];
    lock(b);
    bool ok = load(b, kind);
    if (ok)
        b->users++;
    unlock(b);
    return ok ? ESP_OK : ESP_FAIL;
}
void life_close(life_kind_t kind)
{
    if ((unsigned)kind >= LIFE_COUNT || !s_books[kind].mutex)
        return;
    book_t *b = &s_books[kind];
    lock(b);
    if (b->users)
        b->users--;
    release_cache(b);
    unlock(b);
}
uint32_t life_revision(life_kind_t kind)
{
    return (unsigned)kind < LIFE_COUNT ? atomic_load(&s_books[kind].revision) : 0;
}

static int64_t next_due(const cJSON *d)
{
    int today = life_today();
    if (today == LIFE_NO_DAY)
        return 0;
    time_t now = time(NULL);
    int64_t next = 0;
    char current[11];
    life_date(today, current);
    const cJSON *task;
    cJSON_ArrayForEach(task, cJSON_GetObjectItemCaseSensitive(d, "tasks"))
    {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(task, "enabled")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(task, "notify")))
            continue;
        int day = life_day(life_string(task, "due"));
        if (day == LIFE_NO_DAY)
            continue;
        if (day < today)
            day = today;
        if (!strcmp(life_string(task, "last_notified"), current) && day == today)
            day++;
        char iso[11];
        life_date(day, iso);
        struct tm tm = {0};
        tm.tm_year = atoi(iso) - 1900;
        tm.tm_mon = atoi(iso + 5) - 1;
        tm.tm_mday = atoi(iso + 8);
        tm.tm_hour = life_number(task, "minute", 540) / 60;
        tm.tm_min = life_number(task, "minute", 540) % 60;
        tm.tm_isdst = -1;
        time_t when = mktime(&tm);
        if (when < now + 1)
            when = now + 1;
        if (!next || when < next)
            next = when;
    }
    return next;
}
static void due_timer(void *arg)
{
    (void)arg;
    if (!sys_post(due_job, NULL) && s_due_timer)
        esp_timer_start_once(s_due_timer, 1000000);
}
static void plan(int64_t when)
{
    if (!s_plan_lock)
        return;
    xSemaphoreTake(s_plan_lock, portMAX_DELAY);
    if (!s_due_timer && when)
    {
        esp_timer_create_args_t args = {.callback = due_timer, .name = "zampe_due", .skip_unhandled_events = true};
        if (esp_timer_create(&args, &s_due_timer) != ESP_OK)
        {
            xSemaphoreGive(s_plan_lock);
            return;
        }
    }
    if (s_due_timer)
    {
        esp_timer_stop(s_due_timer);
        if (when)
        {
            int64_t us = (when - (int64_t)time(NULL)) * 1000000;
            if (us < 1000000)
                us = 1000000;
            esp_timer_start_once(s_due_timer, (uint64_t)us);
        }
    }
    xSemaphoreGive(s_plan_lock);
}
static void save_task(void *arg)
{
    life_kind_t kind = (life_kind_t)(uintptr_t)arg;
    book_t *b = &s_books[kind];
    for (;;)
    {
        lock(b);
        char *raw = b->pending;
        if (!raw)
        {
            b->saving = false;
            release_cache(b);
            unlock(b);
            break;
        }
        b->pending = NULL;
        unlock(b);
        char path[192], temporary[208], backup[208];
        bool ok = false;
        if (storage_config_path(FILES[kind], path, sizeof(path)) && storage_files_acquire())
        {
            snprintf(temporary, sizeof(temporary), "%s.tmp", path);
            snprintf(backup, sizeof(backup), "%s.bak", path);
            FILE *f = fopen(temporary, "wb");
            if (f)
            {
                size_t length = strlen(raw);
                bool written = fwrite(raw, 1, length, f) == length && fflush(f) == 0;
                if (written)
                    written = fsync(fileno(f)) == 0;
                int closed = fclose(f);
                if (written && closed == 0)
                {
                    unlink(backup);
                    rename(path, backup);
                    ok = rename(temporary, path) == 0;
                    if (!ok)
                        rename(backup, path);
                }
            }
            storage_files_release();
        }
        if (!ok)
        {
            lock(b);
            if (!b->pending)
            {
                b->pending = raw;
                raw = NULL;
            }
            b->saving = false;
            snprintf(b->error, sizeof(b->error), "Salvataggio non riuscito: dati mantenuti in RAM, riprova");
            atomic_fetch_add(&b->revision, 1);
            state_bump(STATE_LIFE_VERSION);
            unlock(b);
            free(raw);
            vTaskDelete(NULL);
            return;
        }
        cJSON *saved = cJSON_Parse(raw);
        bool metadata_ok = false;
        if (saved)
        {
            nvs_handle_t h;
            int64_t next = kind == LIFE_ZAMPE ? next_due(saved) : 0;
            if (nvs_open("life", NVS_READWRITE, &h) == ESP_OK)
            {
                esp_err_t metadata_err;
                if (kind == LIFE_ZAMPE)
                    metadata_err = nvs_set_i64(h, "dogs_next", next);
                else
                    metadata_err = nvs_set_u8(h, "waste_hint", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(saved, "menu_hint")));
                metadata_ok = metadata_err == ESP_OK && nvs_commit(h) == ESP_OK;
                nvs_close(h);
            }
            if (kind == LIFE_ZAMPE)
                plan(next);
            else
                atomic_store(&s_hint, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(saved, "menu_hint")));
            cJSON_Delete(saved);
        }
        if (!metadata_ok) {
            lock(b);
            if (!b->pending) { b->pending = raw; raw = NULL; }
            b->saving = false;
            snprintf(b->error,sizeof(b->error),"Salvataggio incompleto: dati mantenuti, riprova");
            atomic_fetch_add(&b->revision,1);
            state_bump(STATE_LIFE_VERSION);
            unlock(b);
            free(raw);
            vTaskDelete(NULL);
            return;
        }
        free(raw);
        lock(b);
        snprintf(b->error, sizeof(b->error), "%s",
                 ok ? "" : "Salvataggio non riuscito: controlla memoria file e riprova");
        atomic_fetch_add(&b->revision, 1);
        state_bump(STATE_LIFE_VERSION);
        unlock(b);
    }
    vTaskDelete(NULL);
}
static bool queue_save(book_t *b, life_kind_t kind, cJSON *candidate)
{
    char *raw = cJSON_PrintUnformatted(candidate);
    if (!raw || strlen(raw) > FILE_LIMIT)
    {
        free(raw);
        return false;
    }
    if (!b->saving)
    {
        b->saving = true;
        if (!sys_task_create(save_task, "care_save", 6144, (void *)(uintptr_t)kind, 2, 0))
        {
            b->saving = false;
            free(raw);
            return false;
        }
    }
    free(b->pending);
    b->pending = raw;
    return true;
}
bool life_command(life_kind_t kind, const cJSON *command, char *error, size_t size)
{
    if ((unsigned)kind >= LIFE_COUNT || !s_books[kind].mutex)
    {
        snprintf(error, size, "Servizio non disponibile");
        return false;
    }
    book_t *b = &s_books[kind];
    lock(b);
    if (!load(b, kind))
    {
        snprintf(error, size, "%s", b->error);
        unlock(b);
        return false;
    }
    cJSON *candidate = cJSON_Duplicate(b->doc, true);
    if (!candidate)
        snprintf(error, size, "Memoria insufficiente");
    bool ok = candidate && (!strcmp(life_string(command, "op"), "save.retry") ||
                            life_apply(kind, candidate, command, life_today(), error, size));
    if (ok && !queue_save(b, kind, candidate))
    {
        snprintf(error, size, "Memoria insufficiente per salvare");
        ok = false;
    }
    if (ok)
    {
        cJSON_Delete(b->doc);
        b->doc = candidate;
        atomic_fetch_add(&b->revision, 1);
        state_bump(STATE_LIFE_VERSION);
    }
    else
        cJSON_Delete(candidate);
    release_cache(b);
    unlock(b);
    return ok;
}
cJSON *life_snapshot(life_kind_t kind)
{
    if ((unsigned)kind >= LIFE_COUNT || !s_books[kind].mutex)
        return NULL;
    book_t *b = &s_books[kind];
    lock(b);
    cJSON *out = load(b, kind) ? life_view(kind, b->doc, life_today()) : NULL;
    if (out)
    {
        cJSON_AddStringToObject(out, "save_error", b->error);
        cJSON_AddBoolToObject(out, "saving", b->saving);
        cJSON_AddNumberToObject(out, "revision", atomic_load(&b->revision));
    }
    release_cache(b);
    unlock(b);
    return out;
}
static void due_job(void *arg)
{
    (void)arg;
    book_t *b = &s_books[LIFE_ZAMPE];
    if (life_open(LIFE_ZAMPE) != ESP_OK)
    {
        plan((int64_t)time(NULL) + 60);
        return;
    }
    lock(b);
    int today = life_today();
    char current[11];
    life_date(today, current);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int minute = tm.tm_hour * 60 + tm.tm_min;
    int sent = 0;
    cJSON *task;
    cJSON_ArrayForEach(task, cJSON_GetObjectItemCaseSensitive(b->doc, "tasks"))
    {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(task, "enabled")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(task, "notify")))
            continue;
        int day = life_day(life_string(task, "due"));
        if (day == LIFE_NO_DAY || day > today || (day == today && life_number(task, "minute", 540) > minute) ||
            !strcmp(life_string(task, "last_notified"), current))
            continue;
        if (sent++ < 8)
        {
            const cJSON *pet = life_find(cJSON_GetObjectItemCaseSensitive(b->doc, "pets"), life_number(task, "pet", 0));
            notify_t notification = {.id = 0x6c000000u + (uint32_t)life_number(task, "id", 0), .time = now};
            snprintf(notification.app, sizeof(notification.app), "Zampe");
            snprintf(notification.title, sizeof(notification.title), "%s", life_string(task, "label"));
            snprintf(notification.body, sizeof(notification.body), "%s: %s", life_string(pet, "name"),
                     life_string(task, "notes"));
            notify_add(&notification);
        }
        set_text(task, "last_notified", current);
    }
    if (sent > 8)
    {
        notify_t summary = {.id = 0x6cffffffu, .time = now};
        snprintf(summary.app, sizeof(summary.app), "Zampe");
        snprintf(summary.title, sizeof(summary.title), "Altri %d promemoria", sent - 8);
        snprintf(summary.body, sizeof(summary.body), "Apri Zampe per vedere tutte le attivita in scadenza");
        notify_add(&summary);
    }
    if (sent)
    {
        queue_save(b, LIFE_ZAMPE, b->doc);
        atomic_fetch_add(&b->revision, 1);
        state_bump(STATE_LIFE_VERSION);
    }
    else
        plan(next_due(b->doc));
    unlock(b);
    life_close(LIFE_ZAMPE);
}
static void retime_job(void *arg)
{
    (void)arg;
    book_t *b = &s_books[LIFE_ZAMPE];
    lock(b);
    if (load(b, LIFE_ZAMPE))
        plan(next_due(b->doc));
    release_cache(b);
    unlock(b);
}
static void time_changed(void)
{
    if (s_due_timer)
        sys_post(retime_job, NULL);
}
void life_services_init(void)
{
    if (!s_plan_lock)
    {
        s_plan_lock = xSemaphoreCreateMutexStatic(&s_plan_storage);
        clock_add_listener(time_changed);
    }
    for (int i = 0; i < LIFE_COUNT; i++)
        if (!s_books[i].mutex)
            s_books[i].mutex = xSemaphoreCreateMutex();
    nvs_handle_t h;
    int64_t next = 0;
    if (nvs_open("life", NVS_READONLY, &h) == ESP_OK)
    {
        uint8_t hint = 0;
        nvs_get_u8(h, "waste_hint", &hint);
        atomic_store(&s_hint, hint != 0);
        nvs_get_i64(h, "dogs_next", &next);
        nvs_close(h);
    }
    plan(next);
}
bool life_menu_hint(char *out, size_t size)
{
    if (!atomic_load(&s_hint))
        return false;
    cJSON *d = life_snapshot(LIFE_RACCOLTA);
    if (!d)
        return false;
    const cJSON *city = life_find(cJSON_GetObjectItemCaseSensitive(d, "cities"), life_number(d, "preferred", 0));
    cJSON *bins = life_collections(city, life_today() + 1);
    snprintf(out, size, "Domani: ");
    const cJSON *bin;
    bool first = true;
    cJSON_ArrayForEach(bin, bins)
    {
        size_t used = strlen(out);
        snprintf(out + used, size > used ? size - used : 0, "%s%s", first ? "" : ", ", life_string(bin, "name"));
        first = false;
    }
    if (first)
        snprintf(out, size, "Domani: nessun ritiro");
    bool have_city = city != NULL;
    cJSON_Delete(bins);
    cJSON_Delete(d);
    return have_city;
}
