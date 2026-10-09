#include "services/http_stream.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "http_stream";

#define TIMEOUT_MS    8000
#define MAX_REDIRECTS 6

/* Response headers only reach us through events. */
static esp_err_t on_event(esp_http_client_event_t *event)
{
    http_stream_t *stream = event->user_data;

    if (event->event_id == HTTP_EVENT_ON_HEADER && stream != NULL)
    {
        if (strcasecmp(event->header_key, "Content-Type") == 0)
        {
            snprintf(stream->content_type, sizeof(stream->content_type), "%s", event->header_value);
        }
        else if (strcasecmp(event->header_key, "icy-metaint") == 0)
        {
            stream->icy_metaint = atoi(event->header_value);
        }
    }

    return ESP_OK;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Send the request on the current client and follow redirects. */
static esp_err_t request(http_stream_t *stream)
{
    esp_err_t err = ESP_FAIL;

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++)
    {
        stream->content_type[0] = '\0';
        stream->icy_metaint = 0;

        err = esp_http_client_open(stream->client, 0);

        if (err != ESP_OK)
        {
            break;
        }

        if (esp_http_client_fetch_headers(stream->client) < 0)
        {
            err = ESP_FAIL;
            break;
        }

        stream->status = esp_http_client_get_status_code(stream->client);

        if (!is_redirect(stream->status))
        {
            err = (stream->status >= 200 && stream->status < 300) ? ESP_OK : ESP_ERR_NOT_FOUND;
            break;
        }

        err = esp_http_client_set_redirection(stream->client);

        if (err != ESP_OK)
        {
            break;
        }

        /* Drop the redirect page and connect again (maybe to another host). */
        esp_http_client_close(stream->client);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (err == ESP_OK)
    {
        esp_http_client_get_url(stream->client, stream->url, sizeof(stream->url));
    }

    return err;
}

esp_err_t http_stream_open(http_stream_t *stream, const char *url, bool icy_metadata)
{
    memset(stream, 0, sizeof(*stream));
    stream->icy = icy_metadata;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 3072,             /* GitHub release downloads redirect to ~1.5 KB signed URLs */
        .user_agent = "AmoledWatch/1.0 (ESP32-S3)",
        .disable_auto_redirect = true,     /* handled in request(), to see every hop */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = stream,
        .keep_alive_enable = true,         /* notice dead connections */
        .save_client_session = true,       /* TLS session resumption */
    };

    stream->client = esp_http_client_init(&config);

    if (stream->client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (icy_metadata)
    {
        esp_http_client_set_header(stream->client, "Icy-MetaData", "1");
    }

    esp_err_t err = request(stream);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: %s (HTTP %d)", url, esp_err_to_name(err), stream->status);
        http_stream_close(stream);
    }

    return err;
}

esp_err_t http_stream_reopen(http_stream_t *stream, const char *url)
{
    if (stream->client != NULL)
    {
        /* Same host: esp_http_client keeps the socket and TLS session open. */
        if (esp_http_client_set_url(stream->client, url) == ESP_OK && request(stream) == ESP_OK)
        {
            return ESP_OK;
        }

        ESP_LOGD(TAG, "Connection not reusable, reconnecting");
        http_stream_close(stream);
    }

    return http_stream_open(stream, url, stream->icy);
}

int http_stream_read(http_stream_t *stream, uint8_t *buffer, int size)
{
    if (stream->client == NULL)
    {
        return -1;
    }

    int got = esp_http_client_read(stream->client, (char *)buffer, size);

    if (got == 0 && !esp_http_client_is_complete_data_received(stream->client))
    {
        return -1;   /* connection dropped, not a clean end */
    }

    return got;
}

void http_stream_close(http_stream_t *stream)
{
    if (stream->client != NULL)
    {
        esp_http_client_close(stream->client);
        esp_http_client_cleanup(stream->client);
        stream->client = NULL;
    }
}

esp_err_t http_stream_get_text(const char *url, char *out, size_t size, char *final_url, size_t final_url_size)
{
    http_stream_t *stream = calloc(1, sizeof(http_stream_t));

    if (stream == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = http_stream_open(stream, url, false);

    if (err == ESP_OK)
    {
        size_t length = 0;
        int got;

        while (length + 1 < size &&
               (got = http_stream_read(stream, (uint8_t *)out + length, (int)(size - 1 - length))) > 0)
        {
            length += (size_t)got;
        }

        out[length] = '\0';

        if (final_url != NULL)
        {
            snprintf(final_url, final_url_size, "%s", stream->url);
        }

        err = length > 0 ? ESP_OK : ESP_FAIL;
        http_stream_close(stream);
    }

    free(stream);

    return err;
}
