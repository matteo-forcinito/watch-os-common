#ifndef NEWS_H
#define NEWS_H

/*
 * News: an Italian newspaper on the wrist, built from public RSS feeds.
 *
 *   topics    sections of the paper (Politica, Mondo e guerre, Tecnologia...),
 *             each fed by one or more feeds (editable in the dashboard)
 *   items     the articles of the last 3 days (PSRAM), cached on the card
 *   stories   items about the same fact grouped together (shared keywords):
 *             a story told by many sources is a big story
 *   archive   every headline ever seen, one line each, a file per month
 *             (news/YYYY-MM.tsv): the memory for "how did we get here"
 *   days      the most used keywords of each day (news/d/YYYYMMDD.txt):
 *             the top of the week, month and year come from them
 *
 * One job at a time runs in a short-lived task (8 KB stack, TLS in PSRAM):
 * a round of downloads, an archive search or a Wikipedia background. The
 * task ends when there is nothing to do, so a closed app costs no RAM; the
 * background rounds (every few hours) run only with enough free memory.
 */

#include "text.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define NW_TOPICS      12
#define NW_FEEDS       32
#define NW_ITEMS       800
#define NW_STORIES     320
#define NW_TERMS       30          /* per trend period */
#define NW_HITS        80          /* archive search results */
#define NW_KEEP_HOURS  72          /* items kept in memory */

#define NW_TITLE       200
#define NW_LINK        200
#define NW_SUMMARY     300

typedef struct
{
    char name[28];
    uint32_t color;                /* 0xRRGGBB */
    bool enabled;
} nw_topic_t;

typedef struct
{
    char name[28];                 /* "ANSA Politica" */
    char url[200];
    uint8_t topic;
    bool enabled;
    /* last round */
    int16_t count;                 /* items read */
    int64_t fetched;               /* unix time of the last success */
    char error[40];
} nw_feed_t;

typedef struct
{
    uint32_t id;                   /* hash of the link */
    int64_t when;                  /* unix time */
    uint8_t topic;
    uint8_t feed;
    int16_t story;                 /* index in the stories, -1 = none */
    uint32_t keys[6];              /* keyword stems */
    uint8_t key_count;
    char title[NW_TITLE];
    char link[NW_LINK];
    char summary[NW_SUMMARY];
} nw_item_t;

typedef struct
{
    int16_t lead;                  /* item index shown as the headline */
    uint16_t count;                /* items */
    uint8_t sources;               /* distinct feeds */
    uint8_t topic;
    int64_t first;                 /* oldest and newest item */
    int64_t last;
    float score;
} nw_story_t;

typedef enum { NW_DAY, NW_WEEK, NW_MONTH, NW_YEAR, NW_PERIODS } nw_period_t;

typedef struct
{
    char word[NW_WORD];
    uint16_t count;                /* mentions in the period */
    uint16_t series[12];           /* oldest first: 12 slices of the period */
} nw_term_t;

typedef struct
{
    int64_t when;
    uint8_t topic;
    char source[28];
    char title[NW_TITLE];
    char link[NW_LINK];
} nw_hit_t;

typedef enum { NW_IDLE, NW_WAITING_WIFI, NW_FETCHING, NW_ANALYSING, NW_SEARCHING, NW_FAILED } nw_phase_t;

typedef struct
{
    nw_phase_t phase;
    int percent;
    char message[64];
    int64_t fetched;               /* unix time of the last complete round */
    int items;
    int stories;
    uint32_t revision;             /* bumps whenever something visible changes */
    bool background_ok;            /* the last background round could run */
} nw_status_t;

typedef struct
{
    uint8_t every_hours;           /* background rounds: 0 = only with the app open, 1, 2, 4, 8 */
    uint8_t per_feed;              /* newest items read per feed (10..40) */
} nw_settings_t;

/* ----------------------------------------------------------- service */

/* At boot (internal app): loads the cached items, the feeds and the trends. */
esp_err_t nw_start(void);
/* App open/closed: open asks a round if the data is older than 20 minutes. */
void nw_app_open(bool open);
/* From a 1-minute timer: a background round when due (and memory allows). */
void nw_background_tick(void);
void nw_refresh(void);

void nw_status(nw_status_t *out);
void nw_settings(nw_settings_t *out);
void nw_set_settings(const nw_settings_t *settings);

/* ------------------------------------------------------- data (locked) */

void nw_lock(void);
void nw_unlock(void);
const nw_item_t *nw_items(int *count);
const nw_story_t *nw_stories(int *count);       /* best first */
const nw_topic_t *nw_topics(int *count);
const nw_feed_t *nw_feeds(int *count);
/* Items of a story, newest first (indices into nw_items). */
int nw_story_items(int story, int16_t *out, int max);
/* Items of a topic, newest first. */
int nw_topic_items(int topic, int16_t *out, int max);

/* Feeds and topics replaced together (dashboard). Saved; a round follows. */
bool nw_set_sources(const nw_topic_t *topics, int topic_count, const nw_feed_t *feeds, int feed_count);

/* ---------------------------------------------------------- analysis */

/* Cached top terms of a period (computed after each round). Locked. */
const nw_term_t *nw_trends(nw_period_t period, int *count);

/* Archive search "guerra iran": newest first, `months` back (1..12).
 * Starts the job; nw_search_result() is valid when it is done. */
void nw_search(const char *query, int months);
bool nw_search_running(void);
/* Locked. query = the text searched. */
const nw_hit_t *nw_search_result(int *count, char *query, size_t size);

/* Wikipedia (it) background for a topic: first matching page's summary. */
void nw_context(const char *query);
bool nw_context_running(void);
/* Locked. Empty title = nothing (yet). */
void nw_context_result(char *title, size_t title_size, char *text, size_t text_size);

/* Archive month file "news/2026-10.tsv" (false if no storage). */
bool nw_path(const char *name, char *out, size_t size);

/* ---------------------------------------------------------- UI / web */

void news_ui_create(lv_obj_t *screen);
bool news_ui_back(void);
void news_ui_destroy(void);
esp_err_t news_web_start(void);

/* Local "14 ott, 16:05" / "16:05" for today. */
void nw_time_text(int64_t when, char *out, size_t size);

#endif
