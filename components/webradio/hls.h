#ifndef HLS_H
#define HLS_H

/*
 * HLS (HTTP Live Streaming) playlist parser, for radios like m2o, Radio
 * Deejay or RTL 102.5 that only stream this way.
 *
 * HLS is not one long stream but a text playlist (.m3u8) listing short
 * audio files ("segments", 3-10 s each). A live playlist keeps changing:
 * the player downloads it again every few seconds and fetches the new
 * segments. A "master" playlist only points to other playlists (one per
 * quality).
 *
 *     #EXTM3U                               #EXTM3U
 *     #EXT-X-STREAM-INF:BANDWIDTH=128000    #EXT-X-TARGETDURATION:10
 *     audio_128k.m3u8                       #EXT-X-MEDIA-SEQUENCE:412207
 *                                           #EXTINF:10,
 *        master playlist                    seg_412207.ts
 *                                           #EXTINF:10,
 *                                           seg_412208.ts
 *                                              media playlist
 *
 * Plain C: tested on the PC (test/host).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLS_URI_MAX      256
#define HLS_MAX_SEGMENTS 12   /* only the newest ones are kept */

typedef struct
{
    bool is_master;
    char variant[HLS_URI_MAX];     /* master: the chosen (lowest bandwidth) playlist */

    int target_duration;           /* seconds, max length of a segment */
    int64_t first_sequence;        /* sequence number of segments[0] */
    bool ended;                    /* #EXT-X-ENDLIST: not live */
    bool encrypted;                /* #EXT-X-KEY: we cannot play it */
    int count;
    char segments[HLS_MAX_SEGMENTS][HLS_URI_MAX];
} hls_playlist_t;

/* Parse a playlist. Returns false if it is not an HLS playlist or is empty. */
bool hls_parse(const char *text, hls_playlist_t *playlist);

/*
 * Resolve a (maybe relative) URI against the playlist URL, like a browser:
 *   base "https://a.com/live/list.m3u8?x=1" + "seg1.ts" -> "https://a.com/live/seg1.ts"
 *   base "https://a.com/live/list.m3u8"    + "/x/s.ts" -> "https://a.com/x/s.ts"
 * Returns false if the result does not fit.
 */
bool url_resolve(const char *base, const char *reference, char *out, size_t size);

#endif
