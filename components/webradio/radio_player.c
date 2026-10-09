#include "webradio/radio_player.h"

#include "hls.h"
#include "core/sys.h"
#include "services/http_stream.h"
#include "stream_format.h"

#include "core/power.h"
#include "core/state.h"
#include "hardware/audio.h"
#include "services/wifi.h"

#include "esp_aac_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "impl/esp_ts_dec.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "radio";

#define URL_MAX              HTTP_STREAM_URL_MAX
#define BUFFER_BYTES         (384 * 1024)  /* in PSRAM: ~20 s at 128 kbit/s */
#define PREBUFFER_BYTES      (32 * 1024)   /* ~2 s at 128 kbit/s before playing */
#define WIFI_TIMEOUT_MS      20000
#define MAX_RECONNECTS       5
#define NET_CHUNK            2048
#define DEC_CHUNK            4096
#define PLAYLIST_TEXT_MAX    (16 * 1024)
#define HLS_START_FROM_END   3             /* start this many segments before the live edge */
#define STOP_DEC_TIMEOUT_MS  2000
#define STOP_NET_TIMEOUT_MS  9000
#define MAX_DECODE_ERRORS    100

typedef struct
{
    char url[URL_MAX];

    volatile radio_state_t state;
    volatile bool stop;
    volatile bool output;              /* the decoder may use the speaker */
    volatile stream_format_t format;   /* set by the network task before any data */
    volatile bool net_done;
    volatile bool dec_done;
    volatile int volume;
    volatile int64_t last_audio_us;
    volatile uint32_t sample_rate;
    volatile uint8_t channels;

    bool wifi_held;
    StreamBufferHandle_t buffer;

    char failure[32];
    char title[96];
} player_t;

static player_t s_player = {.state = RADIO_STATE_IDLE, .net_done = true, .dec_done = true};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_codecs_registered = false;

/* ------------------------------------------------------------ helpers */

static void publish(void)
{
    state_set(STATE_RADIO, s_player.state);
}

static void set_state(radio_state_t state)
{
    if (s_player.state != RADIO_STATE_FAILED && s_player.state != state)
    {
        s_player.state = state;
        publish();
    }
}

static void fail(const char *reason)
{
    if (s_player.stop || s_player.state == RADIO_STATE_FAILED)
    {
        return;
    }

    ESP_LOGW(TAG, "Failed: %s", reason);
    snprintf(s_player.failure, sizeof(s_player.failure), "%s", reason);
    s_player.state = RADIO_STATE_FAILED;
    publish();
}

/* Push bytes into the buffer; blocks while it is full (that paces the download). */
static bool send_bytes(const uint8_t *data, size_t length)
{
    while (length > 0 && !s_player.stop)
    {
        size_t sent = xStreamBufferSend(s_player.buffer, data, length, pdMS_TO_TICKS(200));
        data += sent;
        length -= sent;
    }

    return !s_player.stop;
}

static bool set_format(stream_format_t format)
{
    if (format != STREAM_FORMAT_MP3 && format != STREAM_FORMAT_AAC && format != STREAM_FORMAT_TS)
    {
        fail("Formato non supportato");
        return false;
    }

    if (s_player.format == STREAM_FORMAT_UNKNOWN)
    {
        ESP_LOGI(TAG, "Format: %s", stream_format_name(format));
        s_player.format = format;
    }
    else if (s_player.format != format)
    {
        fail("Formato cambiato");
        return false;
    }

    return true;
}

static void sleep_while_running(uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms && !s_player.stop; waited += 100)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------- direct (ICY) streams */

typedef struct
{
    int metaint;       /* audio bytes between metadata blocks, 0 = no metadata */
    int until_meta;
    int meta_left;     /* metadata bytes still to read */
    int meta_length;
    char meta[512];
} icy_state_t;

static void icy_metadata_done(icy_state_t *icy)
{
    char title[sizeof(s_player.title)];

    icy->meta[icy->meta_length < (int)sizeof(icy->meta) ? icy->meta_length : (int)sizeof(icy->meta) - 1] = '\0';

    if (icy_parse_title(icy->meta, title, sizeof(title)))
    {
        portENTER_CRITICAL(&s_lock);
        bool changed = strcmp(s_player.title, title) != 0;
        memcpy(s_player.title, title, sizeof(title));
        portEXIT_CRITICAL(&s_lock);

        if (changed)
        {
            ESP_LOGI(TAG, "Now playing: %s", title);
            state_bump(STATE_RADIO_VERSION);
        }
    }
}

/* Split the received bytes into audio (sent on) and metadata (parsed). */
static bool icy_process(icy_state_t *icy, const uint8_t *data, int length)
{
    while (length > 0)
    {
        if (icy->metaint == 0)
        {
            return send_bytes(data, (size_t)length);
        }

        if (icy->meta_left > 0)
        {
            int take = length < icy->meta_left ? length : icy->meta_left;

            for (int i = 0; i < take; i++)
            {
                if (icy->meta_length < (int)sizeof(icy->meta) - 1)
                {
                    icy->meta[icy->meta_length++] = (char)data[i];
                }
            }

            icy->meta_left -= take;
            data += take;
            length -= take;

            if (icy->meta_left == 0)
            {
                icy_metadata_done(icy);
                icy->until_meta = icy->metaint;
            }

            continue;
        }

        if (icy->until_meta == 0)
        {
            /* One length byte: metadata size / 16 (often 0 = nothing new). */
            icy->meta_left = data[0] * 16;
            icy->meta_length = 0;
            data++;
            length--;

            if (icy->meta_left == 0)
            {
                icy->until_meta = icy->metaint;
            }

            continue;
        }

        int audio = length < icy->until_meta ? length : icy->until_meta;

        if (!send_bytes(data, (size_t)audio))
        {
            return false;
        }

        icy->until_meta -= audio;
        data += audio;
        length -= audio;
    }

    return true;
}

/* Returns how many bytes of audio went through (to tell a real stream from a dud). */
static size_t pump_direct(http_stream_t *stream, const uint8_t *prefix, int prefix_length)
{
    icy_state_t *icy = calloc(1, sizeof(icy_state_t));
    uint8_t *chunk = malloc(NET_CHUNK);
    size_t total = 0;

    if (icy == NULL || chunk == NULL)
    {
        free(icy);
        free(chunk);
        return 0;
    }

    icy->metaint = stream->icy_metaint;
    icy->until_meta = stream->icy_metaint;

    if (prefix_length > 0 && icy_process(icy, prefix, prefix_length))
    {
        total += (size_t)prefix_length;
    }

    set_state(RADIO_STATE_BUFFERING);

    while (!s_player.stop)
    {
        int got = http_stream_read(stream, chunk, NET_CHUNK);

        if (got <= 0 || !icy_process(icy, chunk, got))
        {
            break;
        }

        total += (size_t)got;
    }

    free(icy);
    free(chunk);

    return total;
}

/* ------------------------------------------------------------------ HLS */

/*
 * Download one segment into the buffer, on the reused connection `stream`.
 * Packed AAC segments start with an ID3 tag (skipped).
 */
static bool fetch_segment(http_stream_t *stream, uint8_t *chunk, const char *url)
{
    if (http_stream_reopen(stream, url) != ESP_OK)
    {
        return false;
    }

    stream_format_t format = stream_format_from_type(stream->content_type, stream->url);
    size_t skip = 0;
    bool first = true;
    bool ok = false;
    int got = 0;

    while (!s_player.stop && (got = http_stream_read(stream, chunk, NET_CHUNK)) > 0)
    {
        const uint8_t *data = chunk;
        size_t length = (size_t)got;

        if (first)
        {
            first = false;

            if (format != STREAM_FORMAT_TS && format != STREAM_FORMAT_AAC)
            {
                format = stream_format_sniff(chunk, length);
            }

            if (!set_format(format))
            {
                break;
            }

            skip = id3_tag_size(chunk, length);
        }

        if (skip > 0)
        {
            size_t drop = skip < length ? skip : length;
            skip -= drop;
            data += drop;
            length -= drop;
        }

        if (!send_bytes(data, length))
        {
            break;
        }

        ok = true;
    }

    if (got < 0)
    {
        http_stream_close(stream);   /* broken connection: next segment reconnects */
    }

    return ok;
}

/* Download a playlist on the reused connection. */
static bool fetch_text(http_stream_t *stream, const char *url, char *out, size_t size, char *final_url)
{
    if (http_stream_reopen(stream, url) != ESP_OK)
    {
        return false;
    }

    size_t length = 0;
    int got = 0;

    while (length + 1 < size && (got = http_stream_read(stream, (uint8_t *)out + length, (int)(size - 1 - length))) > 0)
    {
        length += (size_t)got;
    }

    out[length] = '\0';

    if (final_url != NULL)
    {
        snprintf(final_url, URL_MAX, "%s", stream->url);
    }

    return length > 0;
}

static size_t play_hls(char *playlist_url, char *text)
{
    hls_playlist_t *pl = malloc(sizeof(hls_playlist_t));
    char *segment_url = malloc(URL_MAX);
    char *base = malloc(URL_MAX);
    uint8_t *chunk = malloc(NET_CHUNK);
    http_stream_t *connection = calloc(1, sizeof(http_stream_t));
    size_t segments_played = 0;

    if (pl == NULL || segment_url == NULL || base == NULL || chunk == NULL || connection == NULL)
    {
        goto done;
    }

    snprintf(base, URL_MAX, "%s", playlist_url);

    if (!hls_parse(text, pl))
    {
        fail("Playlist non valida");
        goto done;
    }

    /* Master playlist: follow the chosen variant once. */
    if (pl->is_master)
    {
        if (!url_resolve(base, pl->variant, segment_url, URL_MAX) ||
            !fetch_text(connection, segment_url, text, PLAYLIST_TEXT_MAX, base) ||
            !hls_parse(text, pl) || pl->is_master)
        {
            fail("Playlist non valida");
            goto done;
        }
    }

    if (pl->encrypted)
    {
        fail("Stream criptato");
        goto done;
    }

    int64_t next = pl->first_sequence + (pl->count > HLS_START_FROM_END ? pl->count - HLS_START_FROM_END : 0);
    int playlist_errors = 0;

    set_state(RADIO_STATE_BUFFERING);

    while (!s_player.stop)
    {
        if (next < pl->first_sequence)
        {
            next = pl->first_sequence;   /* we fell behind the live window */
        }

        for (int i = 0; i < pl->count && !s_player.stop; i++)
        {
            int64_t sequence = pl->first_sequence + i;

            if (sequence < next)
            {
                continue;
            }

            if (url_resolve(base, pl->segments[i], segment_url, URL_MAX) &&
                fetch_segment(connection, chunk, segment_url))
            {
                segments_played++;
            }

            next = sequence + 1;
        }

        if (pl->ended || s_player.state == RADIO_STATE_FAILED)
        {
            break;
        }

        /* Live playlist: ask again for new segments (same connection). */
        sleep_while_running((uint32_t)pl->target_duration * 1000 / 2);

        if (fetch_text(connection, base, text, PLAYLIST_TEXT_MAX, base) && hls_parse(text, pl) && !pl->is_master)
        {
            playlist_errors = 0;
        }
        else if (++playlist_errors >= 3)
        {
            break;
        }
    }

done:
    if (connection != NULL)
    {
        http_stream_close(connection);
    }

    free(connection);
    free(pl);
    free(segment_url);
    free(base);
    free(chunk);

    /* For the reconnect logic: a playing stream counts as "lots of bytes". */
    return segments_played * 64 * 1024;
}

/* ------------------------------------------------------- network task */

/* Open `url` and play it; follows .pls/.m3u playlists. Returns audio bytes streamed. */
static size_t play_url(const char *start_url)
{
    char *url = malloc(URL_MAX);
    char *text = malloc(PLAYLIST_TEXT_MAX);
    http_stream_t *stream = malloc(sizeof(http_stream_t));
    size_t played = 0;

    if (url == NULL || text == NULL || stream == NULL)
    {
        fail("Memoria insufficiente");
        goto done;
    }

    snprintf(url, URL_MAX, "%s", start_url);

    for (int hop = 0; hop < 3 && !s_player.stop; hop++)
    {
        set_state(RADIO_STATE_CONNECTING);

        if (http_stream_open(stream, url, true) != ESP_OK)
        {
            snprintf(s_player.failure, sizeof(s_player.failure), "Nessuna connessione");
            break;
        }

        stream_format_t format = stream_format_from_type(stream->content_type, stream->url);
        int prefix = 0;

        if (format == STREAM_FORMAT_UNKNOWN)
        {
            prefix = http_stream_read(stream, (uint8_t *)text, 1024);
            format = prefix > 0 ? stream_format_sniff((const uint8_t *)text, (size_t)prefix) : STREAM_FORMAT_UNKNOWN;
        }

        if (format == STREAM_FORMAT_PLAYLIST)
        {
            /* Read the whole (small) text after what was already read. */
            int length = prefix > 0 ? prefix : 0;
            int got;

            while (length < PLAYLIST_TEXT_MAX - 1 &&
                   (got = http_stream_read(stream, (uint8_t *)text + length, PLAYLIST_TEXT_MAX - 1 - length)) > 0)
            {
                length += got;
            }

            text[length] = '\0';
            snprintf(url, URL_MAX, "%s", stream->url);
            http_stream_close(stream);

            if (playlist_is_hls(text))
            {
                played = play_hls(url, text);
                break;
            }

            if (!playlist_first_url(text, url, URL_MAX))
            {
                fail("Playlist non valida");
                break;
            }

            continue;   /* .pls / .m3u: open the URL inside */
        }

        if (set_format(format))
        {
            played = pump_direct(stream, (const uint8_t *)text, prefix);
        }

        http_stream_close(stream);
        break;
    }

done:
    free(url);
    free(text);
    free(stream);

    return played;
}

static void net_task(void *arg)
{
    (void)arg;

    if (!wifi_is_connected())
    {
        set_state(RADIO_STATE_WIFI);
    }

    if (wifi_acquire(WIFI_TIMEOUT_MS) != ESP_OK)
    {
        fail(wifi_known_any() ? "Wi-Fi non raggiungibile" : "Wi-Fi non configurato");
        goto done;
    }

    s_player.wifi_held = true;

    for (int failures = 0; !s_player.stop && s_player.state != RADIO_STATE_FAILED;)
    {
        size_t played = play_url(s_player.url);

        if (s_player.stop || s_player.state == RADIO_STATE_FAILED)
        {
            break;
        }

        /* A stream that played for a while and dropped: reconnect, it is normal. */
        failures = (played > 256 * 1024) ? 0 : failures + 1;

        if (failures >= MAX_RECONNECTS)
        {
            fail(s_player.failure[0] != '\0' ? s_player.failure : "Stream perso");
            break;
        }

        ESP_LOGW(TAG, "Stream ended, reconnecting (%d)", failures);
        sleep_while_running(1000u * (uint32_t)(failures + 1));
    }

done:
    if (s_player.wifi_held)
    {
        s_player.wifi_held = false;
        wifi_release();
    }

    s_player.net_done = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------- decoder task */

static esp_audio_simple_dec_handle_t open_decoder(stream_format_t format)
{
    if (!s_codecs_registered)
    {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_codecs_registered = true;
    }

    /* AAC+ (HE-AAC) is what many radios use at 48-64 kbit/s, e.g. m2o. */
    esp_aac_dec_cfg_t aac_config = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_config.aac_plus_enable = true;

    esp_ts_dec_cfg_t ts_config = {.aac_plus_enable = true};

    esp_audio_simple_dec_cfg_t config = {0};

    switch (format)
    {
        case STREAM_FORMAT_MP3:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            break;

        case STREAM_FORMAT_AAC:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            config.dec_cfg = &aac_config;
            config.cfg_size = sizeof(aac_config);
            break;

        default:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
            config.dec_cfg = &ts_config;
            config.cfg_size = sizeof(ts_config);
            break;
    }

    esp_audio_simple_dec_handle_t decoder = NULL;

    if (esp_audio_simple_dec_open(&config, &decoder) != ESP_AUDIO_ERR_OK)
    {
        return NULL;
    }

    return decoder;
}

/* Wait for enough data (and for permission to play). False if stopped or failed. */
static bool wait_until_ready(void)
{
    while (!s_player.stop)
    {
        size_t buffered = xStreamBufferBytesAvailable(s_player.buffer);

        if (s_player.format != STREAM_FORMAT_UNKNOWN && (buffered >= PREBUFFER_BYTES || s_player.net_done))
        {
            if (s_player.output)
            {
                return buffered > 0;
            }

            set_state(RADIO_STATE_READY);
        }

        if (s_player.state == RADIO_STATE_FAILED || (s_player.net_done && buffered == 0))
        {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return false;
}

static void dec_task(void *arg)
{
    (void)arg;

    uint8_t *input = malloc(DEC_CHUNK);
    uint32_t output_size = 8192;
    uint8_t *output = malloc(output_size);
    esp_audio_simple_dec_handle_t decoder = NULL;
    bool audio_on = false;
    bool boosted = false;
    int applied_volume = -1;
    int errors = 0;

    if (input == NULL || output == NULL || !wait_until_ready())
    {
        goto done;
    }

    decoder = open_decoder(s_player.format);

    if (decoder == NULL)
    {
        fail("Decoder");
        goto done;
    }

    /* HE-AAC decoding is heavy: run at full speed while playing. */
    power_cpu_boost(true);
    boosted = true;

    while (!s_player.stop && s_player.state != RADIO_STATE_FAILED)
    {
        size_t got = xStreamBufferReceive(s_player.buffer, input, DEC_CHUNK, pdMS_TO_TICKS(100));

        if (got == 0)
        {
            if (s_player.net_done)
            {
                break;   /* nothing more will come */
            }

            continue;    /* underrun: the alarm notices via radio_player_silence_ms() */
        }

        esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = (uint32_t)got};

        while (raw.len > 0 && !s_player.stop)
        {
            esp_audio_simple_dec_out_t frame = {.buffer = output, .len = output_size};
            esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &frame);

            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                uint8_t *bigger = realloc(output, frame.needed_size);

                if (bigger == NULL)
                {
                    fail("Memoria insufficiente");
                    break;
                }

                output = bigger;
                output_size = frame.needed_size;
                continue;
            }

            if (ret != ESP_AUDIO_ERR_OK)
            {
                /* Corrupted data (e.g. after a reconnect): drop this piece and resync. */
                if (++errors > MAX_DECODE_ERRORS)
                {
                    fail("Audio non valido");
                }

                esp_audio_simple_dec_reset(decoder);
                break;
            }

            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;

            if (frame.decoded_size == 0)
            {
                continue;
            }

            errors = 0;

            esp_audio_simple_dec_info_t info = {0};
            esp_audio_simple_dec_get_info(decoder, &info);
            uint8_t channels = info.channel >= 2 ? 2 : 1;

            if (!audio_on || info.sample_rate != s_player.sample_rate || channels != s_player.channels)
            {
                if (audio_start(info.sample_rate, channels) != ESP_OK)
                {
                    fail("Audio non disponibile");
                    break;
                }

                audio_on = true;
                applied_volume = -1;
                s_player.sample_rate = info.sample_rate;
                s_player.channels = channels;
                state_bump(STATE_RADIO_VERSION);
                ESP_LOGI(TAG, "Playing %lu Hz, %u ch", (unsigned long)info.sample_rate, channels);
            }

            if (applied_volume != s_player.volume)
            {
                applied_volume = s_player.volume;
                audio_set_volume(applied_volume);
            }

            audio_write((const int16_t *)output, frame.decoded_size / sizeof(int16_t));

            s_player.last_audio_us = esp_timer_get_time();
            set_state(RADIO_STATE_PLAYING);
        }
    }

    if (!s_player.stop && s_player.state != RADIO_STATE_FAILED)
    {
        fail("Stream terminato");
    }

done:
    if (decoder != NULL)
    {
        esp_audio_simple_dec_close(decoder);
    }

    if (audio_on)
    {
        audio_stop();
    }

    if (boosted)
    {
        power_cpu_boost(false);
    }

    free(input);
    free(output);

    s_player.dec_done = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- public */

esp_err_t radio_player_start(const char *url, int volume, bool play_when_ready)
{
    if (!s_player.net_done || !s_player.dec_done)
    {
        return ESP_ERR_INVALID_STATE;   /* the previous station is still closing */
    }

    if (s_player.buffer != NULL)
    {
        vStreamBufferDeleteWithCaps(s_player.buffer);
        s_player.buffer = NULL;
    }

    memset(&s_player, 0, sizeof(s_player));
    snprintf(s_player.url, sizeof(s_player.url), "%s", url);
    s_player.volume = volume;
    s_player.output = play_when_ready;
    s_player.format = STREAM_FORMAT_UNKNOWN;
    s_player.state = RADIO_STATE_CONNECTING;
    publish();
    state_bump(STATE_RADIO_VERSION);

    s_player.buffer = xStreamBufferCreateWithCaps(BUFFER_BYTES, 1, MALLOC_CAP_SPIRAM);

    if (s_player.buffer == NULL)
    {
        s_player.net_done = s_player.dec_done = true;
        fail("Memoria insufficiente");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Starting %s", url);

    /* Network on core 0 (with Wi-Fi), decoding on core 1. */
    if (!sys_task_create(net_task, "radio_net", 8192, NULL, 5, 0))
    {
        s_player.net_done = s_player.dec_done = true;
        fail("Memoria insufficiente");
        return ESP_ERR_NO_MEM;
    }

    if (!sys_task_create(dec_task, "radio_dec", 10240, NULL, 6, 1))
    {
        s_player.stop = true;
        s_player.dec_done = true;
        fail("Memoria insufficiente");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void radio_player_play(void)
{
    s_player.output = true;
}

static void wait_for(volatile bool *done, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; !*done && waited < timeout_ms; waited += 20)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void radio_player_stop(void)
{
    if (s_player.net_done && s_player.dec_done && s_player.state == RADIO_STATE_IDLE)
    {
        return;
    }

    s_player.stop = true;

    /* The decoder stops within ~100 ms and closes the speaker itself. */
    wait_for(&s_player.dec_done, STOP_DEC_TIMEOUT_MS);

    /* The network task leaves at its next read (HTTP timeout at worst). */
    wait_for(&s_player.net_done, STOP_NET_TIMEOUT_MS);

    if (!s_player.dec_done || !s_player.net_done)
    {
        ESP_LOGE(TAG, "Radio tasks did not stop in time");
        return;   /* the buffer is freed by the next start, once they are done */
    }

    vStreamBufferDeleteWithCaps(s_player.buffer);
    s_player.buffer = NULL;
    s_player.state = RADIO_STATE_IDLE;
    publish();

    ESP_LOGI(TAG, "Stopped");
}

bool radio_player_busy(void)
{
    return !s_player.net_done || !s_player.dec_done;
}

radio_state_t radio_player_state(void)
{
    return s_player.state;
}

const char *radio_player_state_text(void)
{
    switch (s_player.state)
    {
        case RADIO_STATE_IDLE:       return "Ferma";
        case RADIO_STATE_WIFI:       return "Connessione Wi-Fi...";
        case RADIO_STATE_CONNECTING: return "Connessione...";
        case RADIO_STATE_BUFFERING:  return "Buffering...";
        case RADIO_STATE_READY:      return "Pronta";
        case RADIO_STATE_PLAYING:    return "In riproduzione";
        case RADIO_STATE_FAILED:
        default:                     return s_player.failure[0] != '\0' ? s_player.failure : "Errore";
    }
}

bool radio_player_title(char *out, size_t size)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(out, size, "%s", s_player.title);
    portEXIT_CRITICAL(&s_lock);

    return out[0] != '\0';
}

void radio_player_format_text(char *out, size_t size)
{
    if (s_player.sample_rate == 0)
    {
        out[0] = '\0';
        return;
    }

    snprintf(out, size, "%s %lu kHz %s", s_player.format == STREAM_FORMAT_MP3 ? "MP3" : "AAC",
             (unsigned long)(s_player.sample_rate / 1000), s_player.channels == 2 ? "stereo" : "mono");
}

void radio_player_set_volume(int percent)
{
    s_player.volume = percent;
}

uint32_t radio_player_silence_ms(void)
{
    if (s_player.last_audio_us == 0)
    {
        return 0;
    }

    return (uint32_t)((esp_timer_get_time() - s_player.last_audio_us) / 1000);
}
