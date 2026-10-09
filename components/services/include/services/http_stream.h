#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

/*
 * An HTTP(S) download read a piece at a time, on top of esp_http_client:
 * follows redirects (radio links often bounce to a CDN), checks HTTPS
 * certificates with the built-in bundle and remembers the headers a radio
 * needs (Content-Type, icy-metaint).
 *
 * HTTPS made cheap for HLS: an HLS radio downloads a new 6-10 s segment all
 * the time, usually from the same CDN host. http_stream_reopen() sends the
 * next request on the connection that is already open (HTTP keep-alive), so
 * the expensive TLS handshake happens once instead of every few seconds.
 * When a new connection is needed anyway, the TLS session is resumed
 * (session tickets), which is much lighter than a full handshake.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

#define HTTP_STREAM_URL_MAX 320

typedef struct
{
    esp_http_client_handle_t client;
    char url[HTTP_STREAM_URL_MAX];   /* final URL, after redirects */
    char content_type[48];
    int icy_metaint;                 /* audio bytes between ICY metadata blocks, 0 = none */
    int status;
    bool icy;
} http_stream_t;

/* Open `url` (GET). `icy_metadata` asks Icecast servers to send song titles. */
esp_err_t http_stream_open(http_stream_t *stream, const char *url, bool icy_metadata);

/*
 * GET another URL on the same stream object, reusing the connection when the
 * host is the same. The previous body must have been read to the end.
 * Opens a fresh connection if the stream is closed or the reuse fails.
 */
esp_err_t http_stream_reopen(http_stream_t *stream, const char *url);

/* Read up to `size` bytes: > 0 bytes read, 0 = end of the body, < 0 = error. */
int http_stream_read(http_stream_t *stream, uint8_t *buffer, int size);

void http_stream_close(http_stream_t *stream);

/* Download a small text body into `out` (NUL-terminated). `final_url` optional. */
esp_err_t http_stream_get_text(const char *url, char *out, size_t size, char *final_url, size_t final_url_size);

#endif
