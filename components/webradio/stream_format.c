#include "stream_format.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* strcasestr() is not standard C: a small portable version. */
static bool contains_ignore_case(const char *text, const char *word)
{
    size_t length = strlen(word);

    for (; *text != '\0'; text++)
    {
        if (strncasecmp(text, word, length) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool type_is(const char *content_type, const char *prefix)
{
    return strncasecmp(content_type, prefix, strlen(prefix)) == 0;
}

/* Extension of the URL path, ignoring any "?query". */
static bool url_has_extension(const char *url, const char *extension)
{
    size_t path_length = strcspn(url, "?#");
    size_t ext_length = strlen(extension);

    return path_length > ext_length &&
           strncasecmp(url + path_length - ext_length, extension, ext_length) == 0;
}

stream_format_t stream_format_from_type(const char *content_type, const char *url)
{
    const char *type = content_type != NULL ? content_type : "";

    if (type_is(type, "audio/mpeg") || type_is(type, "audio/mp3") || type_is(type, "audio/mpeg3"))
    {
        return STREAM_FORMAT_MP3;
    }

    if (type_is(type, "audio/aac") || type_is(type, "audio/aacp") || type_is(type, "audio/x-aac"))
    {
        return STREAM_FORMAT_AAC;
    }

    if (type_is(type, "video/mp2t") || type_is(type, "video/mpeg"))
    {
        return STREAM_FORMAT_TS;
    }

    if (contains_ignore_case(type, "mpegurl") || contains_ignore_case(type, "scpls"))
    {
        return STREAM_FORMAT_PLAYLIST;
    }

    if (url == NULL)
    {
        return STREAM_FORMAT_UNKNOWN;
    }

    if (url_has_extension(url, ".m3u8") || url_has_extension(url, ".m3u") || url_has_extension(url, ".pls"))
    {
        return STREAM_FORMAT_PLAYLIST;
    }

    if (url_has_extension(url, ".mp3"))
    {
        return STREAM_FORMAT_MP3;
    }

    if (url_has_extension(url, ".aac"))
    {
        return STREAM_FORMAT_AAC;
    }

    if (url_has_extension(url, ".ts"))
    {
        return STREAM_FORMAT_TS;
    }

    return STREAM_FORMAT_UNKNOWN;
}

stream_format_t stream_format_sniff(const uint8_t *data, size_t length)
{
    size_t skip = id3_tag_size(data, length);

    if (skip >= length)
    {
        return STREAM_FORMAT_UNKNOWN;
    }

    data += skip;
    length -= skip;

    /* TS: a 0x47 sync byte every 188 bytes. */
    if (length > 376 && data[0] == 0x47 && data[188] == 0x47 && data[376] == 0x47)
    {
        return STREAM_FORMAT_TS;
    }

    if ((length >= 7 && memcmp(data, "#EXTM3U", 7) == 0) ||
        (length >= 10 && strncasecmp((const char *)data, "[playlist]", 10) == 0))
    {
        return STREAM_FORMAT_PLAYLIST;
    }

    /* Look for a frame header: 0xFFF = sync, then layer bits tell AAC from MP3. */
    for (size_t i = 0; i + 1 < length && i < 4096; i++)
    {
        if (data[i] != 0xFF)
        {
            continue;
        }

        uint8_t b = data[i + 1];

        if ((b & 0xF6) == 0xF0)
        {
            return STREAM_FORMAT_AAC;   /* ADTS: layer bits 00 */
        }

        if ((b & 0xE0) == 0xE0 && (b & 0x06) != 0)
        {
            return STREAM_FORMAT_MP3;   /* MPEG audio: layer bits != 00 */
        }
    }

    return STREAM_FORMAT_UNKNOWN;
}

const char *stream_format_name(stream_format_t format)
{
    switch (format)
    {
        case STREAM_FORMAT_MP3:      return "MP3";
        case STREAM_FORMAT_AAC:      return "AAC";
        case STREAM_FORMAT_TS:       return "TS";
        case STREAM_FORMAT_PLAYLIST: return "PLAYLIST";
        default:                     return "?";
    }
}

size_t id3_tag_size(const uint8_t *data, size_t length)
{
    if (length < 10 || memcmp(data, "ID3", 3) != 0)
    {
        return 0;
    }

    /* "Syncsafe" size: 4 bytes of 7 bits each. */
    size_t size = ((size_t)(data[6] & 0x7F) << 21) | ((size_t)(data[7] & 0x7F) << 14) |
                  ((size_t)(data[8] & 0x7F) << 7) | (size_t)(data[9] & 0x7F);
    bool footer = (data[5] & 0x10) != 0;

    return 10 + size + (footer ? 10 : 0);
}

bool playlist_is_hls(const char *text)
{
    return strstr(text, "#EXT-X-") != NULL;
}

static bool copy_url(const char *start, char *out, size_t size)
{
    while (isspace((unsigned char)*start))
    {
        start++;
    }

    size_t length = strcspn(start, "\r\n");

    while (length > 0 && isspace((unsigned char)start[length - 1]))
    {
        length--;
    }

    if (length == 0 || length >= size ||
        (strncasecmp(start, "http://", 7) != 0 && strncasecmp(start, "https://", 8) != 0))
    {
        return false;
    }

    memcpy(out, start, length);
    out[length] = '\0';

    return true;
}

bool playlist_first_url(const char *text, char *out, size_t size)
{
    for (const char *line = text; line != NULL && *line != '\0';)
    {
        while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
        {
            line++;
        }

        /* .pls: File1=http://... */
        if (strncasecmp(line, "File", 4) == 0)
        {
            const char *equals = strchr(line, '=');
            const char *end = line + strcspn(line, "\r\n");

            if (equals != NULL && equals < end && copy_url(equals + 1, out, size))
            {
                return true;
            }
        }
        else if (*line != '#' && copy_url(line, out, size))   /* .m3u: a bare URL */
        {
            return true;
        }

        line = strchr(line, '\n');
    }

    return false;
}

bool icy_parse_title(const char *metadata, char *out, size_t size)
{
    const char *start = strstr(metadata, "StreamTitle='");

    if (start == NULL || size == 0)
    {
        return false;
    }

    start += strlen("StreamTitle='");

    /* The title ends at "';" (a plain ' may be part of the title: "Don't"). */
    const char *end = strstr(start, "';");

    if (end == NULL)
    {
        end = start + strlen(start);

        if (end > start && end[-1] == '\'')
        {
            end--;
        }
    }

    size_t length = (size_t)(end - start);

    if (length >= size)
    {
        length = size - 1;
    }

    memcpy(out, start, length);
    out[length] = '\0';

    return length > 0;
}
