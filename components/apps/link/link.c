/*
 * Orbit Link: what a device offers to the Orbit companion so that several
 * devices behave as one system. Each device stays simple (draw a scene, play
 * or capture audio, report input); Orbit keeps the state and the timing.
 *
 *   GET  /api/link                 version, features, screen
 *   POST /api/link/scene           {scene, ...}: drawn above every app (link_scene.c)
 *   GET  /api/link/events?since=N  input after event N (scene taps, dial, Orbi)
 *   POST /api/link/speaker         raw PCM 16 kHz mono s16le (max 32 KB): queued, played
 *   POST /api/link/mic             {on}: capture into a 4 s ring, indicator on screen
 *   GET  /api/link/mic?since=P     "OMIC" + u32 end position + the samples after P
 *   GET/POST /api/timer            countdown and stopwatch, app open or closed
 *   GET/POST /api/voice            voice memos
 *
 * Nothing runs at rest: the audio rings live in PSRAM and, with their task,
 * exist only while used; they go away after a few idle seconds. Every route
 * goes through web_route(), so only the paired companion reaches it.
 */
#include "apps_internal.h"
#include "link_internal.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "hardware/board.h"
#include "hardware/microphone.h"
#include "services/sound.h"
#include "services/voice_memo.h"
#include "services/web_server.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "link";

#define RATE            16000
#define SPEAKER_RING    (RATE * 2 * 2)        /* 2 s of audio, bytes */
#define SPEAKER_CHUNK   (32 * 1024)           /* largest request body */
#define SPEAKER_PREFILL (RATE * 2 / 5)        /* 200 ms before the first sample */
#define SPEAKER_IDLE_MS 1500
#define MIC_RING        (RATE * 4)            /* 4 s, samples */
#define MIC_MAX_REPLY   RATE                  /* 1 s per request, samples */
#define MIC_IDLE_US     (4LL * 1000000)
#define EVENTS_MAX      32

/* ----------------------------------------------------------------- events */

typedef struct
{
    uint32_t seq;
    char type[12];
    char text[16];   /* string value, "" when the value is a number */
    int32_t value;
} link_event_t;

static link_event_t s_events[EVENTS_MAX];
static uint32_t s_event_seq;
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;

static void push_event(const char *type, const char *text, int value)
{
    taskENTER_CRITICAL(&s_event_mux);
    link_event_t *e = &s_events[s_event_seq % EVENTS_MAX];
    e->seq = ++s_event_seq;
    snprintf(e->type, sizeof(e->type), "%s", type);
    snprintf(e->text, sizeof(e->text), "%s", text != NULL ? text : "");
    e->value = value;
    taskEXIT_CRITICAL(&s_event_mux);
}

void link_event(const char *type, int value)
{
    push_event(type, NULL, value);
}

void link_event_text(const char *type, const char *value)
{
    push_event(type, value, 0);
}

/* ------------------------------------------------------------------ helpers */

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = json != NULL ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

/* Small JSON body (at most `limit` bytes), NULL when missing or invalid. */
static cJSON *read_json(httpd_req_t *req, size_t limit)
{
    if (req->content_len == 0 || req->content_len > limit)
    {
        return NULL;
    }
    char *text = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL)
    {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        int n = httpd_req_recv(req, text + got, req->content_len - got);
        if (n <= 0)
        {
            free(text);
            return NULL;
        }
        got += (size_t)n;
    }
    text[got] = '\0';
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char query[64], value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK)
    {
        return fallback;
    }
    return (int)strtol(value, NULL, 10);
}

/* --------------------------------------------------------------- speaker */

static uint8_t *s_speaker;              /* ring, PSRAM */
static size_t s_speaker_head, s_speaker_fill;
static SemaphoreHandle_t s_audio_lock;  /* guards both rings' pointers */
static atomic_bool s_speaker_running;
static atomic_bool s_mic_running;

static void audio_lock_init(void)
{
    if (s_audio_lock == NULL)
    {
        s_audio_lock = xSemaphoreCreateMutex();
    }
}

static size_t speaker_take(int16_t *out, size_t max_bytes)
{
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    size_t n = s_speaker_fill < max_bytes ? s_speaker_fill : max_bytes;
    n &= ~(size_t)1;
    size_t tail = (s_speaker_head + SPEAKER_RING - s_speaker_fill) % SPEAKER_RING;
    for (size_t i = 0; i < n; i++)
    {
        ((uint8_t *)out)[i] = s_speaker[(tail + i) % SPEAKER_RING];
    }
    s_speaker_fill -= n;
    xSemaphoreGive(s_audio_lock);
    return n;
}

static void speaker_task(void *arg)
{
    (void)arg;
    int16_t block[256];
    int idle_ms = 0;
    /* Prefill: a short network hiccup must not cut every syllable. */
    for (int waited = 0; waited < 600 && s_speaker_fill < SPEAKER_PREFILL; waited += 20)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (audio_start(RATE, 1) == ESP_OK)
    {
        audio_set_volume(settings_get()->volume);
        while (idle_ms < SPEAKER_IDLE_MS)
        {
            size_t n = speaker_take(block, sizeof(block));
            if (n == 0)
            {
                idle_ms += 20;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            idle_ms = 0;
            audio_write(block, n / 2);
        }
        audio_stop();
    }
    else
    {
        ESP_LOGW(TAG, "Speaker busy");
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    free(s_speaker);
    s_speaker = NULL;
    s_speaker_fill = 0;
    s_speaker_head = 0;
    xSemaphoreGive(s_audio_lock);
    atomic_store(&s_speaker_running, false);
    vTaskDelete(NULL);
}

static esp_err_t speaker_post(httpd_req_t *req)
{
    if (!board_info()->has_audio)
    {
        return fail(req, "404 Not Found", "Nessun altoparlante");
    }
    if (sound_alarm_playing() || atomic_load(&s_mic_running))
    {
        return fail(req, "409 Conflict", "Altoparlante occupato");
    }
    size_t length = req->content_len;
    if (length == 0 || length > SPEAKER_CHUNK || (length & 1) != 0)
    {
        return fail(req, "400 Bad Request", "PCM 16 bit, al massimo 32 KB");
    }
    audio_lock_init();
    uint8_t *data = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL)
    {
        return fail(req, "503 Service Unavailable", "Memoria occupata");
    }
    size_t got = 0;
    while (got < length)
    {
        int n = httpd_req_recv(req, (char *)data + got, length - got);
        if (n <= 0)
        {
            free(data);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    if (s_speaker == NULL)
    {
        s_speaker = heap_caps_malloc(SPEAKER_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_speaker_head = 0;
        s_speaker_fill = 0;
    }
    bool room = s_speaker != NULL && s_speaker_fill + length <= SPEAKER_RING;
    if (room)
    {
        for (size_t i = 0; i < length; i++)
        {
            s_speaker[(s_speaker_head + i) % SPEAKER_RING] = data[i];
        }
        s_speaker_head = (s_speaker_head + length) % SPEAKER_RING;
        s_speaker_fill += length;
    }
    size_t queued = s_speaker_fill;
    xSemaphoreGive(s_audio_lock);
    free(data);
    if (!room)
    {
        return fail(req, "503 Service Unavailable", "Buffer pieno: rallenta");
    }
    if (!atomic_exchange(&s_speaker_running, true) &&
        !sys_task_create(speaker_task, "link_spk", 3072, NULL, 5, SYS_CORE_ANY))
    {
        atomic_store(&s_speaker_running, false);
        return fail(req, "503 Service Unavailable", "Memoria interna insufficiente");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "queued_ms", (double)(queued * 1000 / (RATE * 2)));
    return send_json(req, out);
}

/* ------------------------------------------------------------ microphone */

static int16_t *s_mic;           /* ring, PSRAM */
static atomic_uint s_mic_pos;    /* samples written since the capture started */
static atomic_bool s_mic_wanted;
static _Atomic int64_t s_mic_read_at;

static void mic_task(void *arg)
{
    (void)arg;
    int16_t block[256];
    if (microphone_start(RATE, 1) == ESP_OK)
    {
        link_scene_mic(true);
        while (atomic_load(&s_mic_wanted) && esp_timer_get_time() - atomic_load(&s_mic_read_at) < MIC_IDLE_US)
        {
            size_t count = 0;
            if (microphone_read(block, sizeof(block) / sizeof(block[0]), &count) != ESP_OK || count == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            unsigned pos = atomic_load(&s_mic_pos);
            for (size_t i = 0; i < count; i++)
            {
                s_mic[(pos + i) % MIC_RING] = block[i];
            }
            atomic_store(&s_mic_pos, pos + (unsigned)count);
        }
        microphone_stop();
        link_scene_mic(false);
    }
    else
    {
        ESP_LOGW(TAG, "Microphone busy");
    }
    atomic_store(&s_mic_wanted, false);
    atomic_store(&s_mic_running, false);
    vTaskDelete(NULL);
}

static esp_err_t mic_post(httpd_req_t *req)
{
    if (!microphone_supported())
    {
        return fail(req, "404 Not Found", "Nessun microfono");
    }
    cJSON *json = read_json(req, 64);
    bool on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "on"));
    cJSON_Delete(json);
    if (!on)
    {
        atomic_store(&s_mic_wanted, false);
        return send_json(req, cJSON_CreateObject());
    }
    if (atomic_load(&s_speaker_running) || sound_alarm_playing())
    {
        return fail(req, "409 Conflict", "Audio occupato");
    }
    atomic_store(&s_mic_read_at, esp_timer_get_time());
    atomic_store(&s_mic_wanted, true);
    if (!atomic_exchange(&s_mic_running, true))
    {
        if (s_mic == NULL)
        {
            s_mic = heap_caps_malloc(MIC_RING * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        atomic_store(&s_mic_pos, 0);
        if (s_mic == NULL || !sys_task_create(mic_task, "link_mic", 3072, NULL, 5, SYS_CORE_ANY))
        {
            atomic_store(&s_mic_running, false);
            atomic_store(&s_mic_wanted, false);
            return fail(req, "503 Service Unavailable", "Memoria insufficiente");
        }
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "rate", RATE);
    return send_json(req, out);
}

static esp_err_t mic_get(httpd_req_t *req)
{
    if (!atomic_load(&s_mic_running) || s_mic == NULL)
    {
        return fail(req, "409 Conflict", "Microfono spento");
    }
    atomic_store(&s_mic_read_at, esp_timer_get_time());
    unsigned end = atomic_load(&s_mic_pos);
    int since_query = query_int(req, "since", -1);
    unsigned start;
    if (since_query < 0 || (unsigned)since_query > end || end - (unsigned)since_query > MIC_RING - RATE / 2)
    {
        start = end > RATE / 2 ? end - RATE / 2 : 0;   /* first read: the last half second */
    }
    else
    {
        start = (unsigned)since_query;
    }
    if (end - start > MIC_MAX_REPLY)
    {
        end = start + MIC_MAX_REPLY;
    }
    size_t count = end - start;
    uint8_t *out = heap_caps_malloc(8 + count * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL)
    {
        return fail(req, "503 Service Unavailable", "Memoria occupata");
    }
    memcpy(out, "OMIC", 4);
    out[4] = (uint8_t)end;
    out[5] = (uint8_t)(end >> 8);
    out[6] = (uint8_t)(end >> 16);
    out[7] = (uint8_t)(end >> 24);
    for (size_t i = 0; i < count; i++)
    {
        int16_t v = s_mic[(start + i) % MIC_RING];
        out[8 + i * 2] = (uint8_t)v;
        out[9 + i * 2] = (uint8_t)((uint16_t)v >> 8);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)out, (ssize_t)(8 + count * 2));
    free(out);
    return err;
}

/* ---------------------------------------------------------------- routes */

static esp_err_t link_get(httpd_req_t *req)
{
    const board_info_t *b = board_info();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "version", 1);
    cJSON_AddStringToObject(out, "node", sys_nickname());
    cJSON *features = cJSON_AddArrayToObject(out, "features");
    const char *always[] = {"scene", "events", "dial", "timer"};
    for (size_t i = 0; i < sizeof(always) / sizeof(always[0]); i++)
    {
        cJSON_AddItemToArray(features, cJSON_CreateString(always[i]));
    }
    if (b->has_audio)
    {
        cJSON_AddItemToArray(features, cJSON_CreateString("speaker"));
    }
    if (microphone_supported())
    {
        cJSON_AddItemToArray(features, cJSON_CreateString("mic"));
        cJSON_AddItemToArray(features, cJSON_CreateString("voice"));
    }
    if (b->has_knob)
    {
        cJSON_AddItemToArray(features, cJSON_CreateString("knob"));
    }
    cJSON *screen = cJSON_AddObjectToObject(out, "screen");
    cJSON_AddNumberToObject(screen, "w", b->width);
    cJSON_AddNumberToObject(screen, "h", b->height);
    cJSON_AddBoolToObject(screen, "round", b->round);
    cJSON_AddBoolToObject(screen, "visible", !lv_port_headless());
    cJSON *audio = cJSON_AddObjectToObject(out, "audio");
    cJSON_AddNumberToObject(audio, "rate", RATE);
    cJSON_AddBoolToObject(audio, "speaking", atomic_load(&s_speaker_running));
    cJSON_AddBoolToObject(audio, "listening", atomic_load(&s_mic_running));
    return send_json(req, out);
}

static esp_err_t scene_post(httpd_req_t *req)
{
    cJSON *json = read_json(req, 1024);
    if (!cJSON_IsObject(json))
    {
        cJSON_Delete(json);
        return fail(req, "400 Bad Request", "Serve {scene: ...}");
    }
    bool ok = link_scene_post(json);   /* takes ownership */
    if (!ok)
    {
        return fail(req, "400 Bad Request", "Scena sconosciuta o interfaccia occupata");
    }
    return send_json(req, cJSON_CreateObject());
}

static esp_err_t events_get(httpd_req_t *req)
{
    int since = query_int(req, "since", -1);
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "events");
    link_event_t copy[EVENTS_MAX];
    taskENTER_CRITICAL(&s_event_mux);
    uint32_t last = s_event_seq;
    memcpy(copy, s_events, sizeof(copy));
    taskEXIT_CRITICAL(&s_event_mux);
    cJSON_AddNumberToObject(out, "seq", last);
    if (since >= 0)
    {
        uint32_t first = last > EVENTS_MAX ? last - EVENTS_MAX + 1 : 1;
        for (uint32_t seq = (uint32_t)since + 1 > first ? (uint32_t)since + 1 : first; seq <= last; seq++)
        {
            const link_event_t *e = &copy[(seq - 1) % EVENTS_MAX];
            if (e->seq != seq)
            {
                continue;
            }
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "seq", e->seq);
            cJSON_AddStringToObject(j, "type", e->type);
            if (e->text[0] != '\0')
            {
                cJSON_AddStringToObject(j, "value", e->text);
            }
            else
            {
                cJSON_AddNumberToObject(j, "value", e->value);
            }
            cJSON_AddItemToArray(list, j);
        }
    }
    return send_json(req, out);
}

static cJSON *timer_json(void)
{
    timer_remote_t t;
    lv_port_lock();
    timer_remote_state(&t);
    lv_port_unlock();
    cJSON *out = cJSON_CreateObject();
    cJSON *countdown = cJSON_AddObjectToObject(out, "countdown");
    cJSON_AddBoolToObject(countdown, "running", t.countdown_running);
    cJSON_AddBoolToObject(countdown, "ringing", t.countdown_ringing);
    cJSON_AddNumberToObject(countdown, "total_s", t.countdown_total_s);
    cJSON_AddNumberToObject(countdown, "remaining_s", t.countdown_remaining_s);
    cJSON *watch = cJSON_AddObjectToObject(out, "stopwatch");
    cJSON_AddBoolToObject(watch, "running", t.watch_running);
    cJSON_AddNumberToObject(watch, "elapsed_ms", (double)t.watch_elapsed_ms);
    return out;
}

static esp_err_t timer_get(httpd_req_t *req)
{
    return send_json(req, timer_json());
}

static esp_err_t timer_post(httpd_req_t *req)
{
    cJSON *json = read_json(req, 128);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(json, "seconds");
    bool ok = false;
    if (action != NULL)
    {
        lv_port_lock();
        ok = timer_remote(action, cJSON_IsNumber(seconds) ? seconds->valueint : 0);
        lv_port_unlock();
    }
    cJSON_Delete(json);
    if (!ok)
    {
        return fail(req, "400 Bad Request", "Azione non valida");
    }
    return send_json(req, timer_json());
}

static esp_err_t voice_get(httpd_req_t *req)
{
    voice_memo_snapshot_t *v = heap_caps_malloc(sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (v == NULL)
    {
        return fail(req, "503 Service Unavailable", "Memoria occupata");
    }
    voice_memo_snapshot(v);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "phase", v->phase);
    cJSON_AddNumberToObject(out, "elapsed_ms", v->elapsed_ms);
    cJSON_AddNumberToObject(out, "duration_ms", v->duration_ms);
    cJSON_AddNumberToObject(out, "selected", v->selected);
    cJSON_AddStringToObject(out, "folder", "/memos");
    cJSON *levels = cJSON_AddArrayToObject(out, "levels");
    for (size_t i = 0; i < sizeof(v->levels); i++)
    {
        cJSON_AddItemToArray(levels, cJSON_CreateNumber(v->levels[i]));
    }
    cJSON *names = cJSON_AddArrayToObject(out, "names");
    cJSON *files = cJSON_AddArrayToObject(out, "files");
    for (int i = 0; i < v->count && i < VOICE_MEMO_LIST_SIZE; i++)
    {
        char shown[VOICE_MEMO_NAME_SIZE + 16];
        voice_memo_format_name(v->names[i], shown, sizeof(shown));
        cJSON_AddItemToArray(names, cJSON_CreateString(shown));
        cJSON_AddItemToArray(files, cJSON_CreateString(v->names[i]));
    }
    free(v);
    return send_json(req, out);
}

static esp_err_t voice_post(httpd_req_t *req)
{
    if (!microphone_supported())
    {
        return fail(req, "404 Not Found", "Nessun microfono");
    }
    cJSON *json = read_json(req, 128);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(json, "index");
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    bool ok = false;
    if (action != NULL && (atomic_load(&s_mic_running) || atomic_load(&s_speaker_running)) && strcmp(action, "stop") != 0)
    {
        ok = false;
    }
    else if (action != NULL && strcmp(action, "record") == 0)
    {
        ok = voice_memo_record();
    }
    else if (action != NULL && strcmp(action, "stop") == 0)
    {
        voice_memo_stop();
        ok = true;
    }
    else if (action != NULL && strcmp(action, "play") == 0)
    {
        ok = voice_memo_play(i);
    }
    else if (action != NULL && strcmp(action, "delete") == 0)
    {
        ok = voice_memo_delete(i);
    }
    cJSON_Delete(json);
    if (!ok)
    {
        return fail(req, "409 Conflict", "Comando non eseguito: audio occupato o memo inesistente");
    }
    return send_json(req, cJSON_CreateObject());
}

static void register_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/link", .method = HTTP_GET, .handler = link_get},
        {.uri = "/api/link/scene", .method = HTTP_POST, .handler = scene_post},
        {.uri = "/api/link/events", .method = HTTP_GET, .handler = events_get},
        {.uri = "/api/link/speaker", .method = HTTP_POST, .handler = speaker_post},
        {.uri = "/api/link/mic", .method = HTTP_POST, .handler = mic_post},
        {.uri = "/api/link/mic", .method = HTTP_GET, .handler = mic_get},
        {.uri = "/api/timer", .method = HTTP_GET, .handler = timer_get},
        {.uri = "/api/timer", .method = HTTP_POST, .handler = timer_post},
        {.uri = "/api/voice", .method = HTTP_GET, .handler = voice_get},
        {.uri = "/api/voice", .method = HTTP_POST, .handler = voice_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        web_route(server, &routes[i]);
    }
}

void link_init(void)
{
    audio_lock_init();
    web_server_add_routes(register_routes);
}
