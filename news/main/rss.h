#ifndef NEWS_RSS_H
#define NEWS_RSS_H

/*
 * Streaming RSS 2.0 / Atom reader (pure C, tested on the host).
 *
 * The download is fed in chunks of any size; each complete <item> (RSS) or
 * <entry> (Atom) is cut out of a small window and handed to the callback with
 * its title, link, date and summary already cleaned (nw_clean). A feed of
 * any length needs only the window (one item at a time).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSS_TITLE    200
#define RSS_LINK     240
#define RSS_SUMMARY  400

typedef struct
{
    char title[RSS_TITLE];
    char link[RSS_LINK];
    char summary[RSS_SUMMARY];
    int64_t when;               /* unix seconds, 0 = no date */
} rss_item_t;

/* Return false to stop reading the feed. */
typedef bool (*rss_item_cb_t)(const rss_item_t *item, void *ctx);

typedef struct
{
    char *window;               /* caller's buffer (PSRAM), e.g. 16 KB */
    size_t size;
    size_t used;
    bool in_item;
    bool skipping;              /* an item bigger than the window: dropped */
    bool stopped;
    int items;
    rss_item_cb_t cb;
    void *ctx;
    rss_item_t item;            /* scratch, handed to the callback */
} rss_reader_t;

void rss_init(rss_reader_t *r, char *window, size_t size, rss_item_cb_t cb, void *ctx);

/* Feed the next bytes. Returns false once the callback asked to stop. */
bool rss_feed(rss_reader_t *r, const char *data, size_t length);

#endif
