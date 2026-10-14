/*
 * News dashboard API (appkit): served at http://<watch>/news/ while the app
 * is open and its Dashboard page has been shown.
 *
 *   GET  /api/state            status, settings, sections and feeds
 *   GET  /api/front            the best stories with every source's headline
 *   GET  /api/section?t=N      a section's articles
 *   GET  /api/trends?p=0..3    top words of the day/week/month/year + series
 *   GET  /api/search           the last archive search (running?, hits)
 *   POST /api/search           {q, months}: start a search
 *   POST /api/sources          {topics, feeds}: the paper's sections and feeds
 *   POST /api/settings         {every_hours, per_feed}
 *   POST /api/refresh          a round now
 *   GET  /api/archive?m=YYYY-MM  the month's archive as a TSV download
 *
 * The Wikipedia background is fetched by the browser itself (it.wikipedia.org).
 */
#include "news.h"

#include "appkit.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char news_page_start[] asm("_binary_news_dashboard_html_start");
extern const char news_page_end[] asm("_binary_news_dashboard_html_end");

#define GUARD()                                       \
    do                                                \
    {                                                 \
        if (!appkit_authorized(request))              \
        {                                             \
            return appkit_unauthorized(request);      \
        }                                             \
    } while (0)

static void add_item(cJSON *array, const nw_item_t *it, const nw_feed_t *feeds, int feed_count, bool summary)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", it->title);
    cJSON_AddStringToObject(o, "link", it->link);
    if (summary)
    {
        cJSON_AddStringToObject(o, "summary", it->summary);
    }
    cJSON_AddNumberToObject(o, "when", (double)it->when);
    cJSON_AddNumberToObject(o, "topic", it->topic);
    cJSON_AddStringToObject(o, "source", it->feed < feed_count ? feeds[it->feed].name : "");
    cJSON_AddItemToArray(array, o);
}

static esp_err_t state(httpd_req_t *request)
{
    GUARD();
    nw_status_t st;
    nw_status(&st);
    nw_settings_t s;
    nw_settings(&s);
    cJSON *out = cJSON_CreateObject();
    cJSON *status = cJSON_AddObjectToObject(out, "status");
    cJSON_AddNumberToObject(status, "phase", st.phase);
    cJSON_AddNumberToObject(status, "percent", st.percent);
    cJSON_AddStringToObject(status, "message", st.message);
    cJSON_AddNumberToObject(status, "fetched", (double)st.fetched);
    cJSON_AddNumberToObject(status, "items", st.items);
    cJSON_AddNumberToObject(status, "stories", st.stories);
    cJSON_AddNumberToObject(status, "revision", st.revision);
    cJSON_AddBoolToObject(status, "background_ok", st.background_ok);
    cJSON *set = cJSON_AddObjectToObject(out, "settings");
    cJSON_AddNumberToObject(set, "every_hours", s.every_hours);
    cJSON_AddNumberToObject(set, "per_feed", s.per_feed);
    nw_lock();
    int topic_count, feed_count, item_count;
    const nw_topic_t *topics = nw_topics(&topic_count);
    const nw_feed_t *feeds = nw_feeds(&feed_count);
    const nw_item_t *items = nw_items(&item_count);
    cJSON *t = cJSON_AddArrayToObject(out, "topics");
    for (int i = 0; i < topic_count; i++)
    {
        int n = 0;
        for (int k = 0; k < item_count; k++)
        {
            n += items[k].topic == i;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", topics[i].name);
        cJSON_AddNumberToObject(o, "color", topics[i].color);
        cJSON_AddBoolToObject(o, "on", topics[i].enabled);
        cJSON_AddNumberToObject(o, "items", n);
        cJSON_AddItemToArray(t, o);
    }
    cJSON *f = cJSON_AddArrayToObject(out, "feeds");
    for (int i = 0; i < feed_count; i++)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", feeds[i].name);
        cJSON_AddStringToObject(o, "url", feeds[i].url);
        cJSON_AddNumberToObject(o, "topic", feeds[i].topic);
        cJSON_AddBoolToObject(o, "on", feeds[i].enabled);
        cJSON_AddNumberToObject(o, "count", feeds[i].count);
        cJSON_AddNumberToObject(o, "fetched", (double)feeds[i].fetched);
        cJSON_AddStringToObject(o, "error", feeds[i].error);
        cJSON_AddItemToArray(f, o);
    }
    nw_unlock();
    return appkit_json(request, out);
}

static esp_err_t front(httpd_req_t *request)
{
    GUARD();
    int16_t *members = malloc(sizeof(int16_t) * 16);
    if (members == NULL)
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria occupata");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "stories");
    nw_lock();
    int story_count, item_count, feed_count;
    const nw_story_t *stories = nw_stories(&story_count);
    const nw_item_t *items = nw_items(&item_count);
    const nw_feed_t *feeds = nw_feeds(&feed_count);
    for (int s = 0; s < story_count && s < 48; s++)
    {
        const nw_story_t *st = &stories[s];
        if (st->lead < 0 || st->lead >= item_count)
        {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "sources", st->sources);
        cJSON_AddNumberToObject(o, "count", st->count);
        cJSON_AddNumberToObject(o, "first", (double)st->first);
        cJSON_AddNumberToObject(o, "last", (double)st->last);
        cJSON_AddNumberToObject(o, "score", st->score);
        cJSON *lead = cJSON_AddArrayToObject(o, "items");
        add_item(lead, &items[st->lead], feeds, feed_count, true);
        int n = nw_story_items(s, members, 16);
        for (int i = 0; i < n; i++)
        {
            if (members[i] != st->lead)
            {
                add_item(lead, &items[members[i]], feeds, feed_count, false);
            }
        }
        cJSON_AddItemToArray(list, o);
    }
    nw_unlock();
    free(members);
    return appkit_json(request, out);
}

static esp_err_t section(httpd_req_t *request)
{
    GUARD();
    char value[8];
    appkit_query(request, "t", value, sizeof(value));
    int topic = atoi(value);
    int16_t *list = malloc(sizeof(int16_t) * 120);
    if (list == NULL)
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria occupata");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(out, "items");
    nw_lock();
    int item_count, feed_count;
    const nw_item_t *items = nw_items(&item_count);
    const nw_feed_t *feeds = nw_feeds(&feed_count);
    int n = nw_topic_items(topic, list, 120);
    for (int i = 0; i < n; i++)
    {
        add_item(array, &items[list[i]], feeds, feed_count, true);
    }
    nw_unlock();
    free(list);
    return appkit_json(request, out);
}

static esp_err_t trends(httpd_req_t *request)
{
    GUARD();
    char value[8];
    appkit_query(request, "p", value, sizeof(value));
    int period = atoi(value);
    period = period < 0 || period >= NW_PERIODS ? 0 : period;
    cJSON *out = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(out, "terms");
    nw_lock();
    int count;
    const nw_term_t *terms = nw_trends((nw_period_t)period, &count);
    for (int i = 0; i < count; i++)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "w", terms[i].word);
        cJSON_AddNumberToObject(o, "c", terms[i].count);
        int series[12];
        for (int k = 0; k < 12; k++)
        {
            series[k] = terms[i].series[k];
        }
        cJSON_AddItemToObject(o, "s", cJSON_CreateIntArray(series, 12));
        cJSON_AddItemToArray(array, o);
    }
    nw_unlock();
    return appkit_json(request, out);
}

static esp_err_t search_get(httpd_req_t *request)
{
    GUARD();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "running", nw_search_running());
    cJSON *array = cJSON_AddArrayToObject(out, "hits");
    nw_lock();
    int count;
    char query[80];
    const nw_hit_t *hits = nw_search_result(&count, query, sizeof(query));
    cJSON_AddStringToObject(out, "query", query);
    for (int i = 0; i < count; i++)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "when", (double)hits[i].when);
        cJSON_AddNumberToObject(o, "topic", hits[i].topic);
        cJSON_AddStringToObject(o, "source", hits[i].source);
        cJSON_AddStringToObject(o, "title", hits[i].title);
        cJSON_AddStringToObject(o, "link", hits[i].link);
        cJSON_AddItemToArray(array, o);
    }
    nw_unlock();
    return appkit_json(request, out);
}

static esp_err_t search_post(httpd_req_t *request)
{
    GUARD();
    char *text = appkit_body(request, 512);
    cJSON *in = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(in, "q"));
    if (q == NULL || q[0] == '\0')
    {
        cJSON_Delete(in);
        return appkit_error(request, "400 Bad Request", "Scrivi cosa cercare");
    }
    int months = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(in, "months"));
    nw_search(q, months > 0 ? months : 3);
    cJSON_Delete(in);
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t sources(httpd_req_t *request)
{
    GUARD();
    char *text = appkit_body(request, 16 * 1024);
    cJSON *in = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    cJSON *topics = cJSON_GetObjectItem(in, "topics");
    cJSON *feeds = cJSON_GetObjectItem(in, "feeds");
    nw_topic_t *t = heap_caps_calloc(NW_TOPICS, sizeof(nw_topic_t), MALLOC_CAP_SPIRAM);
    nw_feed_t *f = heap_caps_calloc(NW_FEEDS, sizeof(nw_feed_t), MALLOC_CAP_SPIRAM);
    int tn = 0, fn = 0;
    bool ok = cJSON_IsArray(topics) && cJSON_IsArray(feeds) && t != NULL && f != NULL;
    if (ok)
    {
        cJSON *o;
        cJSON_ArrayForEach(o, topics)
        {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
            if (tn == NW_TOPICS || name == NULL || name[0] == '\0')
            {
                continue;
            }
            nw_copy(t[tn].name, sizeof(t[tn].name), name);
            t[tn].color = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "color"));
            t[tn].enabled = !cJSON_IsFalse(cJSON_GetObjectItem(o, "on"));
            tn++;
        }
        cJSON_ArrayForEach(o, feeds)
        {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(o, "url"));
            if (fn == NW_FEEDS || url == NULL || strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(f[0].url))
            {
                continue;   /* https only: the feeds travel over the internet */
            }
            nw_copy(f[fn].name, sizeof(f[fn].name), name != NULL && name[0] ? name : "Feed");
            nw_copy(f[fn].url, sizeof(f[fn].url), url);
            f[fn].topic = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "topic"));
            f[fn].enabled = !cJSON_IsFalse(cJSON_GetObjectItem(o, "on"));
            fn++;
        }
        ok = nw_set_sources(t, tn, f, fn);
    }
    cJSON_Delete(in);
    free(t);
    free(f);
    if (!ok)
    {
        return appkit_error(request, "400 Bad Request", "Sezioni o feed non validi");
    }
    nw_refresh();
    return state(request);
}

static esp_err_t settings(httpd_req_t *request)
{
    GUARD();
    char *text = appkit_body(request, 512);
    cJSON *in = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    nw_settings_t s;
    nw_settings(&s);
    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "every_hours")) != NULL && cJSON_IsNumber(v))
    {
        s.every_hours = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(in, "per_feed")) != NULL && cJSON_IsNumber(v))
    {
        s.per_feed = (uint8_t)v->valueint;
    }
    cJSON_Delete(in);
    nw_set_settings(&s);
    return state(request);
}

static esp_err_t refresh(httpd_req_t *request)
{
    GUARD();
    nw_refresh();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t archive(httpd_req_t *request)
{
    GUARD();
    char month[12], name[24], path[64];
    appkit_query(request, "m", month, sizeof(month));
    int y = 0, m = 0;
    if (sscanf(month, "%4d-%2d", &y, &m) != 2 || y < 2020 || m < 1 || m > 12)
    {
        return appkit_error(request, "400 Bad Request", "Mese non valido");
    }
    snprintf(name, sizeof(name), "%04d-%02d.tsv", y, m);
    FILE *f = nw_path(name, path, sizeof(path)) ? fopen(path, "r") : NULL;
    if (f == NULL)
    {
        return appkit_error(request, "404 Not Found", "Nessun archivio per quel mese");
    }
    char header[80];
    snprintf(header, sizeof(header), "attachment; filename=\"notizie-%s\"", name);
    httpd_resp_set_type(request, "text/tab-separated-values; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Disposition", header);
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    esp_err_t err = chunk != NULL ? httpd_resp_send_chunk(request, "data\tsezione\tfonte\ttitolo\tlink\n", HTTPD_RESP_USE_STRLEN)
                                  : ESP_ERR_NO_MEM;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, 4096, f)) > 0)
    {
        err = httpd_resp_send_chunk(request, chunk, (ssize_t)n);
    }
    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(request, NULL, 0);
    return err;
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/front", .method = HTTP_GET, .handler = front},
    {.uri = "/api/section", .method = HTTP_GET, .handler = section},
    {.uri = "/api/trends", .method = HTTP_GET, .handler = trends},
    {.uri = "/api/search", .method = HTTP_GET, .handler = search_get},
    {.uri = "/api/search", .method = HTTP_POST, .handler = search_post},
    {.uri = "/api/sources", .method = HTTP_POST, .handler = sources},
    {.uri = "/api/settings", .method = HTTP_POST, .handler = settings},
    {.uri = "/api/refresh", .method = HTTP_POST, .handler = refresh},
    {.uri = "/api/archive", .method = HTTP_GET, .handler = archive},
};

static const appkit_web_config_t s_config = {
    .name = "News",
    .page_start = news_page_start,
    .page_end = news_page_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t news_web_start(void)
{
    return appkit_web_start(&s_config);
}
