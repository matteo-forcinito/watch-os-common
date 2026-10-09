#include "radio_list.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool radio_list_init(radio_list_t *list)
{
    list->count = 0;
    list->stations = calloc(RADIO_LIST_MAX, sizeof(radio_station_t));

    return list->stations != NULL;
}

void radio_list_free(radio_list_t *list)
{
    free(list->stations);
    list->stations = NULL;
    list->count = 0;
}

/* Copy [start, end) trimmed into out (size bytes). */
static void copy_trimmed(char *out, size_t size, const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start))
    {
        start++;
    }

    while (end > start && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    size_t length = (size_t)(end - start);

    if (length >= size)
    {
        length = size - 1;
    }

    memcpy(out, start, length);
    out[length] = '\0';
}

static bool is_url(const char *text)
{
    return strncasecmp(text, "http://", 7) == 0 || strncasecmp(text, "https://", 8) == 0;
}

bool radio_list_parse_line(const char *line, radio_station_t *station)
{
    const char *start = line;

    while (isspace((unsigned char)*start))
    {
        start++;
    }

    if (*start == '\0' || *start == '#' || *start == ';')
    {
        return false;
    }

    const char *bar1 = strchr(start, '|');
    const char *bar2 = bar1 != NULL ? strchr(bar1 + 1, '|') : NULL;

    if (bar2 == NULL)
    {
        return false;
    }

    const char *end = start + strcspn(start, "\r\n");

    /* A URL longer than the field would be cut and could never work. */
    const char *url = bar2 + 1;

    while (isspace((unsigned char)*url))
    {
        url++;
    }

    if ((size_t)(end - url) >= RADIO_URL_MAX)
    {
        return false;
    }

    copy_trimmed(station->name, sizeof(station->name), start, bar1);
    copy_trimmed(station->genre, sizeof(station->genre), bar1 + 1, bar2);
    copy_trimmed(station->url, sizeof(station->url), bar2 + 1, end);

    return station->name[0] != '\0' && is_url(station->url);
}

int radio_list_parse_text(radio_list_t *list, const char *text)
{
    int added = 0;

    while (*text != '\0' && list->count < RADIO_LIST_MAX)
    {
        const char *next = strchr(text, '\n');
        size_t length = next != NULL ? (size_t)(next - text) : strlen(text);
        char line[RADIO_NAME_MAX + RADIO_GENRE_MAX + RADIO_URL_MAX + 16];

        if (length < sizeof(line))
        {
            memcpy(line, text, length);
            line[length] = '\0';

            if (radio_list_parse_line(line, &list->stations[list->count]))
            {
                list->count++;
                added++;
            }
        }

        if (next == NULL)
        {
            break;
        }

        text = next + 1;
    }

    return added;
}

int radio_list_read(radio_list_t *list, FILE *file)
{
    char line[RADIO_NAME_MAX + RADIO_GENRE_MAX + RADIO_URL_MAX + 16];
    int added = 0;

    while (fgets(line, sizeof(line), file) != NULL && list->count < RADIO_LIST_MAX)
    {
        if (radio_list_parse_line(line, &list->stations[list->count]))
        {
            list->count++;
            added++;
        }
    }

    return added;
}

void radio_list_write(const radio_list_t *list, FILE *file)
{
    fprintf(file,
        "# Web radio stations for the AMOLED Watch.\n"
        "# One station per line:   name | genre | stream URL\n"
        "# MP3/AAC streams, .pls/.m3u playlists and HLS (.m3u8) with AAC work.\n\n");

    for (int i = 0; i < list->count; i++)
    {
        const radio_station_t *s = &list->stations[i];
        fprintf(file, "%s | %s | %s\n", s->name, s->genre, s->url);
    }
}

int radio_list_find(const radio_list_t *list, const char *name)
{
    for (int i = 0; i < list->count; i++)
    {
        if (strcasecmp(list->stations[i].name, name) == 0)
        {
            return i;
        }
    }

    return -1;
}

/* '|' and line breaks would break the file format. */
static void clean_field(char *text)
{
    for (; *text != '\0'; text++)
    {
        if (*text == '|' || *text == '\r' || *text == '\n')
        {
            *text = ' ';
        }
    }
}

bool radio_list_add(radio_list_t *list, const char *name, const char *genre, const char *url)
{
    if (list->count >= RADIO_LIST_MAX || name == NULL || url == NULL ||
        strlen(url) >= RADIO_URL_MAX || !is_url(url))
    {
        return false;
    }

    radio_station_t *s = &list->stations[list->count];

    copy_trimmed(s->name, sizeof(s->name), name, name + strlen(name));
    const char *g = genre != NULL ? genre : "";
    copy_trimmed(s->genre, sizeof(s->genre), g, g + strlen(g));
    copy_trimmed(s->url, sizeof(s->url), url, url + strlen(url));
    clean_field(s->name);
    clean_field(s->genre);
    clean_field(s->url);

    if (s->name[0] == '\0')
    {
        return false;
    }

    list->count++;

    return true;
}

bool radio_list_remove(radio_list_t *list, int index)
{
    if (index < 0 || index >= list->count)
    {
        return false;
    }

    memmove(&list->stations[index], &list->stations[index + 1],
            (size_t)(list->count - index - 1) * sizeof(radio_station_t));
    list->count--;

    return true;
}
