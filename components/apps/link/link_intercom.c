#include "link_intercom.h"

#include "../../services/ecos/ecos_audio_wire.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "hardware/microphone.h"
#include "services/ecos.h"
#include "services/sound.h"
#include "link_internal.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define INTERCOM_MAX_SECONDS 30
#define INTERCOM_QUEUE_BLOCKS 8
#define INTERCOM_RX_IDLE_US (2LL * 1000000)
#define INTERCOM_REORDER_US 15000
#define INTERCOM_STREAM_LOSS_US 1000000
#define INTERCOM_RPC_TIMEOUT_MS 10000
#define INTERCOM_TASK_STACK 6144

typedef enum { INTERCOM_NONE, INTERCOM_CREATING_TX, INTERCOM_TX, INTERCOM_CREATING_RX, INTERCOM_RX } intercom_role_t;

typedef struct
{
    uint32_t offset;
    int16_t pcm[ECOS_AUDIO_SAMPLES];
} intercom_packet_t;

typedef struct
{
    char peer[13];
    char session_text[17];
    uint64_t session;
    int seconds;
    bool stop;
} intercom_tx_t;

typedef struct
{
    char peer[13];
    char session_text[17];
    uint64_t session;
    bool stop;
    bool closing;
    unsigned callback_refs;
    uint32_t played_offset;
    uint32_t seen_offsets[INTERCOM_QUEUE_BLOCKS];
    unsigned seen_next;
    int64_t last_packet_us;
    QueueHandle_t queue;
    StaticQueue_t *queue_control;
    uint8_t *queue_storage;
    intercom_packet_t *pending; /* PSRAM jitter/reorder window */
    size_t pending_count;
} intercom_rx_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static intercom_role_t s_role;
static void *s_active;
static bool s_handler_registered;
static bool s_cancel_creation;

static bool tx_stopped(const intercom_tx_t *tx)
{
    portENTER_CRITICAL(&s_mux);
    bool stopped = tx->stop;
    portEXIT_CRITICAL(&s_mux);
    return stopped;
}

static bool rx_stopped(const intercom_rx_t *rx)
{
    portENTER_CRITICAL(&s_mux);
    bool stopped = rx->stop;
    portEXIT_CRITICAL(&s_mux);
    return stopped;
}

static bool id_valid(const char *id)
{
    if (id == NULL || strlen(id) != 12) return false;
    bool any = false;
    for (size_t i = 0; i < 12; ++i)
    {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        if (c != '0') any = true;
    }
    return any;
}

static bool id_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL || strlen(a) != 12 || strlen(b) != 12) return false;
    for (size_t i = 0; i < 12; ++i)
    {
        char x = a[i] >= 'A' && a[i] <= 'F' ? (char)(a[i] - 'A' + 'a') : a[i];
        char y = b[i] >= 'A' && b[i] <= 'F' ? (char)(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}

static bool parse_session(const cJSON *item, uint64_t *session, char text[17])
{
    const char *value = cJSON_GetStringValue(item);
    if (value == NULL || strlen(value) != 16) return false;
    uint64_t parsed = 0;
    for (size_t i = 0; i < 16; ++i)
    {
        char c = value[i];
        unsigned nibble = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                          c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                          c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
        if (nibble > 15) return false;
        parsed = (parsed << 4) | nibble;
        text[i] = c;
    }
    text[16] = '\0';
    if (parsed == 0) return false;
    *session = parsed;
    return true;
}

static void session_text(uint64_t session, char out[17])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) out[i] = hex[(session >> ((15 - i) * 4)) & 15];
    out[16] = '\0';
}

static uint64_t new_session(void)
{
    uint64_t session;
    do { session = ((uint64_t)esp_random() << 32) | esp_random(); } while (session == 0);
    return session;
}

static void claim_release(intercom_role_t role)
{
    portENTER_CRITICAL(&s_mux);
    if (s_role == role || (role == INTERCOM_TX && s_role == INTERCOM_CREATING_TX) ||
        (role == INTERCOM_RX && s_role == INTERCOM_CREATING_RX))
    {
        s_role = INTERCOM_NONE;
        s_active = NULL;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool link_intercom_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy = s_role != INTERCOM_NONE;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

void link_intercom_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_role == INTERCOM_CREATING_TX || s_role == INTERCOM_CREATING_RX) s_cancel_creation = true;
    if (s_active != NULL && s_role == INTERCOM_TX)
        ((intercom_tx_t *)s_active)->stop = true;
    else if (s_active != NULL && s_role == INTERCOM_RX)
        ((intercom_rx_t *)s_active)->stop = true;
    portEXIT_CRITICAL(&s_mux);
}

static bool rpc_control(const char *peer, const char *action, const char *token, bool queued)
{
    char request[96];
    int n = snprintf(request, sizeof(request),
                     "{\"op\":\"intercom\",\"action\":\"%s\",\"session\":\"%s\"}",
                     action, token);
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;
    uint8_t reply[ECOS_ORBIT_MAX];
    int length = ecos_family_orbit(peer, (const uint8_t *)request, (size_t)n,
                                   reply, sizeof(reply), INTERCOM_RPC_TIMEOUT_MS);
    if (length <= 0 || (size_t)length > sizeof(reply)) return false;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)reply, (size_t)length, &end, false);
    bool ok = cJSON_IsObject(json) && end == (const char *)reply + length &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) &&
              (!queued || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "queued")));
    cJSON_Delete(json);
    return ok;
}

static void tx_worker(void *arg)
{
    intercom_tx_t *tx = arg;
    bool held = false, stream_open = false, peer_listening = false, mic_started = false;
    uint32_t offset = 0;
    int16_t pcm[ECOS_AUDIO_SAMPLES];
    int16_t input[ECOS_AUDIO_SAMPLES];
    uint8_t encoded[ECOS_AUDIO_FRAME_MAX];
    ima_adpcm_t encoder = {0};
    bool capture_error = false;
    bool stream_lost = false;
    int64_t stream_loss_since = 0;

    if (tx_stopped(tx)) goto done;
    ecos_family_hold(true);
    held = true;
    if (!rpc_control(tx->peer, "listen", tx->session_text, true)) goto done;
    peer_listening = true;
    if (tx_stopped(tx)) goto done;
    if (!ecos_family_stream_open(tx->peer)) goto done;
    stream_open = true;
    if (tx_stopped(tx)) goto done;
    if (microphone_start(ECOS_AUDIO_RATE, 1) != ESP_OK) goto done;
    mic_started = true;
    link_scene_mic(true);

    int64_t deadline = esp_timer_get_time() + (int64_t)tx->seconds * 1000000;
    while (!tx_stopped(tx) && esp_timer_get_time() < deadline)
    {
        size_t have = 0;
        while (have < ECOS_AUDIO_SAMPLES && !tx_stopped(tx) && esp_timer_get_time() < deadline)
        {
            size_t count = 0;
            size_t remaining = ECOS_AUDIO_SAMPLES - have;
            esp_err_t err = microphone_read(input, remaining, &count);
            if (err != ESP_OK)
            {
                capture_error = true;
                break;
            }
            if (count == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(3));
                continue;
            }
            if (count > remaining)
            {
                capture_error = true;
                break;
            }
            memcpy(pcm + have, input, count * sizeof(*pcm));
            have += count;
        }
        if (capture_error || have != ECOS_AUDIO_SAMPLES || esp_timer_get_time() > deadline) break;
        size_t length = ecos_audio_encode(encoded, sizeof(encoded), tx->session, offset,
                                          &encoder, pcm, ECOS_AUDIO_SAMPLES);
        if (length == 0) break;
        if (!ecos_family_stream_send(tx->peer, encoded, length))
        {
            int64_t now = esp_timer_get_time();
            if (!stream_lost)
            {
                stream_lost = true;
                stream_loss_since = now;
            }
            else if (now - stream_loss_since >= INTERCOM_STREAM_LOSS_US)
            {
                break;
            }
        }
        else
        {
            stream_lost = false;
            stream_loss_since = 0;
        }
        offset += ECOS_AUDIO_SAMPLES;
    }

done:
    if (mic_started)
    {
        microphone_stop();
        link_scene_mic(false);
    }
    if (peer_listening) (void)rpc_control(tx->peer, "stop", tx->session_text, false);
    if (stream_open) ecos_family_stream_close(tx->peer);
    if (held) ecos_family_hold(false);
    link_event_text("intercom", !held ? "cancelled" : !peer_listening ? "unknown" :
                              !mic_started ? "mic_error" : "ended");
    claim_release(INTERCOM_TX);
    heap_caps_free(tx);
    vTaskDelete(NULL);
}

bool link_intercom_start(const char *peer_id, int seconds)
{
    if (!id_valid(peer_id) || seconds < 1 || seconds > INTERCOM_MAX_SECONDS ||
        !microphone_supported() || link_audio_busy() || sound_alarm_playing()) return false;
    portENTER_CRITICAL(&s_mux);
    bool claimed = s_role == INTERCOM_NONE;
    if (claimed)
    {
        s_role = INTERCOM_CREATING_TX;
        s_cancel_creation = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!claimed) return false;

    intercom_tx_t *tx = heap_caps_calloc(1, sizeof(*tx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tx == NULL)
    {
        claim_release(INTERCOM_TX);
        return false;
    }
    for (size_t i = 0; i < 12; ++i)
        tx->peer[i] = peer_id[i] >= 'A' && peer_id[i] <= 'F' ? (char)(peer_id[i] - 'A' + 'a') : peer_id[i];
    tx->peer[12] = '\0';
    tx->session = new_session();
    session_text(tx->session, tx->session_text);
    tx->seconds = seconds;
    portENTER_CRITICAL(&s_mux);
    bool cancelled = s_cancel_creation;
    if (cancelled)
    {
        s_role = INTERCOM_NONE;
        portEXIT_CRITICAL(&s_mux);
        heap_caps_free(tx);
        return false;
    }
    s_active = tx;
    s_role = INTERCOM_TX;
    portEXIT_CRITICAL(&s_mux);
    if (!sys_task_create(tx_worker, "intercom_tx", INTERCOM_TASK_STACK, tx, 5, SYS_CORE_ANY))
    {
        claim_release(INTERCOM_TX);
        heap_caps_free(tx);
        return false;
    }
    return true;
}

static void rx_queue_free(intercom_rx_t *rx)
{
    if (rx == NULL) return;
    if (rx->queue != NULL) vQueueDelete(rx->queue);
    heap_caps_free(rx->pending);
    heap_caps_free(rx->queue_storage);
    heap_caps_free(rx->queue_control);
    heap_caps_free(rx);
}

static void rx_unpublish_and_free(intercom_rx_t *rx)
{
    portENTER_CRITICAL(&s_mux);
    rx->closing = true;
    if (s_active == rx)
    {
        s_active = NULL;
        s_role = INTERCOM_NONE;
    }
    portEXIT_CRITICAL(&s_mux);
    for (;;)
    {
        portENTER_CRITICAL(&s_mux);
        unsigned refs = rx->callback_refs;
        portEXIT_CRITICAL(&s_mux);
        if (refs == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    rx_queue_free(rx);
}

static void jitter_insert(intercom_rx_t *rx, const intercom_packet_t *packet)
{
    size_t at = 0;
    while (at < rx->pending_count && rx->pending[at].offset < packet->offset) ++at;
    if (at < rx->pending_count && rx->pending[at].offset == packet->offset) return;
    if (rx->pending_count == INTERCOM_QUEUE_BLOCKS)
    {
        if (at == INTERCOM_QUEUE_BLOCKS) return;
        --rx->pending_count; /* Keep the earliest window; discard later queued audio. */
    }
    for (size_t i = rx->pending_count; i > at; --i) rx->pending[i] = rx->pending[i - 1];
    rx->pending[at] = *packet;
    ++rx->pending_count;
}

static void rx_drain_queue(intercom_rx_t *rx)
{
    intercom_packet_t packet;
    while (xQueueReceive(rx->queue, &packet, 0) == pdTRUE)
        jitter_insert(rx, &packet);
}

static void rx_worker(void *arg)
{
    intercom_rx_t *rx = arg;
    bool held = true, audio_started = false;
    uint32_t expected = 0;
    unsigned gap_blocks = 0;
    int64_t gap_since = 0;
    bool gap_waiting = false, playback_started = false, played_audio = false;
    const int64_t start = esp_timer_get_time();
    if (!rx_stopped(rx) && !link_audio_busy() && !sound_alarm_playing() &&
        audio_start(ECOS_AUDIO_RATE, 1) == ESP_OK)
        audio_started = true;

    while (audio_started && !rx_stopped(rx) &&
           esp_timer_get_time() - start < (int64_t)INTERCOM_MAX_SECONDS * 1000000)
    {
        rx_drain_queue(rx);
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_mux);
        int64_t last_packet = rx->last_packet_us;
        portEXIT_CRITICAL(&s_mux);
        if (now - last_packet >= INTERCOM_RX_IDLE_US) break;

        if (!playback_started)
        {
            if (rx->pending_count >= 3 || now - start >= 45000) playback_started = true;
            else
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }
        if (rx->pending_count == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (rx->pending[0].offset < expected)
        {
            for (size_t i = 1; i < rx->pending_count; ++i) rx->pending[i - 1] = rx->pending[i];
            --rx->pending_count;
            continue;
        }
        if (rx->pending[0].offset > expected)
        {
            uint32_t gap = (rx->pending[0].offset - expected) / ECOS_AUDIO_SAMPLES;
            if (!gap_waiting)
            {
                gap_waiting = true;
                gap_since = now;
            }
            if (gap <= 4 && now - gap_since < INTERCOM_REORDER_US)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (gap <= 4 && gap_blocks < 4)
            {
                int16_t silence[ECOS_AUDIO_SAMPLES] = {0};
                if (audio_write(silence, ECOS_AUDIO_SAMPLES) != ESP_OK) break;
                expected += ECOS_AUDIO_SAMPLES;
                ++gap_blocks;
                gap_since = now;
                gap_waiting = true;
                continue;
            }
            expected = rx->pending[0].offset; /* Large gap: resynchronize without a long silence. */
            gap_blocks = 0;
            gap_since = 0;
            gap_waiting = false;
            continue;
        }
        if (audio_write(rx->pending[0].pcm, ECOS_AUDIO_SAMPLES) != ESP_OK) break;
        played_audio = true;
        expected += ECOS_AUDIO_SAMPLES;
        gap_blocks = 0;
        gap_since = 0;
        gap_waiting = false;
        portENTER_CRITICAL(&s_mux);
        rx->played_offset = expected;
        portEXIT_CRITICAL(&s_mux);
        for (size_t i = 1; i < rx->pending_count; ++i) rx->pending[i - 1] = rx->pending[i];
        --rx->pending_count;
    }
    if (audio_started) audio_stop();
    link_event_text("intercom", played_audio ? "received" : audio_started ? "no_audio" : "speaker_error");
    /* End/failure is reported once; unreliable audio frames are never retried. */
    (void)rpc_control(rx->peer, "stop", rx->session_text, false);
    ecos_family_stream_close(rx->peer);
    if (held) ecos_family_hold(false);
    rx_unpublish_and_free(rx);
    vTaskDelete(NULL);
}

bool link_intercom_control(const char *from_id, const cJSON *json)
{
    if (!id_valid(from_id) || !cJSON_IsObject(json)) return false;
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "op"));
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    uint64_t session = 0;
    char token[17];
    if ((op != NULL && strcmp(op, "intercom") != 0) || action == NULL ||
        (strcmp(action, "listen") != 0 && strcmp(action, "stop") != 0) ||
        !parse_session(cJSON_GetObjectItemCaseSensitive(json, "session"), &session, token)) return false;
    int fields = 0;
    for (const cJSON *item = json->child; item != NULL; item = item->next)
    {
        ++fields;
        if (item->string == NULL || (strcmp(item->string, "op") != 0 &&
            strcmp(item->string, "action") != 0 && strcmp(item->string, "session") != 0)) return false;
    }
    if (fields != (op != NULL ? 3 : 2)) return false;
    bool audio_busy = link_audio_busy() || sound_alarm_playing();

    portENTER_CRITICAL(&s_mux);
    if (strcmp(action, "stop") == 0)
    {
        if (s_role == INTERCOM_RX && s_active != NULL)
        {
            intercom_rx_t *rx = s_active;
            if (rx->session == session && id_equal(rx->peer, from_id))
                rx->stop = true;
            else { portEXIT_CRITICAL(&s_mux); return false; }
        }
        else if (s_role == INTERCOM_TX && s_active != NULL)
        {
            intercom_tx_t *tx = s_active;
            if (tx->session == session && id_equal(tx->peer, from_id))
                tx->stop = true;
            else { portEXIT_CRITICAL(&s_mux); return false; }
        }
        else { portEXIT_CRITICAL(&s_mux); return false; }
        portEXIT_CRITICAL(&s_mux);
        return true;
    }

    if (s_role == INTERCOM_RX && s_active != NULL)
    {
        intercom_rx_t *rx = s_active;
        bool duplicate = rx->session == session && id_equal(rx->peer, from_id) &&
                         !rx->stop && !rx->closing;
        portEXIT_CRITICAL(&s_mux);
        return duplicate;
    }
    if (s_role != INTERCOM_NONE || audio_busy)
    {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    s_role = INTERCOM_CREATING_RX;
    s_cancel_creation = false;
    portEXIT_CRITICAL(&s_mux);

    intercom_rx_t *rx = heap_caps_calloc(1, sizeof(*rx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rx != NULL)
    {
        rx->queue_control = heap_caps_calloc(1, sizeof(*rx->queue_control), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        rx->queue_storage = heap_caps_calloc(INTERCOM_QUEUE_BLOCKS, sizeof(intercom_packet_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        rx->pending = heap_caps_calloc(INTERCOM_QUEUE_BLOCKS, sizeof(intercom_packet_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (rx == NULL || rx->queue_control == NULL || rx->queue_storage == NULL || rx->pending == NULL)
    {
        rx_queue_free(rx);
        claim_release(INTERCOM_RX);
        return false;
    }
    memcpy(rx->peer, from_id, sizeof(rx->peer));
    rx->session = session;
    memcpy(rx->session_text, token, sizeof(rx->session_text));
    rx->last_packet_us = esp_timer_get_time();
    rx->queue = xQueueCreateStatic(INTERCOM_QUEUE_BLOCKS, sizeof(intercom_packet_t),
                                   rx->queue_storage, rx->queue_control);
    if (rx->queue == NULL)
    {
        rx_queue_free(rx);
        claim_release(INTERCOM_RX);
        return false;
    }
    /* The transport delivers stream frames only for an opened target. Hold the
     * radio before opening, and publish the receive target before task startup. */
    ecos_family_hold(true);
    if (!ecos_family_stream_open(rx->peer))
    {
        ecos_family_hold(false);
        rx_queue_free(rx);
        claim_release(INTERCOM_RX);
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_cancel_creation)
    {
        portEXIT_CRITICAL(&s_mux);
        ecos_family_stream_close(rx->peer);
        ecos_family_hold(false);
        rx_queue_free(rx);
        claim_release(INTERCOM_RX);
        return false;
    }
    s_active = rx;
    s_role = INTERCOM_RX;
    portEXIT_CRITICAL(&s_mux);
    if (!sys_task_create(rx_worker, "intercom_rx", INTERCOM_TASK_STACK, rx, 5, SYS_CORE_ANY))
    {
        char peer[sizeof(rx->peer)];
        memcpy(peer, rx->peer, sizeof(peer));
        ecos_family_stream_close(peer);
        ecos_family_hold(false);
        rx_unpublish_and_free(rx);
        return false;
    }
    return true;
}

void link_intercom_receive(const char *from_id, const uint8_t *data, size_t length)
{
    intercom_rx_t *rx = NULL;
    portENTER_CRITICAL(&s_mux);
    if (s_role == INTERCOM_RX && s_active != NULL)
    {
        intercom_rx_t *current = s_active;
        if (!current->closing && id_equal(current->peer, from_id) &&
            !current->stop)
        {
            ++current->callback_refs;
            rx = current;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (rx == NULL) return;

    intercom_packet_t packet = {0};
    uint64_t session = 0;
    size_t samples = 0;
    bool valid = ecos_audio_decode(data, length, packet.pcm, ECOS_AUDIO_SAMPLES,
                                   &session, &packet.offset, &samples) &&
                 session == rx->session && samples == ECOS_AUDIO_SAMPLES &&
                 packet.offset % ECOS_AUDIO_SAMPLES == 0;
    bool enqueue = false;
    if (valid)
    {
        portENTER_CRITICAL(&s_mux);
        if (!rx->closing && s_active == rx && packet.offset >= rx->played_offset)
        {
            bool duplicate = false;
            for (size_t i = 0; i < INTERCOM_QUEUE_BLOCKS; ++i)
                if (rx->seen_offsets[i] == packet.offset + 1) duplicate = true;
            if (!duplicate)
            {
                rx->seen_offsets[rx->seen_next++ % INTERCOM_QUEUE_BLOCKS] = packet.offset + 1;
                rx->last_packet_us = esp_timer_get_time();
                enqueue = true;
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }
    if (enqueue) (void)xQueueSend(rx->queue, &packet, 0); /* queue operation outside the pointer lock */

    portENTER_CRITICAL(&s_mux);
    if (rx->callback_refs > 0) --rx->callback_refs;
    portEXIT_CRITICAL(&s_mux);
}

void link_intercom_init(void)
{
    portENTER_CRITICAL(&s_mux);
    bool register_handler = !s_handler_registered;
    if (register_handler) s_handler_registered = true;
    portEXIT_CRITICAL(&s_mux);
    if (register_handler) ecos_family_set_stream_handler(link_intercom_receive);
}
