/*
 * Calendar sync (see giornata.h): every 20 minutes, or at once after a change,
 * each enabled ICS link is streamed through the reader (ics.c). Recurring
 * events wait until the end of their file (moved instances may follow), then
 * expand. A calendar that fails keeps its previous occurrences.
 */
#include "giornata.h"
#include "internal.h"

#include "core/sys.h"
#include "services/storage.h"
#include "services/time_sync.h"
#include "services/wifi.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "giornata_sync";

#define PERIOD_MS     (20 * 60 * 1000)
#define MAX_DEFERRED  300
#define CACHE_MAGIC   0x47444331u   /* "GDC1" */

static TaskHandle_t s_task;

extern const char gts_root_r1_start[] asm("_binary_gts_root_r1_pem_start");

typedef struct
{
    gd_item_t *items;      /* this sync's occurrences, all calendars */
    int count;
    int source;
    int source_count;      /* this calendar's */
    ics_event_t *deferred;
    int deferred_count;
} sync_t;

void gd_sync_now(void)
{
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

static bool emit(const ics_occurrence_t *o, void *ctx)
{
    sync_t *s = ctx;
    if (s->count >= GD_MAX_ITEMS)
    {
        return false;
    }
    gd_item_t *item = &s->items[s->count++];
    memset(item, 0, sizeof(*item));
    item->start = o->start;
    item->end = o->end;
    item->all_day = o->all_day;
    item->source = (int8_t)s->source;
    gd_copy(item->title, sizeof(item->title), o->title[0] ? o->title : "(senza titolo)");
    gd_copy(item->location, sizeof(item->location), o->location);
    s->source_count++;
    return true;
}

static void defer(const ics_event_t *event, void *ctx)
{
    sync_t *s = ctx;
    if (s->deferred_count < MAX_DEFERRED)
    {
        s->deferred[s->deferred_count++] = *event;
    }
}

/* Download one calendar into the reader. Returns an error text or NULL. */
static const char *fetch(const char *link, ics_reader_t *reader)
{
    char url[260];
    if (strncmp(link, "webcal://", 9) == 0)
    {
        snprintf(url, sizeof(url), "https://%s", link + 9);
    }
    else
    {
        snprintf(url, sizeof(url), "%s", link);
    }
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)
    {
        return "Indirizzo non valido";
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Giornata/1.0 (watch)",
        .max_redirection_count = 5,
    };
    /* Google sends GTS Root R1 cross-signed by GlobalSign Root CA, which is no
     * longer in the certificate bundle: trust GTS Root R1 itself for Google. */
    const char *host = strstr(url, "://") + 3;
    if (strncmp(host, "calendar.google.com", 19) == 0 || strncmp(host, "www.google.com", 14) == 0)
    {
        config.crt_bundle_attach = NULL;
        config.cert_pem = gts_root_r1_start;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return "Memoria insufficiente";
    }
    const char *error = NULL;
    int status = 0;
    for (int hop = 0; hop < 5; hop++)
    {
        if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0)
        {
            error = "Server non raggiungibile";
            break;
        }
        status = esp_http_client_get_status_code(client);
        if (status >= 300 && status < 400 && esp_http_client_set_redirection(client) == ESP_OK)
        {
            esp_http_client_close(client);
            continue;
        }
        break;
    }
    if (error == NULL && status != 200)
    {
        error = status == 404 ? "Calendario non trovato (404)" : status == 401 || status == 403 ?
                "Accesso negato: usa l'indirizzo segreto" : "Risposta inattesa del server";
    }
    char *chunk = error == NULL ? heap_caps_malloc(4096, MALLOC_CAP_SPIRAM) : NULL;
    if (error == NULL && chunk == NULL)
    {
        error = "Memoria insufficiente";
    }
    bool first = true;
    while (error == NULL)
    {
        int got = esp_http_client_read(client, chunk, 4096);
        if (got < 0)
        {
            error = "Download interrotto";
            break;
        }
        if (got == 0)
        {
            if (!esp_http_client_is_complete_data_received(client))
            {
                error = "Download incompleto";
            }
            break;
        }
        if (first)
        {
            first = false;
            /* A web page instead of a calendar: the link is the wrong one. */
            if (strncmp(chunk, "BEGIN:VCALENDAR", got < 15 ? got : 15) != 0 &&
                !(got > 3 && (unsigned char)chunk[0] == 0xEF && strncmp(chunk + 3, "BEGIN:VCALENDAR", 15) == 0))
            {
                error = "Non è un calendario ICS";
                break;
            }
        }
        ics_feed_text(reader, chunk, (size_t)got);
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return error;
}

static void sync_all(void)
{
    gd_lock();
    int sources = gd_source_count();
    gd_source_t copy[GD_MAX_SOURCES];
    for (int i = 0; i < sources; i++)
    {
        copy[i] = *gd_source_at(i);
    }
    gd_state.syncing = true;
    snprintf(gd_state.message, sizeof(gd_state.message), "Aggiorno i calendari...");
    gd_changed(false);
    gd_unlock();

    sync_t *s = heap_caps_calloc(1, sizeof(sync_t), MALLOC_CAP_SPIRAM);
    ics_reader_t *reader = heap_caps_malloc(sizeof(ics_reader_t), MALLOC_CAP_SPIRAM);
    if (s != NULL)
    {
        s->items = heap_caps_calloc(GD_MAX_ITEMS, sizeof(gd_item_t), MALLOC_CAP_SPIRAM);
        s->deferred = heap_caps_calloc(MAX_DEFERRED, sizeof(ics_event_t), MALLOC_CAP_SPIRAM);
    }
    if (s == NULL || reader == NULL || s->items == NULL || s->deferred == NULL)
    {
        if (s != NULL)
        {
            free(s->items);
            free(s->deferred);
        }
        free(s);
        free(reader);
        return;
    }

    ics_time_t now = gd_now();
    ics_time_t from = (now / 86400 - 1) * 86400, to = from + (GD_DAYS + 1) * 86400LL;
    const char *errors[GD_MAX_SOURCES] = {0};
    int counts[GD_MAX_SOURCES] = {0};
    int ok = 0, enabled = 0;
    for (int i = 0; i < sources; i++)
    {
        if (!copy[i].enabled || copy[i].url[0] == '\0')
        {
            continue;
        }
        enabled++;
        int before = s->count;
        s->source = i;
        s->source_count = 0;
        s->deferred_count = 0;
        ics_init(reader, from, to, gd_to_local, emit, defer, s);
        errors[i] = fetch(copy[i].url, reader);
        if (errors[i] == NULL)
        {
            ics_finish(reader);
            for (int d = 0; d < s->deferred_count; d++)
            {
                ics_expand(reader, &s->deferred[d]);
            }
            counts[i] = s->source_count;
            ok++;
            ESP_LOGI(TAG, "%s: %d events, %d occurrences", copy[i].name, reader->events, s->source_count);
        }
        else
        {
            s->count = before;   /* drop a half-read calendar */
            ESP_LOGW(TAG, "%s: %s", copy[i].name, errors[i]);
        }
    }

    gd_lock();
    /* Failed or disabled calendars keep what they had. */
    for (int i = 0; i < gd_cache_count && s->count < GD_MAX_ITEMS; i++)
    {
        int src = gd_cache[i].source;
        if (src >= 0 && src < sources && errors[src] != NULL)
        {
            s->items[s->count++] = gd_cache[i];
        }
    }
    memcpy(gd_cache, s->items, sizeof(gd_item_t) * s->count);
    gd_cache_count = s->count;
    for (int i = 0; i < sources && i < gd_source_count(); i++)
    {
        gd_source_t *src = gd_source_at(i);
        if (!copy[i].enabled || strcmp(src->url, copy[i].url) != 0)
        {
            continue;
        }
        if (errors[i] == NULL)
        {
            src->synced = time(NULL);
            src->count = (int16_t)counts[i];
            src->error[0] = '\0';
        }
        else
        {
            snprintf(src->error, sizeof(src->error), "%s", errors[i]);
        }
    }
    gd_state.syncing = false;
    if (ok > 0)
    {
        gd_state.synced = time(NULL);
    }
    snprintf(gd_state.message, sizeof(gd_state.message), "%s",
             enabled == 0 ? "Nessun calendario collegato" : ok == enabled ? "Calendari aggiornati" :
             ok == 0 ? "Calendari non raggiungibili" : "Alcuni calendari non rispondono");
    gd_rebuild_agenda();
    gd_unlock();
    gd_cache_save();

    free(s->items);
    free(s->deferred);
    free(s);
    free(reader);
}

static void sync_task(void *arg)
{
    (void)arg;
    s_task = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the UI come up first */
    for (;;)
    {
        gd_lock();
        bool any = false;
        for (int i = 0; i < gd_source_count(); i++)
        {
            any |= gd_source_at(i)->enabled && gd_source_at(i)->url[0] != '\0';
        }
        gd_unlock();
        if (any || time(NULL) < 1700000000)
        {
            if (wifi_acquire(25000) == ESP_OK)
            {
                if (time(NULL) < 1700000000)
                {
                    time_sync_now();   /* the agenda needs the date */
                    gd_lock();
                    gd_rebuild_agenda();
                    gd_unlock();
                }
                if (any)
                {
                    wifi_set_fast(true);
                    sync_all();
                    wifi_set_fast(false);
                }
                wifi_release();
            }
            else if (any)
            {
                gd_lock();
                snprintf(gd_state.message, sizeof(gd_state.message), "Wi-Fi non raggiungibile");
                gd_changed(false);
                gd_unlock();
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
    }
}

void gd_sync_task_start(void)
{
    sys_task_create(sync_task, "giornata_sync", 8192, NULL, 3, SYS_CORE_ANY);
}

/* ---------------------------------------------------------------- cache */

typedef struct
{
    uint32_t magic;
    int32_t count;
    int64_t synced;
} cache_header_t;

void gd_cache_save(void)
{
    char path[80];
    if (!gd_path("cache.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "wb");
    if (file != NULL)
    {
        gd_lock();
        cache_header_t header = {.magic = CACHE_MAGIC, .count = gd_cache_count, .synced = gd_state.synced};
        fwrite(&header, sizeof(header), 1, file);
        fwrite(gd_cache, sizeof(gd_item_t), gd_cache_count, file);
        int sources = gd_source_count();
        for (int i = 0; i < sources; i++)
        {
            const gd_source_t *s = gd_source_at(i);
            fwrite(&s->synced, sizeof(s->synced), 1, file);
            fwrite(&s->count, sizeof(s->count), 1, file);
        }
        gd_unlock();
        fclose(file);
    }
    storage_files_release();
}

void gd_cache_load(void)
{
    char path[80];
    if (!gd_path("cache.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "rb");
    cache_header_t header;
    if (file != NULL && fread(&header, sizeof(header), 1, file) == 1 && header.magic == CACHE_MAGIC &&
        header.count >= 0 && header.count <= GD_MAX_ITEMS &&
        fread(gd_cache, sizeof(gd_item_t), header.count, file) == (size_t)header.count)
    {
        gd_lock();
        gd_cache_count = header.count;
        gd_state.synced = header.synced;
        snprintf(gd_state.message, sizeof(gd_state.message), "Calendari dalla memoria");
        for (int i = 0; i < gd_source_count(); i++)
        {
            gd_source_t *s = gd_source_at(i);
            if (fread(&s->synced, sizeof(s->synced), 1, file) != 1 || fread(&s->count, sizeof(s->count), 1, file) != 1)
            {
                break;
            }
        }
        gd_unlock();
    }
    if (file != NULL)
    {
        fclose(file);
    }
    storage_files_release();
}
