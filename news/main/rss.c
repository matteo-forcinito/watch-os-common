/* Streaming RSS / Atom reader (see rss.h). Pure C: also built by the host tests. */
#include "rss.h"

#include "text.h"

#include <string.h>

void rss_init(rss_reader_t *r, char *window, size_t size, rss_item_cb_t cb, void *ctx)
{
    memset(r, 0, sizeof(*r));
    r->window = window;
    r->size = size;
    r->cb = cb;
    r->ctx = ctx;
    r->window[0] = '\0';
}

/* "<tag" followed by a separator (so "<title" does not match "<titles"). */
static const char *find_tag(const char *from, const char *end, const char *open)
{
    size_t n = strlen(open);
    for (const char *p = from; p + n < end; p++)
    {
        p = strstr(p, open);
        if (p == NULL || p + n >= end)
        {
            return NULL;
        }
        char c = p[n];
        if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\n' || c == '\r')
        {
            return p;
        }
    }
    return NULL;
}

/* Text of <tag ...>text</tag> inside [block, end); false if missing or empty. */
static bool field(const char *block, const char *end, const char *tag, char *out, size_t size)
{
    char open[24] = "<", close[26] = "</";
    strncat(open, tag, sizeof(open) - 2);
    strncat(close, tag, sizeof(close) - 4);
    strcat(close, ">");
    const char *p = find_tag(block, end, open);
    if (p == NULL)
    {
        return false;
    }
    const char *gt = memchr(p, '>', (size_t)(end - p));
    if (gt == NULL || gt[-1] == '/')
    {
        return false;   /* self-closing */
    }
    const char *text = gt + 1;
    const char *stop = strstr(text, close);
    if (stop == NULL || stop > end)
    {
        return false;
    }
    /* Copy a generous slice, clean it (tags and entities shrink it), then cut. */
    char raw[1024];
    size_t n = (size_t)(stop - text);
    n = n < sizeof(raw) - 1 ? n : sizeof(raw) - 1;
    memcpy(raw, text, n);
    raw[n] = '\0';
    nw_clean(raw);
    nw_copy(out, size, raw);
    return out[0] != '\0';
}

/* Atom: <link rel="alternate" href="..."/> (or the first link without rel). */
static bool atom_link(const char *block, const char *end, char *out, size_t size)
{
    const char *p = block;
    bool found = false;
    while ((p = find_tag(p, end, "<link")) != NULL)
    {
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (gt == NULL)
        {
            break;
        }
        char tag[512];
        size_t n = (size_t)(gt - p);
        n = n < sizeof(tag) - 1 ? n : sizeof(tag) - 1;
        memcpy(tag, p, n);
        tag[n] = '\0';
        const char *href = strstr(tag, "href=\"");
        const char *rel = strstr(tag, "rel=\"");
        bool alternate = rel == NULL || strncmp(rel + 5, "alternate", 9) == 0;
        if (href != NULL && alternate)
        {
            href += 6;
            const char *q = strchr(href, '"');
            if (q != NULL)
            {
                size_t len = (size_t)(q - href);
                len = len < size - 1 ? len : size - 1;
                memcpy(out, href, len);
                out[len] = '\0';
                nw_clean(out);
                found = true;
                break;
            }
        }
        p = gt;
    }
    return found;
}

static void parse_item(rss_reader_t *r, const char *block, const char *end)
{
    rss_item_t *it = &r->item;
    memset(it, 0, sizeof(*it));
    if (!field(block, end, "title", it->title, sizeof(it->title)))
    {
        return;
    }
    if (!field(block, end, "link", it->link, sizeof(it->link)) && !atom_link(block, end, it->link, sizeof(it->link)))
    {
        field(block, end, "guid", it->link, sizeof(it->link));
    }
    char date[64];
    if (field(block, end, "pubDate", date, sizeof(date)) || field(block, end, "dc:date", date, sizeof(date)) ||
        field(block, end, "published", date, sizeof(date)) || field(block, end, "updated", date, sizeof(date)))
    {
        it->when = nw_date(date);
    }
    if (!field(block, end, "description", it->summary, sizeof(it->summary)))
    {
        if (!field(block, end, "summary", it->summary, sizeof(it->summary)))
        {
            field(block, end, "content", it->summary, sizeof(it->summary));
        }
    }
    r->items++;
    if (r->cb != NULL && !r->cb(it, r->ctx))
    {
        r->stopped = true;
    }
}

static void drop(rss_reader_t *r, size_t count)
{
    if (count >= r->used)
    {
        r->used = 0;
    }
    else
    {
        memmove(r->window, r->window + count, r->used - count);
        r->used -= count;
    }
    r->window[r->used] = '\0';
}

/* Item starts and ends, RSS or Atom. */
static const char *item_start(const char *from, const char *end)
{
    const char *a = find_tag(from, end, "<item");
    const char *b = find_tag(from, end, "<entry");
    return a == NULL ? b : b == NULL ? a : (a < b ? a : b);
}

static const char *item_end(const char *from, size_t *tag_len)
{
    const char *a = strstr(from, "</item>");
    const char *b = strstr(from, "</entry>");
    if (a != NULL && (b == NULL || a < b))
    {
        *tag_len = 7;
        return a;
    }
    *tag_len = 8;
    return b;
}

static void process(rss_reader_t *r)
{
    for (;;)
    {
        char *w = r->window;
        const char *end = w + r->used;
        if (!r->in_item)
        {
            const char *start = item_start(w, end);
            if (start == NULL)
            {
                /* Keep a possible partial "<entr" for the next chunk. */
                if (r->used > 8)
                {
                    drop(r, r->used - 8);
                }
                return;
            }
            drop(r, (size_t)(start - w));
            r->in_item = true;
            continue;
        }
        size_t tag_len = 0;
        const char *stop = item_end(w, &tag_len);
        if (stop == NULL)
        {
            if (r->used + 1 >= r->size)
            {
                /* Bigger than the window (an article inlined in the feed): skip it. */
                r->skipping = true;
                drop(r, r->used - 9);
            }
            return;
        }
        if (!r->skipping)
        {
            parse_item(r, w, stop);
        }
        r->skipping = false;
        r->in_item = false;
        drop(r, (size_t)(stop - w) + tag_len);
        if (r->stopped)
        {
            return;
        }
    }
}

bool rss_feed(rss_reader_t *r, const char *data, size_t length)
{
    while (length > 0 && !r->stopped)
    {
        size_t room = r->size - 1 - r->used;
        size_t n = length < room ? length : room;
        memcpy(r->window + r->used, data, n);
        r->used += n;
        r->window[r->used] = '\0';
        data += n;
        length -= n;
        process(r);
    }
    return !r->stopped;
}
