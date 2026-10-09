#include "hls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Copy one line (without \r\n) into out. Returns a pointer to the next line or NULL. */
static const char *next_line(const char *text, char *out, size_t size)
{
    size_t length = strcspn(text, "\r\n");
    size_t copy = length < size ? length : size - 1;

    memcpy(out, text, copy);
    out[copy] = '\0';

    /* Trailing spaces would end up in URLs. */
    while (copy > 0 && isspace((unsigned char)out[copy - 1]))
    {
        out[--copy] = '\0';
    }

    text += length;

    while (*text == '\r' || *text == '\n')
    {
        text++;
    }

    return *text != '\0' ? text : NULL;
}

static long bandwidth_of(const char *stream_inf)
{
    const char *found = strstr(stream_inf, "BANDWIDTH=");

    return found != NULL ? strtol(found + strlen("BANDWIDTH="), NULL, 10) : 0;
}

bool hls_parse(const char *text, hls_playlist_t *pl)
{
    memset(pl, 0, sizeof(*pl));

    while (*text == ' ' || *text == '\r' || *text == '\n' || (unsigned char)*text == 0xEF ||
           (unsigned char)*text == 0xBB || (unsigned char)*text == 0xBF)   /* also skip a UTF-8 BOM */
    {
        text++;
    }

    if (!starts_with(text, "#EXTM3U"))
    {
        return false;
    }

    char line[HLS_URI_MAX];
    long best_bandwidth = -1;
    bool next_is_variant = false;
    long variant_bandwidth = 0;
    int64_t total_segments = 0;

    for (const char *p = text; p != NULL;)
    {
        p = next_line(p, line, sizeof(line));

        if (line[0] == '\0')
        {
            continue;
        }

        if (starts_with(line, "#EXT-X-STREAM-INF:"))
        {
            pl->is_master = true;
            next_is_variant = true;
            variant_bandwidth = bandwidth_of(line);
        }
        else if (starts_with(line, "#EXT-X-TARGETDURATION:"))
        {
            pl->target_duration = atoi(line + strlen("#EXT-X-TARGETDURATION:"));
        }
        else if (starts_with(line, "#EXT-X-MEDIA-SEQUENCE:"))
        {
            pl->first_sequence = strtoll(line + strlen("#EXT-X-MEDIA-SEQUENCE:"), NULL, 10);
        }
        else if (starts_with(line, "#EXT-X-ENDLIST"))
        {
            pl->ended = true;
        }
        else if (starts_with(line, "#EXT-X-KEY:") && strstr(line, "METHOD=NONE") == NULL)
        {
            pl->encrypted = true;
        }
        else if (line[0] == '#')
        {
            continue;   /* other tags (#EXTINF, dates...) are not needed */
        }
        else if (next_is_variant)
        {
            /* Lowest bandwidth: less data over Wi-Fi, and audio-only if offered. */
            if (best_bandwidth < 0 || variant_bandwidth < best_bandwidth)
            {
                best_bandwidth = variant_bandwidth;
                snprintf(pl->variant, sizeof(pl->variant), "%s", line);
            }

            next_is_variant = false;
        }
        else
        {
            /* A segment. Keep the newest HLS_MAX_SEGMENTS, shifting older ones out. */
            if (pl->count == HLS_MAX_SEGMENTS)
            {
                memmove(pl->segments[0], pl->segments[1], sizeof(pl->segments[0]) * (HLS_MAX_SEGMENTS - 1));
                pl->count--;
            }

            snprintf(pl->segments[pl->count++], HLS_URI_MAX, "%s", line);
            total_segments++;
        }
    }

    /* The sequence number belongs to the first segment in the file. */
    pl->first_sequence += total_segments - pl->count;

    if (pl->target_duration <= 0)
    {
        pl->target_duration = 6;
    }

    return pl->is_master ? pl->variant[0] != '\0' : pl->count > 0;
}

bool url_resolve(const char *base, const char *reference, char *out, size_t size)
{
    int written;

    if (strstr(reference, "://") != NULL)
    {
        written = snprintf(out, size, "%s", reference);
        return written > 0 && (size_t)written < size;
    }

    const char *scheme_end = strstr(base, "://");

    if (scheme_end == NULL)
    {
        return false;
    }

    const char *host_start = scheme_end + 3;
    const char *path_start = host_start + strcspn(host_start, "/?#");

    if (reference[0] == '/' && reference[1] == '/')
    {
        /* "//host/path": same scheme */
        written = snprintf(out, size, "%.*s:%s", (int)(scheme_end - base), base, reference);
    }
    else if (reference[0] == '/')
    {
        written = snprintf(out, size, "%.*s%s", (int)(path_start - base), base, reference);
    }
    else
    {
        /* Relative: replace the last path element (ignore any ?query of the base). */
        size_t path_length = strcspn(path_start, "?#");
        const char *last_slash = NULL;

        for (const char *c = path_start; c < path_start + path_length; c++)
        {
            if (*c == '/')
            {
                last_slash = c;
            }
        }

        if (last_slash == NULL)
        {
            written = snprintf(out, size, "%.*s/%s", (int)(path_start - base), base, reference);
        }
        else
        {
            written = snprintf(out, size, "%.*s%s", (int)(last_slash + 1 - base), base, reference);
        }
    }

    return written > 0 && (size_t)written < size;
}
