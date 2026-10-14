/*
 * News jobs (see news.h): rounds of downloads, archive searches and
 * Wikipedia backgrounds, one at a time in a task that exists only while
 * there is work (8 KB of internal RAM for the TLS handshakes).
 */
#include "news.h"
#include "news_internal.h"
#include "rss.h"

#include "core/sys.h"
#include "hardware/sdcard.h"
#include "services/time_sync.h"
#include "services/wifi.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "news";

#define WINDOW_SIZE      (16 * 1024)
#define FRESH_MINUTES    20                         /* opening the app refreshes older data */
#define BOOT_QUIET_US    (15LL * 60 * 1000000)       /* the boot has its own Wi-Fi jobs */
#define BACKGROUND_FREE  (115 * 1024)               /* Wi-Fi + this task with room to spare */
#define USER_AGENT       "NewsWatch/1.0 (ESP32 smartwatch; personal RSS reader)"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_running;               /* the job task exists */
static bool s_want_round, s_round_background;
static bool s_want_search, s_want_context;
static char s_search_query[80];
static int s_search_months;
static char s_context_query[80];
static bool s_searching, s_contexting;
static volatile bool s_app_open;
static volatile bool s_cancel;       /* a foreground round stops: the app closed */
static int64_t s_last_round_us;      /* esp_timer time of the last round start */

static void job_task(void *arg);

static void kick(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool start = !s_running;
    s_running = true;
    taskEXIT_CRITICAL(&s_mux);
    if (start && !sys_task_create(job_task, "news", 8192, NULL, 3, SYS_CORE_ANY))
    {
        taskENTER_CRITICAL(&s_mux);
        s_running = false;
        s_want_round = s_want_search = s_want_context = false;
        s_searching = s_contexting = false;
        taskEXIT_CRITICAL(&s_mux);
        nw_lock();
        nw_state.phase = NW_FAILED;
        snprintf(nw_state.message, sizeof(nw_state.message), "Memoria piena: riprova tra poco");
        nw_changed();
        nw_unlock();
    }
}

static void set_status(nw_phase_t phase, int percent, const char *message)
{
    nw_lock();
    nw_state.phase = phase;
    nw_state.percent = percent;
    if (message != NULL)
    {
        snprintf(nw_state.message, sizeof(nw_state.message), "%s", message);
    }
    nw_changed();
    nw_unlock();
}

/* ---------------------------------------------------------------- http */

typedef bool (*body_fn_t)(const char *data, int length, void *ctx);

/* GET with up to 3 redirects; the body goes to fn in chunks. */
static bool http_get(const char *url, body_fn_t fn, void *ctx, int *status_out)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (client == NULL || chunk == NULL)
    {
        if (client != NULL)
        {
            esp_http_client_cleanup(client);
        }
        free(chunk);
        return false;
    }
    bool ok = false;
    int status = 0;
    for (int hop = 0; hop < 4; hop++)
    {
        if (esp_http_client_open(client, 0) != ESP_OK)
        {
            break;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && hop < 3)
        {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        ok = status == 200;
        while (ok)
        {
            if (s_cancel)
            {
                ok = false;
                break;
            }
            int got = esp_http_client_read(client, chunk, 4096);
            if (got < 0)
            {
                ok = false;
                break;
            }
            if (got == 0)
            {
                ok = esp_http_client_is_complete_data_received(client);
                break;
            }
            if (!fn(chunk, got, ctx))
            {
                break;   /* the reader has enough */
            }
        }
        break;
    }
    if (status_out != NULL)
    {
        *status_out = status;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk);
    return ok;
}

/* --------------------------------------------------------------- round */

typedef struct
{
    rss_reader_t reader;
    nw_item_t *fresh;                /* this feed's items (PSRAM) */
    int count;
    int max;
    uint8_t feed;
    uint8_t topic;
    int64_t now;
} feed_ctx_t;

static bool on_item(const rss_item_t *item, void *ctx)
{
    feed_ctx_t *f = ctx;
    if (f->count >= f->max)
    {
        return false;
    }
    nw_item_t *it = &f->fresh[f->count];
    memset(it, 0, sizeof(*it));
    nw_copy(it->title, sizeof(it->title), item->title);
    nw_copy(it->link, sizeof(it->link), item->link);
    nw_copy(it->summary, sizeof(it->summary), item->summary);
    it->id = nw_hash(item->link[0] ? item->link : item->title);
    /* No date, or one from the future (wrong zone): the time we saw it. */
    it->when = item->when > 0 && item->when <= f->now + 3600 ? item->when : f->now;
    it->topic = f->topic;
    it->feed = f->feed;
    it->story = -1;
    nw_key_t keys[NW_KEY_MAX];
    int n = nw_keywords(item->title, keys, NW_KEY_MAX);
    for (int k = 0; k < n && k < 6; k++)
    {
        it->keys[it->key_count++] = keys[k].hash;
    }
    f->count++;
    return true;
}

static bool feed_body(const char *data, int length, void *ctx)
{
    feed_ctx_t *f = ctx;
    return rss_feed(&f->reader, data, (size_t)length);
}

static void run_round(bool background)
{
    set_status(NW_WAITING_WIFI, 0, "Connessione al Wi-Fi...");
    if (wifi_acquire(25000) != ESP_OK)
    {
        set_status(NW_FAILED, 0, "Wi-Fi non raggiungibile");
        return;
    }
    if (time(NULL) < 1700000000)
    {
        time_sync_now();   /* boards without a clock chip: dates need it */
    }
    wifi_set_fast(true);

    nw_settings_t settings;
    nw_settings(&settings);
    char *window = heap_caps_malloc(WINDOW_SIZE, MALLOC_CAP_SPIRAM);
    nw_item_t *fresh = heap_caps_malloc(sizeof(nw_item_t) * 40, MALLOC_CAP_SPIRAM);
    nw_item_t *added = heap_caps_malloc(sizeof(nw_item_t) * NW_ITEMS, MALLOC_CAP_SPIRAM);
    int added_count = 0;
    if (window != NULL && fresh != NULL && added != NULL)
    {
        int total = nw_feed_total(), failures = 0, done = 0;
        int enabled = 0;
        int topic_count;
        nw_lock();
        const nw_topic_t *topics = nw_topics(&topic_count);
        for (int i = 0; i < total; i++)
        {
            const nw_feed_t *f = &nw_feed_table()[i];
            enabled += f->enabled && f->topic < topic_count && topics[f->topic].enabled;
        }
        nw_unlock();
        for (int i = 0; i < total && !s_cancel; i++)
        {
            nw_feed_t feed;
            bool on;
            nw_lock();
            feed = nw_feed_table()[i];
            const nw_topic_t *t = nw_topics(&topic_count);
            on = feed.enabled && feed.topic < topic_count && t[feed.topic].enabled;
            nw_unlock();
            if (!on)
            {
                continue;
            }
            char message[64];
            snprintf(message, sizeof(message), "Leggo %s", feed.name);
            set_status(NW_FETCHING, enabled ? done * 100 / enabled : 0, message);

            feed_ctx_t ctx = {.fresh = fresh, .max = settings.per_feed, .feed = (uint8_t)i, .topic = feed.topic,
                              .now = time(NULL)};
            rss_init(&ctx.reader, window, WINDOW_SIZE, on_item, &ctx);
            int status = 0;
            bool ok = http_get(feed.url, feed_body, &ctx, &status) || ctx.count > 0;
            done++;

            nw_lock();
            nw_feed_t *live = i < nw_feed_total() ? &nw_feed_table()[i] : NULL;
            if (live != NULL && strcmp(live->url, feed.url) == 0)
            {
                live->count = (int16_t)ctx.count;
                if (ok && ctx.count > 0)
                {
                    live->fetched = time(NULL);
                    live->error[0] = '\0';
                }
                else
                {
                    snprintf(live->error, sizeof(live->error), status ? "Errore HTTP %d" : (ok ? "Nessuna notizia" : "Non raggiungibile"),
                             status);
                }
            }
            for (int k = 0; k < ctx.count; k++)
            {
                if (nw_add_item(&fresh[k]) && added_count < NW_ITEMS)
                {
                    added[added_count++] = fresh[k];
                }
            }
            nw_changed();
            nw_unlock();
            failures += !(ok && ctx.count > 0);
        }
        ESP_LOGI(TAG, "Round: %d feeds, %d failed, %d new items%s", done, failures, added_count,
                 s_cancel ? " (stopped: app closed)" : "");
    }
    wifi_set_fast(false);
    wifi_release();
    free(window);
    free(fresh);

    /* Analysis, offline: stories, archive, keywords of the day, trends. */
    set_status(NW_ANALYSING, 100, "Raggruppo le notizie...");
    nw_lock();
    nw_cluster();
    nw_save_items();
    int count = nw_item_count;
    nw_item_t *snapshot = heap_caps_malloc(sizeof(nw_item_t) * (size_t)(count ? count : 1), MALLOC_CAP_SPIRAM);
    if (snapshot != NULL)
    {
        memcpy(snapshot, nw_item_table, sizeof(nw_item_t) * (size_t)count);
    }
    nw_unlock();
    if (added != NULL && added_count > 0)
    {
        nw_archive(added, added_count);
    }
    free(added);
    if (snapshot != NULL)
    {
        nw_write_day_terms(snapshot, count);
        nw_compute_trends(snapshot, count);
        free(snapshot);
    }
    nw_archive_trim(!sdcard_is_mounted());

    char message[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(message, sizeof(message), "Aggiornato alle %02d:%02d", tm.tm_hour, tm.tm_min);
    nw_lock();
    nw_state.phase = NW_IDLE;
    nw_state.percent = 100;
    nw_state.fetched = now;
    nw_state.background_ok = true;
    snprintf(nw_state.message, sizeof(nw_state.message), "%s", message);
    nw_changed();
    nw_unlock();
    (void)background;
}

/* ------------------------------------------------------------- context */

typedef struct
{
    char *buffer;
    int used;
    int size;
} grow_t;

static bool collect(const char *data, int length, void *ctx)
{
    grow_t *g = ctx;
    int room = g->size - 1 - g->used;
    int n = length < room ? length : room;
    memcpy(g->buffer + g->used, data, (size_t)n);
    g->used += n;
    g->buffer[g->used] = '\0';
    return g->used < g->size - 1;
}

static void url_encode(const char *in, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < size; p++)
    {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
        {
            out[n++] = (char)*p;
        }
        else
        {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
}

static void run_context(const char *query)
{
    set_status(NW_SEARCHING, 0, "Cerco il contesto su Wikipedia...");
    char title[80] = "", *text = heap_caps_calloc(1, 1200, MALLOC_CAP_SPIRAM);
    grow_t g = {.buffer = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM), .size = 16 * 1024};
    char *url = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    char encoded[240];
    bool wifi = wifi_acquire(25000) == ESP_OK;
    if (wifi && text != NULL && g.buffer != NULL && url != NULL)
    {
        url_encode(query, encoded, sizeof(encoded));
        snprintf(url, 512, "https://it.wikipedia.org/w/api.php?action=query&list=search&srlimit=1&format=json&srsearch=%s",
                 encoded);
        cJSON *root = NULL;
        if (http_get(url, collect, &g, NULL) && (root = cJSON_Parse(g.buffer)) != NULL)
        {
            cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "query"), "search"), 0);
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(first, "title"));
            if (t != NULL)
            {
                nw_copy(title, sizeof(title), t);
            }
            cJSON_Delete(root);
        }
        if (title[0] != '\0')
        {
            char path_title[80];
            snprintf(path_title, sizeof(path_title), "%s", title);
            for (char *p = path_title; *p; p++)
            {
                *p = *p == ' ' ? '_' : *p;
            }
            url_encode(path_title, encoded, sizeof(encoded));
            snprintf(url, 512, "https://it.wikipedia.org/api/rest_v1/page/summary/%s", encoded);
            g.used = 0;
            if (http_get(url, collect, &g, NULL) && (root = cJSON_Parse(g.buffer)) != NULL)
            {
                const char *extract = cJSON_GetStringValue(cJSON_GetObjectItem(root, "extract"));
                if (extract != NULL)
                {
                    nw_copy(text, 1200, extract);
                }
                cJSON_Delete(root);
            }
        }
    }
    if (wifi)
    {
        wifi_release();
    }
    nw_lock();
    snprintf(nw_ctx_title, sizeof(nw_ctx_title), "%s", title[0] ? title : "Nessuna voce trovata");
    snprintf(nw_ctx_text, 1200, "%s", text != NULL && text[0] ? text : (wifi ? "Wikipedia non ha una voce per questa ricerca."
                                                                            : "Wi-Fi non raggiungibile."));
    nw_state.phase = NW_IDLE;
    nw_changed();
    nw_unlock();
    free(text);
    free(g.buffer);
    free(url);
}

/* ---------------------------------------------------------------- task */

static void job_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        char query[80];
        int months = 0;
        enum { NONE, ROUND, SEARCH, CONTEXT } job = NONE;
        bool background = false;
        taskENTER_CRITICAL(&s_mux);
        if (s_want_context)
        {
            job = CONTEXT;
            s_want_context = false;
            memcpy(query, s_context_query, sizeof(query));
        }
        else if (s_want_search)
        {
            job = SEARCH;
            s_want_search = false;
            memcpy(query, s_search_query, sizeof(query));
            months = s_search_months;
        }
        else if (s_want_round)
        {
            job = ROUND;
            s_want_round = false;
            background = s_round_background;
        }
        else
        {
            s_running = false;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (job == NONE)
        {
            vTaskDelete(NULL);
            return;
        }
        if (job == ROUND)
        {
            s_cancel = !background && !s_app_open;
            if (!s_cancel)
            {
                s_last_round_us = esp_timer_get_time();
                run_round(background);
            }
            s_cancel = false;
        }
        else if (job == SEARCH)
        {
            static volatile bool no_cancel = false;
            set_status(NW_SEARCHING, 0, "Cerco nell'archivio...");
            nw_run_search(query, months, &no_cancel);
            taskENTER_CRITICAL(&s_mux);
            s_searching = false;
            taskEXIT_CRITICAL(&s_mux);
            set_status(NW_IDLE, 100, NULL);
        }
        else
        {
            run_context(query);
            taskENTER_CRITICAL(&s_mux);
            s_contexting = false;
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

static void want_round(bool background)
{
    taskENTER_CRITICAL(&s_mux);
    s_want_round = true;
    s_round_background = background;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void nw_refresh(void)
{
    want_round(false);
}

void nw_search(const char *query, int months)
{
    taskENTER_CRITICAL(&s_mux);
    nw_copy(s_search_query, sizeof(s_search_query), query);
    s_search_months = months < 1 ? 1 : months > 12 ? 12 : months;
    s_want_search = true;
    s_searching = true;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

bool nw_search_running(void)
{
    return s_searching;
}

void nw_context(const char *query)
{
    taskENTER_CRITICAL(&s_mux);
    nw_copy(s_context_query, sizeof(s_context_query), query);
    s_want_context = true;
    s_contexting = true;
    taskEXIT_CRITICAL(&s_mux);
    nw_lock();
    nw_ctx_title[0] = '\0';
    nw_ctx_text[0] = '\0';
    nw_unlock();
    kick();
}

bool nw_context_running(void)
{
    return s_contexting;
}

void nw_app_open(bool open)
{
    s_app_open = open;
    if (!open)
    {
        s_cancel = true;   /* a foreground round in progress stops at the next chunk */
        return;
    }
    s_cancel = false;
    nw_status_t st;
    nw_status(&st);
    time_t now = time(NULL);
    if (st.fetched == 0 || now < 1700000000 || now - st.fetched > FRESH_MINUTES * 60)
    {
        want_round(false);
    }
}

void nw_background_tick(void)
{
    nw_settings_t s;
    nw_settings(&s);
    int64_t now = esp_timer_get_time();
    if (s_app_open || s_running || s.every_hours == 0 || now < BOOT_QUIET_US ||
        (s_last_round_us != 0 && now - s_last_round_us < (int64_t)s.every_hours * 3600 * 1000000))
    {
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < BACKGROUND_FREE)
    {
        s_last_round_us = now;   /* not now (e.g. Bluetooth on): next slot */
        nw_lock();
        nw_state.background_ok = false;
        nw_unlock();
        return;
    }
    want_round(true);
}

esp_err_t nw_start(void)
{
    return nw_store_init();
}
