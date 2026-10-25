/*
 * News data (see news.h): items, stories, sources, archive, trends.
 *
 * Everything big lives in PSRAM. The job task (fetch.c) writes under the
 * lock; the UI and the dashboard read under it, briefly.
 */
#include "news.h"
#include "news_internal.h"

#include "services/storage.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "news";

#define ITEMS_MAGIC  0x4E575331u   /* "NWS1" */
#define TERM_SLOTS   4096          /* trend hash table */
#define DAY_TERMS    60            /* keywords kept per day file */

static SemaphoreHandle_t s_lock;
nw_item_t *nw_item_table;            /* NW_ITEMS, PSRAM */
int nw_item_count;
static nw_story_t *s_stories;        /* NW_STORIES, PSRAM */
static int s_story_count;
static nw_topic_t s_topics[NW_TOPICS];
static int s_topic_count;
static nw_feed_t *s_feeds;           /* NW_FEEDS, PSRAM */
static int s_feed_count;
static nw_settings_t s_settings = {.every_hours = 2, .per_feed = 25};
nw_status_t nw_state;
static nw_term_t *s_trends;          /* NW_PERIODS * NW_TERMS, PSRAM */
static int s_trend_count[NW_PERIODS];
nw_hit_t *nw_hits;                   /* NW_HITS, PSRAM */
int nw_hit_count;
char nw_query[80];
char nw_ctx_title[80];
char *nw_ctx_text;                   /* 1200, PSRAM */

void nw_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void nw_unlock(void) { xSemaphoreGive(s_lock); }
const nw_item_t *nw_items(int *count) { *count = nw_item_count; return nw_item_table; }
const nw_story_t *nw_stories(int *count) { *count = s_story_count; return s_stories; }
const nw_topic_t *nw_topics(int *count) { *count = s_topic_count; return s_topics; }
const nw_feed_t *nw_feeds(int *count) { *count = s_feed_count; return s_feeds; }
nw_feed_t *nw_feed_table(void) { return s_feeds; }
int nw_feed_total(void) { return s_feed_count; }

const nw_term_t *nw_trends(nw_period_t period, int *count)
{
    *count = s_trend_count[period];
    return &s_trends[period * NW_TERMS];
}

const nw_hit_t *nw_search_result(int *count, char *query, size_t size)
{
    *count = nw_hit_count;
    snprintf(query, size, "%s", nw_query);
    return nw_hits;
}

void nw_context_result(char *title, size_t title_size, char *text, size_t text_size)
{
    snprintf(title, title_size, "%s", nw_ctx_title);
    snprintf(text, text_size, "%s", nw_ctx_text != NULL ? nw_ctx_text : "");
}

void nw_status(nw_status_t *out)
{
    nw_lock();
    *out = nw_state;
    nw_unlock();
}

void nw_settings(nw_settings_t *out)
{
    nw_lock();
    *out = s_settings;
    nw_unlock();
}

/* Locked by the caller. */
void nw_changed(void)
{
    nw_state.revision++;
    nw_state.items = nw_item_count;
    nw_state.stories = s_story_count;
}

bool nw_path(const char *name, char *out, size_t size)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/news", root);
    mkdir(folder, 0775);
    snprintf(out, size, "%s/%s", folder, name);
    return true;
}

void nw_time_text(int64_t when, char *out, size_t size)
{
    static const char *const months[] = {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"};
    if (when <= 0)
    {
        snprintf(out, size, "–");
        return;
    }
    time_t t = (time_t)when, now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
    {
        snprintf(out, size, "%02d:%02d", a.tm_hour, a.tm_min);
    }
    else
    {
        snprintf(out, size, "%d %s, %02d:%02d", a.tm_mday, months[a.tm_mon], a.tm_hour, a.tm_min);
    }
}

/* ------------------------------------------------------------ sources */

typedef struct
{
    const char *name;
    uint32_t color;
    bool enabled;
} default_topic_t;

typedef struct
{
    const char *name;
    const char *url;
    uint8_t topic;
} default_feed_t;

static const default_topic_t s_default_topics[] = {
    {"Prima pagina", 0xFF6B5B, true},     {"Politica", 0x5B8CFF, true},       {"Mondo e guerre", 0xFF9F0A, true},
    {"Cronaca", 0x98A2B3, true},          {"Tecnologia", 0x64D2FF, true},     {"Salute e medicina", 0x30D158, true},
    {"Scienza e fisica", 0xBF5AF2, true}, {"Spazio", 0x7D7AFF, true},         {"Storia e cultura", 0xFFD60A, true},
    {"Cinema", 0xFF375F, true},           {"TV", 0xC9A27A, true},             {"Economia", 0x34C7A9, false},
};

static const default_feed_t s_default_feeds[] = {
    {"ANSA Top", "https://www.ansa.it/sito/notizie/topnews/topnews_rss.xml", 0},
    {"Corriere", "https://www.corriere.it/rss/homepage.xml", 0},
    {"Repubblica", "https://www.repubblica.it/rss/homepage/rss2.0.xml", 0},
    {"ANSA Politica", "https://www.ansa.it/sito/notizie/politica/politica_rss.xml", 1},
    {"ANSA Mondo", "https://www.ansa.it/sito/notizie/mondo/mondo_rss.xml", 2},
    {"ANSA Cronaca", "https://www.ansa.it/sito/notizie/cronaca/cronaca_rss.xml", 3},
    {"ANSA Tecnologia", "https://www.ansa.it/sito/notizie/tecnologia/tecnologia_rss.xml", 4},
    {"Wired Italia", "https://www.wired.it/feed/rss", 4},
    {"ANSA Salute", "https://www.ansa.it/canale_saluteebenessere/notizie/saluteebenessere_rss.xml", 5},
    {"ANSA Scienza", "https://www.ansa.it/canale_scienza_tecnica/notizie/scienzaetecnica_rss.xml", 6},
    {"Galileo", "https://www.galileonet.it/feed/", 6},
    {"Il Post Scienza", "https://www.ilpost.it/scienza/feed/", 6},
    {"INAF", "https://www.media.inaf.it/feed/", 7},
    {"Astronomia.com", "https://www.astronomia.com/feed/", 7},
    {"ANSA Cultura", "https://www.ansa.it/sito/notizie/cultura/cultura_rss.xml", 8},
    {"Focus", "https://www.focus.it/rss", 8},
    {"Vanilla Magazine", "https://www.vanillamagazine.it/feed/", 8},
    {"ANSA Cinema", "https://www.ansa.it/sito/notizie/cultura/cinema/cinema_rss.xml", 9},
    {"TVBlog", "https://www.tvblog.it/feed", 10},
    {"SuperGuidaTV", "https://www.superguidatv.it/feed/", 10},
    {"ANSA Economia", "https://www.ansa.it/sito/notizie/economia/economia_rss.xml", 11},
};

static void default_sources(void)
{
    s_topic_count = (int)(sizeof(s_default_topics) / sizeof(s_default_topics[0]));
    for (int i = 0; i < s_topic_count; i++)
    {
        snprintf(s_topics[i].name, sizeof(s_topics[i].name), "%s", s_default_topics[i].name);
        s_topics[i].color = s_default_topics[i].color;
        s_topics[i].enabled = s_default_topics[i].enabled;
    }
    s_feed_count = (int)(sizeof(s_default_feeds) / sizeof(s_default_feeds[0]));
    for (int i = 0; i < s_feed_count; i++)
    {
        memset(&s_feeds[i], 0, sizeof(s_feeds[i]));
        snprintf(s_feeds[i].name, sizeof(s_feeds[i].name), "%s", s_default_feeds[i].name);
        snprintf(s_feeds[i].url, sizeof(s_feeds[i].url), "%s", s_default_feeds[i].url);
        s_feeds[i].topic = s_default_feeds[i].topic;
        s_feeds[i].enabled = true;
    }
}

/* Locked. */
static void save_sources(void)
{
    char path[64];
    if (!nw_path("sources.json", path, sizeof(path)))
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "every_hours", s_settings.every_hours);
    cJSON_AddNumberToObject(root, "per_feed", s_settings.per_feed);
    cJSON *topics = cJSON_AddArrayToObject(root, "topics");
    for (int i = 0; i < s_topic_count; i++)
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", s_topics[i].name);
        cJSON_AddNumberToObject(t, "color", s_topics[i].color);
        cJSON_AddBoolToObject(t, "on", s_topics[i].enabled);
        cJSON_AddItemToArray(topics, t);
    }
    cJSON *feeds = cJSON_AddArrayToObject(root, "feeds");
    for (int i = 0; i < s_feed_count; i++)
    {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", s_feeds[i].name);
        cJSON_AddStringToObject(f, "url", s_feeds[i].url);
        cJSON_AddNumberToObject(f, "topic", s_feeds[i].topic);
        cJSON_AddBoolToObject(f, "on", s_feeds[i].enabled);
        cJSON_AddItemToArray(feeds, f);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL)
    {
        return;
    }
    char temp[72];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (f != NULL)
    {
        bool ok = fputs(text, f) >= 0;
        ok &= fclose(f) == 0;
        if (ok)
        {
            unlink(path);
            rename(temp, path);
        }
    }
    cJSON_free(text);
}

static char *read_file(const char *path, size_t limit)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        return NULL;
    }
    char *text = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM);
    size_t n = text != NULL ? fread(text, 1, limit, f) : 0;
    fclose(f);
    if (text != NULL)
    {
        text[n] = '\0';
    }
    return text;
}

static void load_sources(void)
{
    default_sources();
    char path[64];
    if (!nw_path("sources.json", path, sizeof(path)))
    {
        return;
    }
    char *text = read_file(path, 24 * 1024);
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (root == NULL)
    {
        return;
    }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "every_hours")) != NULL && cJSON_IsNumber(v))
    {
        s_settings.every_hours = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "per_feed")) != NULL && cJSON_IsNumber(v))
    {
        s_settings.per_feed = (uint8_t)v->valueint;
    }
    cJSON *topics = cJSON_GetObjectItem(root, "topics");
    cJSON *feeds = cJSON_GetObjectItem(root, "feeds");
    if (cJSON_IsArray(topics) && cJSON_GetArraySize(topics) > 0)
    {
        s_topic_count = 0;
        cJSON *t;
        cJSON_ArrayForEach(t, topics)
        {
            if (s_topic_count == NW_TOPICS)
            {
                break;
            }
            nw_topic_t *o = &s_topics[s_topic_count++];
            snprintf(o->name, sizeof(o->name), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(t, "name")) ?: "?");
            o->color = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "color"));
            o->enabled = cJSON_IsTrue(cJSON_GetObjectItem(t, "on"));
        }
    }
    if (cJSON_IsArray(feeds))
    {
        s_feed_count = 0;
        cJSON *f;
        cJSON_ArrayForEach(f, feeds)
        {
            if (s_feed_count == NW_FEEDS)
            {
                break;
            }
            nw_feed_t *o = &s_feeds[s_feed_count];
            memset(o, 0, sizeof(*o));
            snprintf(o->name, sizeof(o->name), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(f, "name")) ?: "?");
            snprintf(o->url, sizeof(o->url), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(f, "url")) ?: "");
            o->topic = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(f, "topic"));
            o->enabled = cJSON_IsTrue(cJSON_GetObjectItem(f, "on"));
            if (o->url[0] != '\0' && o->topic < s_topic_count)
            {
                s_feed_count++;
            }
        }
    }
    cJSON_Delete(root);
}

bool nw_set_sources(const nw_topic_t *topics, int topic_count, const nw_feed_t *feeds, int feed_count)
{
    if (topic_count < 1 || topic_count > NW_TOPICS || feed_count < 0 || feed_count > NW_FEEDS)
    {
        return false;
    }
    nw_lock();
    memcpy(s_topics, topics, sizeof(nw_topic_t) * (size_t)topic_count);
    s_topic_count = topic_count;
    for (int i = 0; i < feed_count; i++)
    {
        s_feeds[i] = feeds[i];
        if (s_feeds[i].topic >= topic_count)
        {
            s_feeds[i].topic = 0;
        }
    }
    s_feed_count = feed_count;
    /* Items of a removed topic move to the first one until they age out. */
    for (int i = 0; i < nw_item_count; i++)
    {
        if (nw_item_table[i].topic >= topic_count)
        {
            nw_item_table[i].topic = 0;
        }
        if (nw_item_table[i].feed >= feed_count)
        {
            nw_item_table[i].feed = 0;
        }
    }
    save_sources();
    nw_changed();
    nw_unlock();
    return true;
}

void nw_set_settings(const nw_settings_t *settings)
{
    nw_lock();
    s_settings = *settings;
    if (s_settings.every_hours != 0 && s_settings.every_hours != 1 && s_settings.every_hours != 2 &&
        s_settings.every_hours != 4 && s_settings.every_hours != 8)
    {
        s_settings.every_hours = 2;
    }
    s_settings.per_feed = s_settings.per_feed < 10 ? 10 : s_settings.per_feed > 40 ? 40 : s_settings.per_feed;
    save_sources();
    nw_changed();
    nw_unlock();
}

/* -------------------------------------------------------------- items */

/* Locked. Newest first by insertion sort (rounds add few items). */
static void sort_items(void)
{
    for (int i = 1; i < nw_item_count; i++)
    {
        if (nw_item_table[i].when <= nw_item_table[i - 1].when)
        {
            continue;
        }
        nw_item_t moving = nw_item_table[i];
        int j = i - 1;
        while (j >= 0 && nw_item_table[j].when < moving.when)
        {
            nw_item_table[j + 1] = nw_item_table[j];
            j--;
        }
        nw_item_table[j + 1] = moving;
    }
}

/* Locked. Drops what is older than NW_KEEP_HOURS (with a valid clock). */
static void prune(void)
{
    int64_t now = time(NULL);
    if (now < 1700000000)
    {
        return;
    }
    int64_t limit = now - NW_KEEP_HOURS * 3600;
    int keep = 0;
    for (int i = 0; i < nw_item_count; i++)
    {
        if (nw_item_table[i].when >= limit)
        {
            if (keep != i)
            {
                nw_item_table[keep] = nw_item_table[i];
            }
            keep++;
        }
    }
    nw_item_count = keep;
}

/* Locked. True if new (then it is added, possibly replacing the oldest). */
bool nw_add_item(const nw_item_t *item)
{
    for (int i = 0; i < nw_item_count; i++)
    {
        if (nw_item_table[i].id == item->id)
        {
            return false;
        }
    }
    if (nw_item_count < NW_ITEMS)
    {
        nw_item_table[nw_item_count++] = *item;
    }
    else
    {
        /* Full: replace the oldest if the new one is newer. */
        int oldest = 0;
        for (int i = 1; i < nw_item_count; i++)
        {
            if (nw_item_table[i].when < nw_item_table[oldest].when)
            {
                oldest = i;
            }
        }
        if (nw_item_table[oldest].when >= item->when)
        {
            return false;
        }
        nw_item_table[oldest] = *item;
    }
    return true;
}

int nw_story_items(int story, int16_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < nw_item_count && n < max; i++)
    {
        if (nw_item_table[i].story == story)
        {
            out[n++] = (int16_t)i;
        }
    }
    return n;
}

int nw_topic_items(int topic, int16_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < nw_item_count && n < max; i++)
    {
        if (nw_item_table[i].topic == topic)
        {
            out[n++] = (int16_t)i;
        }
    }
    return n;
}

void nw_save_items(void)
{
    char path[64], temp[72];
    if (!nw_path("items.bin", path, sizeof(path)))
    {
        return;
    }
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "wb");
    if (f == NULL)
    {
        return;
    }
    uint32_t header[2] = {ITEMS_MAGIC, (uint32_t)nw_item_count};
    bool ok = fwrite(header, sizeof(header), 1, f) == 1;
    ok &= nw_item_count == 0 || fwrite(nw_item_table, sizeof(nw_item_t), (size_t)nw_item_count, f) == (size_t)nw_item_count;
    ok &= fclose(f) == 0;
    if (ok)
    {
        unlink(path);
        rename(temp, path);
    }
}

static void load_items(void)
{
    char path[64];
    if (!nw_path("items.bin", path, sizeof(path)))
    {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return;
    }
    uint32_t header[2];
    if (fread(header, sizeof(header), 1, f) == 1 && header[0] == ITEMS_MAGIC && header[1] <= NW_ITEMS)
    {
        nw_item_count = (int)fread(nw_item_table, sizeof(nw_item_t), header[1], f);
    }
    fclose(f);
    prune();
    sort_items();
}

/* ------------------------------------------------------------ stories */

static int shared_keys(const nw_item_t *a, const nw_item_t *b)
{
    int n = 0;
    for (int i = 0; i < a->key_count; i++)
    {
        for (int j = 0; j < b->key_count; j++)
        {
            n += a->keys[i] == b->keys[j];
        }
    }
    return n;
}

/* Locked. Same story: 2+ shared keywords covering half of the shorter headline. */
void nw_cluster(void)
{
    sort_items();
    s_story_count = 0;
    int64_t now = time(NULL);
    for (int i = 0; i < nw_item_count; i++)
    {
        nw_item_t *it = &nw_item_table[i];
        it->story = -1;
        for (int s = 0; s < s_story_count && it->story < 0; s++)
        {
            /* Compare with the story's lead and its first follower (enough, cheap). */
            const nw_item_t *lead = &nw_item_table[s_stories[s].lead];
            int shared = shared_keys(it, lead);
            int shorter = it->key_count < lead->key_count ? it->key_count : lead->key_count;
            if (shared >= 2 && shared * 2 >= shorter)
            {
                it->story = (int16_t)s;
            }
        }
        if (it->story < 0 && s_story_count < NW_STORIES)
        {
            nw_story_t *st = &s_stories[s_story_count];
            memset(st, 0, sizeof(*st));
            st->lead = (int16_t)i;
            st->topic = it->topic;
            st->first = st->last = it->when;
            it->story = (int16_t)s_story_count++;
        }
    }
    /* Counts, distinct sources, time span; the lead is the oldest item of the
     * first source that told it (usually the most complete headline). */
    for (int s = 0; s < s_story_count; s++)
    {
        nw_story_t *st = &s_stories[s];
        uint32_t feeds = 0;
        st->count = 0;
        for (int i = 0; i < nw_item_count; i++)
        {
            const nw_item_t *it = &nw_item_table[i];
            if (it->story != s)
            {
                continue;
            }
            st->count++;
            feeds |= 1u << (it->feed % 32);
            st->first = it->when < st->first ? it->when : st->first;
            st->last = it->when > st->last ? it->when : st->last;
        }
        st->sources = (uint8_t)__builtin_popcount(feeds);
        float hours = now > 1700000000 ? (float)(now - st->last) / 3600.0f : 0;
        hours = hours < 0 ? 0 : hours;
        /* Many sources weigh most; a story fades over a day. */
        st->score = (st->sources * 3.0f + st->count) / (1.0f + hours / 12.0f);
    }
    /* Best first: sort indices, then remap the items' story numbers. */
    int16_t *order = heap_caps_malloc(sizeof(int16_t) * NW_STORIES, MALLOC_CAP_SPIRAM);
    int16_t *rank = heap_caps_malloc(sizeof(int16_t) * NW_STORIES, MALLOC_CAP_SPIRAM);
    nw_story_t *sorted = heap_caps_malloc(sizeof(nw_story_t) * NW_STORIES, MALLOC_CAP_SPIRAM);
    if (order != NULL && rank != NULL && sorted != NULL)
    {
        for (int s = 0; s < s_story_count; s++)
        {
            order[s] = (int16_t)s;
        }
        for (int a = 1; a < s_story_count; a++)
        {
            int16_t moving = order[a];
            int b = a - 1;
            while (b >= 0 && s_stories[order[b]].score < s_stories[moving].score)
            {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = moving;
        }
        for (int s = 0; s < s_story_count; s++)
        {
            sorted[s] = s_stories[order[s]];
            rank[order[s]] = (int16_t)s;
        }
        memcpy(s_stories, sorted, sizeof(nw_story_t) * (size_t)s_story_count);
        for (int i = 0; i < nw_item_count; i++)
        {
            if (nw_item_table[i].story >= 0)
            {
                nw_item_table[i].story = rank[nw_item_table[i].story];
            }
        }
    }
    free(order);
    free(rank);
    free(sorted);
    nw_changed();
}

/* ------------------------------------------------------------ archive */

/* Appends the new items to news/YYYY-MM.tsv: when, topic, source, title, link. */
void nw_archive(const nw_item_t *items, int count)
{
    FILE *f = NULL;
    char current[32] = "";
    for (int i = 0; i < count; i++)
    {
        const nw_item_t *it = &items[i];
        time_t t = (time_t)it->when;
        struct tm tm;
        localtime_r(&t, &tm);
        char month[32], path[64];
        snprintf(month, sizeof(month), "%04d-%02d", tm.tm_year + 1900, (tm.tm_mon + 1) % 100);
        if (strcmp(month, current) != 0)
        {
            if (f != NULL)
            {
                fclose(f);
            }
            snprintf(current, sizeof(current), "%s", month);
            char name[40];
            snprintf(name, sizeof(name), "%s.tsv", month);
            f = nw_path(name, path, sizeof(path)) ? fopen(path, "a") : NULL;
        }
        if (f == NULL)
        {
            continue;
        }
        const char *source = it->feed < s_feed_count ? s_feeds[it->feed].name : "";
        fprintf(f, "%lld\t%u\t%s\t%s\t%s\n", (long long)it->when, it->topic, source, it->title, it->link);
    }
    if (f != NULL)
    {
        fclose(f);
    }
}

/* Small storage (no card): only the last two months of archive stay. */
void nw_archive_trim(bool small_storage)
{
    if (!small_storage)
    {
        return;
    }
    char folder[64];
    if (!nw_path("", folder, sizeof(folder)))
    {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int keep_from = (tm.tm_year + 1900) * 12 + tm.tm_mon - 1;   /* last month and this one */
    DIR *dir = opendir(folder);
    if (dir == NULL)
    {
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
    {
        int y, m;
        if (strlen(e->d_name) == 11 && sscanf(e->d_name, "%4d-%2d.tsv", &y, &m) == 2 && y * 12 + m - 1 < keep_from)
        {
            char path[320];
            snprintf(path, sizeof(path), "%s%s", folder, e->d_name);
            unlink(path);
        }
    }
    closedir(dir);
}

/* --------------------------------------------------------- day terms */

typedef struct
{
    uint32_t hash;
    char word[NW_WORD];
    uint16_t count;
    uint16_t series[12];
} slot_t;

static slot_t *table_add(slot_t *table, uint32_t hash, const char *word, int count, int slice)
{
    if (hash == 0)
    {
        hash = 1;
    }
    uint32_t i = hash & (TERM_SLOTS - 1);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) & (TERM_SLOTS - 1))
    {
        if (table[i].hash == 0)
        {
            table[i].hash = hash;
            nw_copy(table[i].word, sizeof(table[i].word), word);
        }
        if (table[i].hash == hash)
        {
            table[i].count = (uint16_t)(table[i].count + count > 65000 ? 65000 : table[i].count + count);
            if (slice >= 0 && slice < 12)
            {
                table[i].series[slice] = (uint16_t)(table[i].series[slice] + count > 65000 ? 65000 : table[i].series[slice] + count);
            }
            return &table[i];
        }
    }
    return NULL;   /* table full: rare words lost */
}

static int compare_slots(const void *a, const void *b)
{
    return (int)((const slot_t *)b)->count - (int)((const slot_t *)a)->count;
}

/* Top `max` of the table into terms (the table is sorted in place). */
static int table_top(slot_t *table, nw_term_t *terms, int max)
{
    qsort(table, TERM_SLOTS, sizeof(slot_t), compare_slots);
    int n = 0;
    for (int i = 0; i < TERM_SLOTS && n < max && table[i].count > 0; i++)
    {
        nw_copy(terms[n].word, sizeof(terms[n].word), table[i].word);
        terms[n].count = table[i].count;
        memcpy(terms[n].series, table[i].series, sizeof(terms[n].series));
        n++;
    }
    return n;
}

static void day_name(int64_t day_start, char *out, size_t size)
{
    time_t t = (time_t)day_start;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, size, "d/%04d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* Local midnight of the day containing `when`. */
static int64_t day_start_of(int64_t when)
{
    time_t t = (time_t)when;
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

/* Not locked: the job task owns the analysis. Items copied under the lock. */
void nw_write_day_terms(const nw_item_t *items, int count)
{
    int64_t now = time(NULL);
    if (now < 1700000000)
    {
        return;
    }
    slot_t *table = heap_caps_calloc(TERM_SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    if (table == NULL)
    {
        return;
    }
    char folder[64];
    if (nw_path("d", folder, sizeof(folder)))
    {
        mkdir(folder, 0775);
    }
    /* Today and yesterday (completed by late items). */
    for (int back = 0; back < 2; back++)
    {
        int64_t start = day_start_of(now - back * 86400);
        memset(table, 0, sizeof(slot_t) * TERM_SLOTS);
        int used = 0;
        for (int i = 0; i < count; i++)
        {
            if (items[i].when < start || items[i].when >= start + 86400)
            {
                continue;
            }
            nw_key_t keys[NW_KEY_MAX];
            int n = nw_keywords(items[i].title, keys, NW_KEY_MAX);
            for (int k = 0; k < n; k++)
            {
                table_add(table, keys[k].hash, keys[k].word, 1, -1);
            }
            used++;
        }
        if (used == 0)
        {
            continue;
        }
        nw_term_t *top = heap_caps_malloc(sizeof(nw_term_t) * DAY_TERMS, MALLOC_CAP_SPIRAM);
        int n = top != NULL ? table_top(table, top, DAY_TERMS) : 0;
        char name[24], path[64];
        day_name(start, name, sizeof(name));
        FILE *f = n > 0 && nw_path(name, path, sizeof(path)) ? fopen(path, "w") : NULL;
        if (f != NULL)
        {
            for (int i = 0; i < n; i++)
            {
                fprintf(f, "%u\t%s\n", top[i].count, top[i].word);
            }
            fclose(f);
        }
        free(top);
    }
    free(table);
}

/* Trends for every period, into the cache. Day: from the items (2-hour
 * slices); week, month, year: from the day files. */
void nw_compute_trends(const nw_item_t *items, int count)
{
    int64_t now = time(NULL);
    if (now < 1700000000)
    {
        return;
    }
    slot_t *table = heap_caps_calloc(TERM_SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    nw_term_t *out = heap_caps_malloc(sizeof(nw_term_t) * NW_TERMS, MALLOC_CAP_SPIRAM);
    if (table == NULL || out == NULL)
    {
        free(table);
        free(out);
        return;
    }
    static const int days[NW_PERIODS] = {1, 7, 30, 365};
    for (int p = 0; p < NW_PERIODS; p++)
    {
        memset(table, 0, sizeof(slot_t) * TERM_SLOTS);
        if (p == NW_DAY)
        {
            for (int i = 0; i < count; i++)
            {
                int64_t age = now - items[i].when;
                if (age < 0 || age >= 86400)
                {
                    continue;
                }
                int slice = 11 - (int)(age / 7200);
                nw_key_t keys[NW_KEY_MAX];
                int n = nw_keywords(items[i].title, keys, NW_KEY_MAX);
                for (int k = 0; k < n; k++)
                {
                    table_add(table, keys[k].hash, keys[k].word, 1, slice);
                }
            }
        }
        else
        {
            int64_t today = day_start_of(now);
            for (int back = 0; back < days[p]; back++)
            {
                char name[24], path[64];
                day_name(today - (int64_t)back * 86400 + 3600, name, sizeof(name));
                FILE *f = nw_path(name, path, sizeof(path)) ? fopen(path, "r") : NULL;
                if (f == NULL)
                {
                    continue;
                }
                int slice = 11 - back * 12 / days[p];
                char line[64];
                while (fgets(line, sizeof(line), f) != NULL)
                {
                    char *tab = strchr(line, '\t');
                    if (tab == NULL)
                    {
                        continue;
                    }
                    *tab = '\0';
                    char *word = tab + 1;
                    word[strcspn(word, "\r\n")] = '\0';
                    table_add(table, nw_stem_hash(word), word, atoi(line), slice);
                }
                fclose(f);
            }
        }
        int n = table_top(table, out, NW_TERMS);
        nw_lock();
        memcpy(&s_trends[p * NW_TERMS], out, sizeof(nw_term_t) * (size_t)n);
        s_trend_count[p] = n;
        nw_changed();
        nw_unlock();
    }
    free(table);
    free(out);
}

/* -------------------------------------------------------------- search */

/* Archive search, newest month first. Not locked (job task); results copied in. */
void nw_run_search(const char *query, int months, volatile bool *cancel)
{
    char needles[6][8];
    int needle_count = nw_needles(query, needles, 6);
    nw_hit_t *hits = heap_caps_calloc(NW_HITS, sizeof(nw_hit_t), MALLOC_CAP_SPIRAM);
    char *line = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char *folded = heap_caps_malloc(NW_TITLE + 8, MALLOC_CAP_SPIRAM);
    int found = 0;
    if (hits != NULL && line != NULL && folded != NULL && needle_count > 0)
    {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int month_index = (tm.tm_year + 1900) * 12 + tm.tm_mon;
        for (int back = 0; back < months && found < NW_HITS && !*cancel; back++)
        {
            int mi = month_index - back;
            char name[24], path[64];
            snprintf(name, sizeof(name), "%04d-%02d.tsv", mi / 12, mi % 12 + 1);
            FILE *f = nw_path(name, path, sizeof(path)) ? fopen(path, "r") : NULL;
            if (f == NULL)
            {
                continue;
            }
            /* Lines are oldest first: collect this month's matches, then keep the newest. */
            int month_first = found;
            while (fgets(line, 1024, f) != NULL && !*cancel)
            {
                char *fields[5] = {line};
                int n = 1;
                for (char *p = line; *p && n < 5; p++)
                {
                    if (*p == '\t')
                    {
                        *p = '\0';
                        fields[n++] = p + 1;
                    }
                }
                if (n < 5)
                {
                    continue;
                }
                fields[4][strcspn(fields[4], "\r\n")] = '\0';
                nw_fold(fields[3], folded, NW_TITLE + 8);
                if (!nw_match(folded, needles, needle_count))
                {
                    continue;
                }
                /* Ring within this month: when full, drop the oldest of the month. */
                int slot;
                if (found < NW_HITS)
                {
                    slot = found++;
                }
                else
                {
                    memmove(&hits[month_first], &hits[month_first + 1], sizeof(nw_hit_t) * (size_t)(NW_HITS - month_first - 1));
                    slot = NW_HITS - 1;
                }
                nw_hit_t *h = &hits[slot];
                h->when = atoll(fields[0]);
                h->topic = (uint8_t)atoi(fields[1]);
                nw_copy(h->source, sizeof(h->source), fields[2]);
                nw_copy(h->title, sizeof(h->title), fields[3]);
                nw_copy(h->link, sizeof(h->link), fields[4]);
            }
            fclose(f);
            /* Newest first inside the month. */
            for (int a = month_first, b = found - 1; a < b; a++, b--)
            {
                nw_hit_t t = hits[a];
                hits[a] = hits[b];
                hits[b] = t;
            }
        }
    }
    nw_lock();
    if (hits != NULL)
    {
        memcpy(nw_hits, hits, sizeof(nw_hit_t) * (size_t)found);
    }
    nw_hit_count = found;
    nw_copy(nw_query, sizeof(nw_query), query);
    nw_changed();
    nw_unlock();
    free(hits);
    free(line);
    free(folded);
}

/* -------------------------------------------------------------- start */

esp_err_t nw_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nw_item_table = heap_caps_calloc(NW_ITEMS, sizeof(nw_item_t), MALLOC_CAP_SPIRAM);
    s_stories = heap_caps_calloc(NW_STORIES, sizeof(nw_story_t), MALLOC_CAP_SPIRAM);
    s_feeds = heap_caps_calloc(NW_FEEDS, sizeof(nw_feed_t), MALLOC_CAP_SPIRAM);
    s_trends = heap_caps_calloc(NW_PERIODS * NW_TERMS, sizeof(nw_term_t), MALLOC_CAP_SPIRAM);
    nw_hits = heap_caps_calloc(NW_HITS, sizeof(nw_hit_t), MALLOC_CAP_SPIRAM);
    nw_ctx_text = heap_caps_calloc(1, 1200, MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || nw_item_table == NULL || s_stories == NULL || s_feeds == NULL || s_trends == NULL ||
        nw_hits == NULL || nw_ctx_text == NULL)
    {
        if (s_lock != NULL) vSemaphoreDelete(s_lock);
        s_lock = NULL;
        free(nw_item_table); nw_item_table = NULL;
        free(s_stories); s_stories = NULL;
        free(s_feeds); s_feeds = NULL;
        free(s_trends); s_trends = NULL;
        free(nw_hits); nw_hits = NULL;
        free(nw_ctx_text); nw_ctx_text = NULL;
        return ESP_ERR_NO_MEM;
    }
    load_sources();
    load_items();
    nw_lock();
    nw_cluster();
    snprintf(nw_state.message, sizeof(nw_state.message), "%s",
             nw_item_count ? "Notizie dalla memoria" : "Prima lettura dei giornali...");
    nw_unlock();
    ESP_LOGI(TAG, "%d items, %d stories, %d feeds", nw_item_count, s_story_count, s_feed_count);
    return ESP_OK;
}
