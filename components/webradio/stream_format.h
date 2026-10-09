#ifndef STREAM_FORMAT_H
#define STREAM_FORMAT_H

/*
 * Small helpers to understand what a radio URL returns. Plain C, tested on
 * the PC (test/host).
 *
 *   - which format is it?  (Content-Type, URL extension, first bytes)
 *   - .pls / .m3u playlist -> the real stream URL
 *   - ID3 tags (at the start of HLS .aac segments) -> how many bytes to skip
 *   - ICY metadata ("StreamTitle='Artist - Song';") -> the song title
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    STREAM_FORMAT_UNKNOWN,
    STREAM_FORMAT_MP3,
    STREAM_FORMAT_AAC,        /* ADTS frames */
    STREAM_FORMAT_TS,         /* MPEG transport stream (HLS segments) */
    STREAM_FORMAT_PLAYLIST,   /* .pls/.m3u, or HLS .m3u8: read the text to know */
} stream_format_t;

/* From the Content-Type header, or the URL extension if the type says nothing. */
stream_format_t stream_format_from_type(const char *content_type, const char *url);

/* From the first bytes of the data (after any ID3 tag). */
stream_format_t stream_format_sniff(const uint8_t *data, size_t length);

const char *stream_format_name(stream_format_t format);

/* Size of the ID3v2 tag at the start of `data` (header included), 0 if none. */
size_t id3_tag_size(const uint8_t *data, size_t length);

/* True if the playlist text is HLS (has #EXT-X- tags) rather than a plain list. */
bool playlist_is_hls(const char *text);

/* First stream URL of a .pls ("File1=...") or plain .m3u playlist. */
bool playlist_first_url(const char *text, char *out, size_t size);

/* Extract the title from ICY metadata. False if there is none or it is empty. */
bool icy_parse_title(const char *metadata, char *out, size_t size);

#endif
