#include "intercom_stubs/services/ecos.h"
#include "intercom_stubs/core/sys.h"
#include "intercom_stubs/freertos/queue.h"
#include "intercom_stubs/hardware/audio.h"
#include "intercom_stubs/hardware/microphone.h"
#include "intercom_stubs/services/sound.h"
#include "intercom_stubs/esp_heap_caps.h"
#include "intercom_stubs/esp_timer.h"
#include "intercom_stubs/freertos/task.h"
#include "../../../../components/services/ecos/ecos_audio_wire.h"
#include "../link_intercom.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t fake_now_us = 1000000;
static int lock_depth, hold_count, stream_open_count, stream_close_count, stream_send_count;
static int orbit_count, mic_start_count, mic_stop_count, mic_read_count;
static int audio_start_count, audio_stop_count, audio_write_count, silence_write_count;
static int scene_mic_count, task_delete_count, queue_delete_count, stream_handler_count;
static int random_value = 10;
static bool task_fail, stream_open_ok = true, stream_send_ok = true;
static bool microphone_ok = true, microphone_start_ok = true, audio_start_ok = true;
static bool audio_busy, alarm_playing;
static int orbit_mode; /* 0 queued/ok, 1 timeout, 2 ack without queued */
static int mic_fail_after = -1;
static size_t mic_chunk = ECOS_AUDIO_SAMPLES;
static bool mic_capacity_shrank;
static bool stop_during_orbit;
static bool stop_during_allocation;
void link_event_text(const char *type, const char *value)
{
    assert(lock_depth == 0 && strcmp(type, "intercom") == 0 && value != NULL);
}
static uint8_t last_stream[ECOS_AUDIO_FRAME_MAX];
static size_t last_stream_length;
static char last_peer[13];
static char last_orbit[100];
static TaskFunction_t queued_task;
static void *queued_arg;
static ecos_stream_handler_t stream_handler;
static int allocation_attempts, allocation_success, allocation_frees, fail_allocation;

void test_lock_enter(void) { ++lock_depth; }
void test_lock_exit(void) { assert(lock_depth > 0); --lock_depth; }

void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    ++allocation_attempts;
    if (stop_during_allocation) link_intercom_stop();
    if (fail_allocation == allocation_attempts) return NULL;
    void *p = calloc(n, size);
    if (p != NULL) ++allocation_success;
    return p;
}
void heap_caps_free(void *ptr)
{
    if (ptr != NULL) { ++allocation_frees; free(ptr); }
}

int64_t esp_timer_get_time(void) { return fake_now_us; }
uint32_t esp_random(void) { return (uint32_t)random_value++; }
bool link_audio_busy(void) { return audio_busy; }
bool sound_alarm_playing(void) { return alarm_playing; }
bool microphone_supported(void) { return microphone_ok; }
esp_err_t microphone_start(uint32_t rate, int channels)
{
    assert(lock_depth == 0 && rate == ECOS_AUDIO_RATE && channels == 1);
    ++mic_start_count;
    return microphone_start_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t microphone_read(int16_t *samples, size_t capacity, size_t *count)
{
    assert(lock_depth == 0 && capacity > 0 && capacity <= ECOS_AUDIO_SAMPLES);
    if (capacity < ECOS_AUDIO_SAMPLES) mic_capacity_shrank = true;
    ++mic_read_count;
    fake_now_us += 15000;
    if (mic_fail_after >= 0 && mic_read_count > mic_fail_after) return ESP_FAIL;
    size_t n = capacity < mic_chunk ? capacity : mic_chunk;
    for (size_t i = 0; i < n; ++i) samples[i] = (int16_t)(i * 13 + mic_read_count);
    *count = n;
    return ESP_OK;
}
void microphone_stop(void) { assert(lock_depth == 0); ++mic_stop_count; }

esp_err_t audio_start(uint32_t rate, int channels)
{
    assert(lock_depth == 0 && rate == ECOS_AUDIO_RATE && channels == 1);
    ++audio_start_count;
    return audio_start_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t audio_write(const int16_t *samples, size_t count)
{
    assert(lock_depth == 0 && samples != NULL && count == ECOS_AUDIO_SAMPLES);
    ++audio_write_count;
    bool silence = true;
    for (size_t i = 0; i < count; ++i) if (samples[i] != 0) { silence = false; break; }
    if (silence) ++silence_write_count;
    fake_now_us += 15000;
    return ESP_OK;
}
void audio_stop(void) { assert(lock_depth == 0); ++audio_stop_count; }

void ecos_family_hold(bool on) { assert(lock_depth == 0); hold_count += on ? 1 : -1; }
bool ecos_family_stream_open(const char *id)
{
    assert(lock_depth == 0);
    ++stream_open_count;
    snprintf(last_peer, sizeof(last_peer), "%s", id);
    return stream_open_ok;
}
void ecos_family_stream_close(const char *id)
{
    assert(lock_depth == 0 && strcmp(id, last_peer) == 0);
    ++stream_close_count;
}
bool ecos_family_stream_send(const char *id, const uint8_t *data, size_t length)
{
    assert(lock_depth == 0 && strcmp(id, last_peer) == 0 && length <= sizeof(last_stream));
    ++stream_send_count;
    memcpy(last_stream, data, length);
    last_stream_length = length;
    return stream_send_ok;
}
void ecos_family_set_stream_handler(ecos_stream_handler_t handler)
{
    stream_handler = handler;
    ++stream_handler_count;
}
int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms)
{
    assert(lock_depth == 0 && timeout_ms > 0 && length < sizeof(last_orbit));
    snprintf(last_peer, sizeof(last_peer), "%s", id);
    memcpy(last_orbit, request, length);
    last_orbit[length] = '\0';
    ++orbit_count;
    if (stop_during_orbit)
    {
        stop_during_orbit = false;
        link_intercom_stop();
    }
    if (orbit_mode == 1) return -1;
    const char *answer = orbit_mode == 2 ? "{\"ok\":true}" : "{\"ok\":true,\"queued\":true}";
    size_t n = strlen(answer);
    assert(n <= max);
    memcpy(reply, answer, n);
    return (int)n;
}
void link_scene_mic(bool on)
{
    assert(lock_depth == 0);
    scene_mic_count += on ? 1 : -1;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *queue)
{
    if (length == 0 || item_size == 0 || storage == NULL || queue == NULL) return NULL;
    *queue = (StaticQueue_t){.storage = storage, .item_size = item_size, .capacity = length};
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    if (queue == NULL || queue->deleted || item == NULL || queue->count == queue->capacity) return pdFALSE;
    size_t at = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->storage + at * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    if (queue == NULL || queue->deleted || item == NULL || queue->count == 0)
    {
        if (ticks != 0) fake_now_us += (int64_t)ticks * 1000;
        return pdFALSE;
    }
    memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue)
{
    if (queue != NULL) { queue->deleted = true; ++queue_delete_count; }
}

bool sys_task_create(TaskFunction_t fn, const char *name, uint32_t stack,
                     void *arg, UBaseType_t priority, int core)
{
    assert(fn != NULL && stack == 6144 && priority == 5 && core == SYS_CORE_ANY);
    assert(strcmp(name, "intercom_tx") == 0 || strcmp(name, "intercom_rx") == 0);
    if (task_fail) return false;
    assert(queued_task == NULL);
    queued_task = fn;
    queued_arg = arg;
    return true;
}
void vTaskDelay(TickType_t ticks) { fake_now_us += (int64_t)ticks * 1000; }
void vTaskDelete(void *task) { assert(task == NULL && lock_depth == 0); ++task_delete_count; }

#include "../link_intercom.c"

static cJSON *command(const char *action, const char *token)
{
    cJSON *json = cJSON_CreateObject();
    assert(json != NULL);
    cJSON_AddStringToObject(json, "op", "intercom");
    cJSON_AddStringToObject(json, "action", action);
    cJSON_AddStringToObject(json, "session", token);
    return json;
}

static void deliver(uint64_t session, uint32_t offset, const char *from, bool malformed)
{
    uint8_t packet[ECOS_AUDIO_FRAME_MAX];
    int16_t pcm[ECOS_AUDIO_SAMPLES];
    ima_adpcm_t state = {0};
    for (size_t i = 0; i < ECOS_AUDIO_SAMPLES; ++i) pcm[i] = (int16_t)(i * 7 + offset);
    size_t length = ecos_audio_encode(packet, sizeof(packet), session, offset, &state,
                                      pcm, ECOS_AUDIO_SAMPLES);
    assert(length == ECOS_AUDIO_FRAME_MAX);
    if (malformed) length--;
    link_intercom_receive(from, packet, length);
}

static void run_queued(void)
{
    assert(queued_task != NULL);
    TaskFunction_t fn = queued_task;
    void *arg = queued_arg;
    queued_task = NULL;
    queued_arg = NULL;
    fn(arg);
}

static void reset(void)
{
    assert(s_role == INTERCOM_NONE && s_active == NULL && queued_task == NULL);
    fake_now_us = 1000000;
    lock_depth = hold_count = stream_open_count = stream_close_count = stream_send_count = 0;
    orbit_count = mic_start_count = mic_stop_count = mic_read_count = 0;
    audio_start_count = audio_stop_count = audio_write_count = silence_write_count = 0;
    scene_mic_count = task_delete_count = queue_delete_count = stream_handler_count = 0;
    random_value = 10; task_fail = false; stream_open_ok = true; stream_send_ok = true;
    microphone_ok = true; microphone_start_ok = true; audio_start_ok = true;
    audio_busy = false; alarm_playing = false; orbit_mode = 0; mic_fail_after = -1;
    mic_chunk = ECOS_AUDIO_SAMPLES; mic_capacity_shrank = false; stop_during_orbit = false;
    stop_during_allocation = false;
    last_stream_length = 0; last_peer[0] = last_orbit[0] = '\0';
    allocation_attempts = allocation_success = allocation_frees = fail_allocation = 0;
    s_handler_registered = false;
    stream_handler = NULL;
}

static void test_start_bounds_task_failure_and_busy_claim(void)
{
    reset();
    assert(!link_intercom_start("000000000000", 1));
    assert(!link_intercom_start("001122334455", 0));
    assert(!link_intercom_start("001122334455", 31));
    assert(!link_intercom_busy() && queued_task == NULL);

    task_fail = true;
    assert(!link_intercom_start("AABBCCDDEEFF", 2));
    assert(!link_intercom_busy() && allocation_success == allocation_frees);
    task_fail = false;
    audio_busy = true;
    assert(!link_intercom_start("001122334455", 2));
    assert(!link_intercom_busy());
}

static void test_tx_stream_rpc_audio_and_cleanup(void)
{
    reset();
    mic_chunk = 73; /* Exercise accumulation across short microphone reads. */
    assert(link_intercom_start("AABBCCDDEEFF", 1));
    assert(link_intercom_busy() && strcmp(((intercom_tx_t *)s_active)->peer, "aabbccddeeff") == 0);
    run_queued();
    assert(hold_count == 0 && stream_open_count == 1 && stream_close_count == 1);
    assert(orbit_count == 2 && strstr(last_orbit, "\"action\":\"stop\"") != NULL);
    assert(mic_start_count == 1 && mic_stop_count == 1 && scene_mic_count == 0);
    assert(stream_send_count > 1 && last_stream_length == ECOS_AUDIO_FRAME_MAX && mic_capacity_shrank);
    int16_t decoded[ECOS_AUDIO_SAMPLES];
    uint64_t session;
    uint32_t offset;
    size_t samples;
    assert(ecos_audio_decode(last_stream, last_stream_length, decoded, ECOS_AUDIO_SAMPLES,
                             &session, &offset, &samples));
    assert(session != 0 && samples == ECOS_AUDIO_SAMPLES && offset > 0);
    assert(!link_intercom_busy() && task_delete_count == 1);
    assert(allocation_success == allocation_frees);
}

static void test_tx_unknown_ack_and_audio_faults_do_not_retry(void)
{
    reset();
    orbit_mode = 1;
    assert(link_intercom_start("001122334455", 1));
    run_queued();
    assert(orbit_count == 1 && mic_start_count == 0 && stream_open_count == 0 && stream_close_count == 0);
    assert(hold_count == 0 && !link_intercom_busy() && allocation_success == allocation_frees);

    reset();
    stream_open_ok = false; /* Peer accepted listen, but our channel is held elsewhere. */
    assert(link_intercom_start("001122334455", 1));
    run_queued();
    assert(orbit_count == 2 && strstr(last_orbit, "\"action\":\"stop\"") != NULL);
    assert(mic_start_count == 0 && stream_open_count == 1 && stream_close_count == 0);
    assert(hold_count == 0 && !link_intercom_busy() && allocation_success == allocation_frees);

    reset();
    mic_fail_after = 1;
    assert(link_intercom_start("001122334455", 1));
    run_queued();
    assert(mic_start_count == 1 && mic_stop_count == 1 && orbit_count == 2);
    assert(stream_send_count == 1 && hold_count == 0 && allocation_success == allocation_frees);

    reset();
    microphone_start_ok = false;
    assert(link_intercom_start("001122334455", 1));
    run_queued();
    assert(mic_start_count == 1 && mic_stop_count == 0 && orbit_count == 2);
    assert(hold_count == 0 && stream_close_count == 1 && allocation_success == allocation_frees);

    reset();
    stop_during_orbit = true;
    assert(link_intercom_start("001122334455", 2));
    run_queued();
    assert(mic_start_count == 0 && mic_stop_count == 0 && orbit_count == 2);
    assert(stream_open_count == 0 && stream_close_count == 0 && hold_count == 0 && allocation_success == allocation_frees);

    reset();
    stream_send_ok = false; /* Unreliable frame loss must end only after sustained failure. */
    assert(link_intercom_start("001122334455", 30));
    run_queued();
    assert(stream_send_count > 1 && mic_read_count >= 60 && mic_read_count < 100);
    assert(mic_stop_count == 1 && orbit_count == 2 && stream_close_count == 1);
    assert(hold_count == 0 && allocation_success == allocation_frees);
}

static void test_rx_control_dedup_auth_queue_and_playback(void)
{
    reset();
    link_intercom_init();
    link_intercom_init();
    assert(stream_handler == link_intercom_receive && stream_handler_count == 1);
    cJSON *listen = command("listen", "0000000000001234");
    assert(link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    assert(link_intercom_busy() && queued_task != NULL);
    listen = command("listen", "0000000000001234");
    assert(link_intercom_control("001122334455", listen)); /* duplicate request: one RX worker */
    cJSON_Delete(listen);
    assert(task_delete_count == 0);
    cJSON *stop = command("stop", "0000000000001235");
    assert(!link_intercom_control("001122334455", stop));
    cJSON_Delete(stop);
    stop = command("stop", "0000000000001234");
    assert(!link_intercom_control("001122334456", stop));
    cJSON_Delete(stop);

    deliver(0x1234, 0, "001122334456", false); /* wrong sender */
    deliver(0x1235, 0, "001122334455", false); /* wrong session */
    deliver(0x1234, 0, "001122334455", true);  /* malformed */
    deliver(0x1234, 0, "001122334455", false);
    deliver(0x1234, 480, "001122334455", false); /* out of order */
    deliver(0x1234, 240, "001122334455", false);
    deliver(0x1234, 240, "001122334455", false); /* duplicate offset */
    intercom_rx_t *rx = s_active;
    assert(rx != NULL && rx->queue->count == 3);
    run_queued();
    assert(audio_start_count == 1 && audio_stop_count == 1 && audio_write_count == 3);
    assert(orbit_count == 1 && strstr(last_orbit, "\"action\":\"stop\"") != NULL);
    assert(hold_count == 0 && !link_intercom_busy() && queue_delete_count == 1);
    assert(stream_open_count == 1 && stream_close_count == 1);
    assert(task_delete_count == 1 && allocation_success == allocation_frees);
}

static void test_rx_gap_limit_remote_stop_and_task_failure(void)
{
    reset();
    cJSON *listen = command("listen", "000000000000abcd");
    assert(link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    deliver(0xabcd, 0, "001122334455", false);
    deliver(0xabcd, 1200, "001122334455", false); /* four missing blocks -> bounded silence */
    run_queued();
    assert(audio_write_count == 6 && silence_write_count == 4);
    assert(!link_intercom_busy() && hold_count == 0 && allocation_success == allocation_frees);

    reset();
    listen = command("listen", "0000000000002222");
    assert(link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    cJSON *stop = command("stop", "0000000000002222");
    assert(link_intercom_control("001122334455", stop));
    cJSON_Delete(stop);
    run_queued();
    assert(audio_start_count == 0 && orbit_count == 1 && !link_intercom_busy());
    assert(hold_count == 0 && allocation_success == allocation_frees);

    reset();
    task_fail = true;
    listen = command("listen", "0000000000003333");
    assert(!link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    assert(!link_intercom_busy() && queued_task == NULL);
    assert(queue_delete_count == 1 && allocation_success == allocation_frees);
    assert(stream_close_count == 1 && hold_count == 0);

    reset();
    stream_open_ok = false;
    listen = command("listen", "0000000000004444");
    assert(!link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    assert(!link_intercom_busy() && queued_task == NULL && stream_close_count == 0);
    assert(hold_count == 0 && allocation_success == allocation_frees);
}

int main(void)
{
    test_start_bounds_task_failure_and_busy_claim();
    test_tx_stream_rpc_audio_and_cleanup();
    test_tx_unknown_ack_and_audio_faults_do_not_retry();
    test_rx_control_dedup_auth_queue_and_playback();
    test_rx_gap_limit_remote_stop_and_task_failure();
    reset();
    stop_during_allocation = true;
    assert(!link_intercom_start("001122334455", 10));
    assert(!link_intercom_busy() && queued_task == NULL && hold_count == 0 && allocation_success == allocation_frees);
    reset();
    stop_during_allocation = true;
    cJSON *listen = command("listen", "0000000000005555");
    assert(!link_intercom_control("001122334455", listen));
    cJSON_Delete(listen);
    assert(!link_intercom_busy() && queued_task == NULL && hold_count == 0 && allocation_success == allocation_frees);
    puts("Orbit intercom host tests passed");
    return 0;
}
