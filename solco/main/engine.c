/* Storage and synthesis live on core 0. The audio task never touches files. */
#include "engine.h"
#include "core/power.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hardware/audio.h"
#include "hardware/imu.h"
#include "mix.h"
#include "prepare.h"
#include "studio_motion.h"
#include "vinyl.h"
#include "wav.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAMES 256
#define PAGE 1024
#define PAGES 128
#define RING (PAGE * PAGES)
#ifndef SOLCO_LOOP_EDGE_PAGES
#define SOLCO_LOOP_EDGE_PAGES 16
#endif
_Static_assert(SOLCO_LOOP_EDGE_PAGES >= 4 && SOLCO_LOOP_EDGE_PAGES % 2 == 0,
               "Loop edges require an even number of pages");
#define TRACKS 32
#define RATE 44100
#define DEMO_FRAMES (RATE / 2)

extern const int16_t s_pulse_pcm[] asm("_binary_pulse_pcm_start");
extern const int16_t s_chord_pcm[] asm("_binary_chord_pcm_start");

typedef struct
{
    int16_t *pcm;
    uint16_t peaks[PAGES];
    uint32_t tags[PAGES];
    int16_t *loop_pcm;
    uint32_t loop_tags[SOLCO_LOOP_EDGE_PAGES];
    int16_t *loop_staging_pcm;
    uint32_t loop_staging_tags[SOLCO_LOOP_EDGE_PAGES], loop_request;
    bool loop_pending;
    solco_vinyl_t loop_proposal;
    uint32_t length;
    uint32_t generation;
    uint32_t miss_frame, miss_tag;
    FILE *file;
    solco_wav_t wav;
    solco_vinyl_t vinyl;
    bool demo;
    bool ready;
    bool playing;
    bool mp3, preparing;
    int track;
    char title[64];
} deck_t;

typedef struct
{
    int deck;
    int action;
    int track;
} command_t;
static const char *TAG = "solco";
static deck_t s_decks[2];
static char s_tracks[TRACKS][256];
static int s_track_count;
static QueueHandle_t s_commands;
/* All callers are tasks. A priority-inheriting mutex keeps I2S DMA interrupts
 * serviceable while WSOLA renders a grain; a spinlock would mask them. */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

void solco_engine_sync_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    configASSERT(s_lock);
}
static solco_status_t s_status = {.cross = 500, .volume = 0};
static int16_t s_a[FRAMES * 2], s_b[FRAMES * 2], s_out[FRAMES * 2];
static solco_mix_t s_mix;
static studio_t s_studio;
static studio_status_t s_studio_status;
static QueueHandle_t s_studio_commands;
static int16_t s_bus[FRAMES * 2];
static uint64_t s_anchor_frame;
static int64_t s_anchor_time;
static int s_motion_mode;
static unsigned s_motion_calibration;
static uint32_t s_studio_input_drops, s_studio_init_us;
static uint32_t s_motion_reads, s_motion_errors, s_motion_hits;
static int s_motion_learned, s_motion_pad;

bool solco_studio_submit_at(studio_event_t event, int64_t time_us)
{
    if (!s_studio_commands || event.type < STUDIO_HIT || event.type > STUDIO_KIT ||
        (event.type == STUDIO_HIT &&
         (event.value >= STUDIO_BANKS || !event.velocity || event.velocity > 127)))
    {
        return false;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    int64_t delta = time_us - s_anchor_time;
    event.frame = time_us > 0 && s_anchor_time
                      ? s_anchor_frame + (delta > 0 ? (uint64_t)delta * RATE / 1000000 : 0)
                      : 0;
    xSemaphoreGiveRecursive(s_lock);
    if (xQueueSend(s_studio_commands, &event, 0) == pdTRUE)
    {
        return true;
    }
    __atomic_add_fetch(&s_studio_input_drops, 1, __ATOMIC_RELAXED);
    return false;
}
void solco_studio_command(int type, int value, int velocity)
{
    studio_event_t event = {.type = type, .value = value, .velocity = velocity};
    solco_studio_submit_at(event, 0);
}
void solco_studio_local_status(studio_status_t *status)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    *status = s_studio_status;
    status->dropped += __atomic_load_n(&s_studio_input_drops, __ATOMIC_RELAXED);
    xSemaphoreGiveRecursive(s_lock);
}
void solco_studio_status(studio_status_t *status) { solco_studio_local_status(status); }
uint32_t solco_studio_init_us(void) { return s_studio_init_us; }
bool solco_studio_saved(void)
{
    studio_status_t state;
    solco_studio_local_status(&state);
    studio_pattern_t pattern;
    FILE *file = fopen("/sdcard/music/.solco-pattern.bin", "rb");
    if (!file)
    {
        return false;
    }
    bool ok = fread(&pattern, 1, sizeof(pattern), file) == sizeof(pattern);
    fclose(file);
    return ok && !memcmp(&pattern, &state.pattern, sizeof(pattern));
}
void solco_motion_calibrate(void)
{
    __atomic_add_fetch(&s_motion_calibration, 1, __ATOMIC_RELEASE);
    solco_motion_mode(2);
}
void solco_motion_mode(int mode)
{
    __atomic_store_n(&s_motion_mode, imu_supported() && mode >= 0 && mode <= 2 ? mode : 0,
                     __ATOMIC_RELEASE);
}
int solco_motion_get(void) { return __atomic_load_n(&s_motion_mode, __ATOMIC_ACQUIRE); }
void solco_motion_state(int *learned, int *pad)
{
    *learned = __atomic_load_n(&s_motion_learned, __ATOMIC_RELAXED);
    *pad = __atomic_load_n(&s_motion_pad, __ATOMIC_RELAXED);
}
void solco_motion_stats(uint32_t *reads, uint32_t *errors, uint32_t *hits)
{
    *reads = __atomic_load_n(&s_motion_reads, __ATOMIC_RELAXED);
    *errors = __atomic_load_n(&s_motion_errors, __ATOMIC_RELAXED);
    *hits = __atomic_load_n(&s_motion_hits, __ATOMIC_RELAXED);
}
static void motion_task(void *arg)
{
    (void)arg;
    studio_motion_t detector = {0};
    bool started = false;
    unsigned calibration = 0;
    int previous_mode = 0;
    for (;;)
    {
        int mode = solco_motion_get();
        if (mode && !started)
        {
            esp_err_t start_error = imu_start();
            started = start_error == ESP_OK;
            if (!started)
            {
                __atomic_add_fetch(&s_motion_errors, 1, __ATOMIC_RELAXED);
                ESP_LOGW(TAG, "IMU start failed: %s", esp_err_to_name(start_error));
                solco_motion_mode(0);
            }
            memset(&detector, 0, sizeof(detector));
        }
        if (!mode && started)
        {
            imu_stop();
            started = false;
        }
        unsigned requested = __atomic_load_n(&s_motion_calibration, __ATOMIC_ACQUIRE);
        if (requested != calibration || mode != previous_mode)
        {
            memset(&detector, 0, sizeof(detector));
            calibration = requested;
            previous_mode = mode;
        }
        if (mode && started)
        {
            imu_sample_t sample;
            esp_err_t result = imu_read(&sample);
            if (result == ESP_OK)
            {
                __atomic_add_fetch(&s_motion_reads, 1, __ATOMIC_RELAXED);
                int pad, velocity;
                const float *acceleration = sample.acceleration_g;
                if (studio_motion_detect(&detector, acceleration, sample.angular_dps,
                                         (uint32_t)(esp_timer_get_time() / 1000), mode, &pad,
                                         &velocity))
                {
                    __atomic_add_fetch(&s_motion_hits, 1, __ATOMIC_RELAXED);
                    solco_studio_command(STUDIO_HIT, pad, velocity);
                }
            }
            __atomic_store_n(&s_motion_learned, detector.learned, __ATOMIC_RELAXED);
            __atomic_store_n(&s_motion_pad, detector.selected_pad, __ATOMIC_RELAXED);
            if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED)
            {
                __atomic_add_fetch(&s_motion_errors, 1, __ATOMIC_RELAXED);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(mode ? 10 : 100));
    }
}

static void error_text(const char *text)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    snprintf(s_status.error, sizeof(s_status.error), "%s", text);
    xSemaphoreGiveRecursive(s_lock);
    ESP_LOGW(TAG, "%s", text);
}

static void scan(void)
{
    DIR *dir = opendir("/sdcard/music");
    if (!dir)
    {
        return;
    }
    struct dirent *entry;
    while (s_track_count < TRACKS && (entry = readdir(dir)) != NULL)
    {
        size_t n = strlen(entry->d_name);
        if (n > 4 &&
            (!strcasecmp(entry->d_name + n - 4, ".wav") ||
             !strcasecmp(entry->d_name + n - 4, ".mp3")) &&
            n < 240)
        {
            xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
            snprintf(s_tracks[s_track_count++], 256, "/sdcard/music/%.239s", entry->d_name);
            xSemaphoreGiveRecursive(s_lock);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "%d audio files in /sdcard/music", s_track_count);
}

static void reset_deck(int index, bool next)
{
    deck_t *d = &s_decks[index];
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    d->playing = false;
    bool reuse = !next && d->mp3 && d->file && d->ready;
    d->ready = false;
    d->preparing = false;
    ++d->generation;
    int32_t rate = d->vinyl.play_speed;
    solco_keylock_t *lock = d->vinyl.keylock;
    bool keylock_enabled = d->vinyl.keylock_enabled;
    if (lock) memset(lock, 0, sizeof(*lock));
    memset(&d->vinyl, 0, sizeof(d->vinyl));
    d->vinyl.play_speed = rate;
    d->vinyl.keylock = lock;
    d->vinyl.keylock_enabled = keylock_enabled;
    memset(d->tags, 0xff, sizeof(d->tags));
    memset(d->loop_tags, 0xff, sizeof(d->loop_tags));
    memset(d->loop_staging_tags, 0xff, sizeof(d->loop_staging_tags));
    d->loop_pending = false;
    ++d->loop_request;
    memset(d->peaks, 0, sizeof(d->peaks));
    s_status.error[0] = '\0';
    xSemaphoreGiveRecursive(s_lock);
    solco_prepare_cancel(index, d->generation);
    if (reuse)
    {
        return;
    }
    if (d->file)
    {
        fclose(d->file);
        d->file = NULL;
    }
    if (next)
    {
        d->track = (d->track + 1) % (s_track_count + 2);
    }
    bool demo = d->track < 2;
    uint32_t length = DEMO_FRAMES;
    bool ready = demo;
    d->mp3 = false;
    char title[64] = {0};
    if (demo)
    {
        snprintf(title, sizeof(title), "DEMO %s / 120 BPM", d->track == 0 ? "PULSE" : "CHORD");
    }
    else
    {
        const char *path = s_tracks[d->track - 2];
        d->mp3 = !strcasecmp(path + strlen(path) - 4, ".mp3");
        if (d->mp3)
        {
            xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
            d->demo = false;
            d->length = 0;
            d->preparing = true;
            snprintf(d->title, sizeof(d->title), "%.63s", path + 14);
            xSemaphoreGiveRecursive(s_lock);
            if (!solco_prepare_request(index, d->generation, path))
            {
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                d->preparing = false;
                xSemaphoreGiveRecursive(s_lock);
                error_text("Preparazione occupata: riprova");
            }
            return;
        }
        d->file = fopen(path, "rb");
        if (!d->file || !solco_wav_open(d->file, &d->wav))
        {
            if (d->file)
            {
                fclose(d->file);
                d->file = NULL;
            }
            length = 0;
            error_text("File non valido: WAV PCM16 44.1 kHz");
        }
        else
        {
            length = d->wav.bytes / (d->wav.channels * 2);
            ESP_LOGI(TAG, "Deck %c: %s (%lu frames)", 'A' + index, path, (unsigned long)length);
        }
        snprintf(title, sizeof(title), "%.63s", path + 14);
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    d->demo = demo;
    d->length = length;
    d->ready = ready;
    memcpy(d->title, title, sizeof(title));
    xSemaphoreGiveRecursive(s_lock);
}

/* Bidirectional page cache, filled outside the audio task. Each tag identifies
 * an absolute WAV page; modulo placement never aliases stale samples. */
static void fill(int index)
{
    deck_t *d = &s_decks[index];
    if (d->demo || !d->file || !d->length)
    {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    uint32_t current = (uint32_t)(d->vinyl.cursor / SOLCO_SAMPLE_ONE) / PAGE;
    uint32_t total = (d->length + PAGE - 1) / PAGE;
    if (current >= total)
    {
        current = total - 1;
    }
    uint32_t start = current > PAGES / 2 - 1 ? current - (PAGES / 2 - 1) : 0;
    if (total > PAGES && start > total - PAGES)
    {
        start = total - PAGES;
    }
    uint32_t wanted = UINT32_MAX;
    int edge_slot = -1;
    bool staging = d->loop_pending;
    uint32_t request = d->loop_request;
    solco_vinyl_t *range = staging ? &d->loop_proposal : &d->vinyl;
    uint32_t *edge_tags = staging ? d->loop_staging_tags : d->loop_tags;
    if (range->loop_marked)
    {
        uint32_t head = range->loop_in / PAGE;
        uint32_t tail = range->loop_out > range->loop_in ? (range->loop_out - 1) / PAGE : head;
        uint32_t edges[SOLCO_LOOP_EDGE_PAGES];
        for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES / 2; ++i)
        {
            edges[i] = head + i < total ? head + i : total - 1;
            edges[i + SOLCO_LOOP_EDGE_PAGES / 2] = tail >= (uint32_t)i ? tail - i : 0;
        }
        for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES; ++i)
        {
            if (edge_tags[i] != edges[i])
            {
                wanted = edges[i];
                edge_slot = i;
                break;
            }
        }
    }
    for (int i = 0; wanted == UINT32_MAX && i < PAGES * 2; ++i)
    {
        int delta = (i + 1) / 2;
        int64_t page = (int64_t)current + (i % 2 ? delta : -delta);
        if (page < start || page >= total || page >= start + PAGES)
        {
            continue;
        }
        if (d->tags[page % PAGES] != (uint32_t)page)
        {
            wanted = (uint32_t)page;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    if (wanted == UINT32_MAX)
    {
        return;
    }
    int16_t pcm[PAGE * 2];
    uint32_t first = wanted * PAGE;
    size_t frames = d->length - first < PAGE ? d->length - first : PAGE;
    size_t unit = d->wav.channels * sizeof(int16_t);
    if (fseek(d->file, (long)(d->wav.offset + first * unit), SEEK_SET) != 0 ||
        fread(pcm, unit, frames, d->file) != frames)
    {
        error_text("Errore lettura microSD");
        fclose(d->file);
        d->file = NULL;
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        d->playing = false;
        d->ready = false;
        d->vinyl.held = false;
        xSemaphoreGiveRecursive(s_lock);
        return;
    }
    if (d->wav.channels == 1)
    {
        for (size_t i = frames; i-- > 0;)
        {
            pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
        }
    }
    uint16_t peak = 0;
    for (size_t i = 0; i < frames * 2; ++i)
    {
        uint16_t magnitude = (uint16_t)(pcm[i] < 0 ? -(int)pcm[i] : pcm[i]);
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (edge_slot >= 0)
    {
        if (staging)
        {
            if (d->loop_pending && d->loop_request == request)
            {
                memcpy(d->loop_staging_pcm + edge_slot * PAGE * 2, pcm,
                       frames * 2 * sizeof(int16_t));
                d->loop_staging_tags[edge_slot] = wanted;
                bool complete = true;
                for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES; ++i)
                    if (d->loop_staging_tags[i] == UINT32_MAX)
                        complete = false;
                if (complete)
                {
                    /* Publish only once every new edge page is available. The old
                     * loop keeps playing during SD reads; no uncached midpoint jump. */
                    /* Both buffers have identical capacity. Publish by swapping
                     * ownership, avoiding a 64 KiB copy in the audio mutex. */
                    int16_t *previous = d->loop_pcm;
                    d->loop_pcm = d->loop_staging_pcm;
                    d->loop_staging_pcm = previous;
                    memcpy(d->loop_tags, d->loop_staging_tags, sizeof(d->loop_tags));
                    d->vinyl.loop_in = d->loop_proposal.loop_in;
                    d->vinyl.loop_out = d->loop_proposal.loop_out;
                    d->vinyl.loop_enabled = d->loop_proposal.loop_enabled;
                    d->vinyl.loop_marked = d->loop_proposal.loop_marked;
                    int64_t low = (int64_t)d->vinyl.loop_in * SOLCO_SAMPLE_ONE;
                    int64_t high = (int64_t)d->vinyl.loop_out * SOLCO_SAMPLE_ONE;
                    if (!d->vinyl.held && (d->vinyl.cursor < low || d->vinyl.cursor >= high))
                    {
                        d->vinyl.cursor = d->vinyl.target = low;
                        d->vinyl.gate = 0;
                    }
                    d->loop_pending = false;
                }
            }
        }
        else
        {
            memcpy(d->loop_pcm + edge_slot * PAGE * 2, pcm, frames * 2 * sizeof(int16_t));
            d->loop_tags[edge_slot] = wanted;
        }
        xSemaphoreGiveRecursive(s_lock);
        return;
    }
    uint32_t slot = wanted % PAGES;
    memcpy(d->pcm + slot * PAGE * 2, pcm, frames * 2 * sizeof(int16_t));
    d->peaks[slot] = peak;
    d->tags[slot] = wanted;
    if (wanted == 0)
    {
        d->ready = true;
    }
    xSemaphoreGiveRecursive(s_lock);
}

/* USB stdout may block for 50 ms when the host stops reading. Never log in audio. */
static void log_status(void *arg)
{
    (void)arg;
    solco_status_t status;
    solco_status(&status);
    ESP_LOGI(
        TAG, "blocks=%lu mix_max=%luus empty=%lu errors=%lu clips=%lu buffer=%lu/%lu pos=%lu/%lu",
        (unsigned long)status.blocks, (unsigned long)status.mix_us, (unsigned long)status.underruns,
        (unsigned long)status.audio_errors, (unsigned long)status.clips,
        (unsigned long)status.buffered[0], (unsigned long)status.buffered[1],
        (unsigned long)status.frames[0], (unsigned long)status.frames[1]);
}

static void storage_task(void *arg)
{
    (void)arg;
    uint32_t saved_revision = 0;
    int64_t next_save = 0;
    mkdir("/sdcard/music", 0775);
    scan();
    reset_deck(0, false);
    reset_deck(1, false);
    int64_t last_log = esp_timer_get_time();
    for (;;)
    {
        studio_status_t studio;
        solco_studio_local_status(&studio);
        if (studio.revision != saved_revision && esp_timer_get_time() >= next_save)
        {
            next_save = esp_timer_get_time() + 2000000;
            FILE *pattern = fopen("/sdcard/music/.solco-pattern.pending", "wb");
            if (pattern)
            {
                bool ok = fwrite(&studio.pattern, 1, sizeof(studio.pattern), pattern) ==
                          sizeof(studio.pattern);
                if (fflush(pattern) != 0)
                {
                    ok = false;
                }
                if (fclose(pattern) != 0)
                {
                    ok = false;
                }
                if (ok)
                {
                    unlink("/sdcard/music/.solco-pattern.bak");
                    rename("/sdcard/music/.solco-pattern.bin", "/sdcard/music/.solco-pattern.bak");
                }
                if (ok && rename("/sdcard/music/.solco-pattern.pending",
                                 "/sdcard/music/.solco-pattern.bin") == 0)
                {
                    saved_revision = studio.revision;
                }
            }
        }
        solco_prepared_t prepared;
        while (solco_prepare_poll(&prepared))
        {
            deck_t *d = &s_decks[prepared.deck];
            if (d->generation != prepared.generation || !d->preparing)
            {
                continue;
            }
            if (prepared.error[0])
            {
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                d->preparing = false;
                xSemaphoreGiveRecursive(s_lock);
                error_text(prepared.error);
                continue;
            }
            FILE *file = fopen(prepared.path, "rb");
            solco_wav_t wav;
            bool valid = file && solco_wav_open(file, &wav);
            if (!valid && file)
            {
                fclose(file);
            }
            if (valid)
            {
                d->file = file;
                d->wav = wav;
            }
            xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
            d->preparing = false;
            d->length = valid ? wav.bytes / (wav.channels * 2) : 0;
            xSemaphoreGiveRecursive(s_lock);
            if (!valid)
            {
                error_text("Cache PCM non disponibile");
            }
        }
        command_t command;
        while (xQueueReceive(s_commands, &command, 0) == pdTRUE)
        {
            if (command.action == 3 || command.action == 5 || command.action == 6)
            {
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                bool busy = s_decks[0].playing || s_decks[1].playing || s_decks[0].vinyl.held ||
                            s_decks[1].vinyl.held ||
                            (command.action == 6 && (s_decks[0].preparing || s_decks[1].preparing));
                xSemaphoreGiveRecursive(s_lock);
                if (busy)
                {
                    error_text("Pausa i deck prima di rileggere");
                    continue;
                }
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                s_track_count = 0;
                xSemaphoreGiveRecursive(s_lock);
                scan();
                if (command.action == 6)
                {
                    /* Close both cache handles before forgetting only our test entry. */
                    for (int i = 0; i < 2; ++i)
                    {
                        s_decks[i].track = i;
                        s_decks[i].mp3 = false;
                        reset_deck(i, false);
                    }
                    const char *cache =
                        "/sdcard/music/.solco/"
                        "p1-5e339c95ac562e756875e61c7ec56ec6ef16adc6af54cf2c5e8f0b9a01f5f2fb.wav";
                    remove(cache);
                }
                s_decks[0].track = 0;
                s_decks[1].track = 1;
                for (int i = 0; i < s_track_count; ++i)
                {
                    if (strstr(s_tracks[i], command.action != 3
                                                ? "/Solco-import-test-48k.mp3"
                                                : "/Solco-original-scratch-test.wav"))
                    {
                        s_decks[0].track = i + 2;
                    }
                    if (command.action == 6 &&
                        strstr(s_tracks[i], "/Solco-original-scratch-test.wav"))
                    {
                        s_decks[1].track = i + 2;
                    }
                }
                reset_deck(0, false);
                reset_deck(1, false);
                continue;
            }
            if (command.action == 0)
            {
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                if (s_decks[command.deck].ready)
                {
                    s_decks[command.deck].playing = !s_decks[command.deck].playing;
                }
                xSemaphoreGiveRecursive(s_lock);
            }
            else
            {
                xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                bool playing = s_decks[command.deck].playing || s_decks[command.deck].vinyl.held;
                xSemaphoreGiveRecursive(s_lock);
                if ((command.action == 2 || command.action == 4) && playing)
                {
                    error_text("Pausa prima di FILE");
                    continue;
                }
                if (command.action == 4)
                {
                    if (command.track < 0 || command.track >= s_track_count + 2)
                    {
                        continue;
                    }
                    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
                    s_decks[command.deck].ready = false;
                    s_decks[command.deck].track = command.track;
                    xSemaphoreGiveRecursive(s_lock);
                }
                reset_deck(command.deck, command.action == 2);
            }
        }
        fill(0);
        fill(1);
        if (esp_timer_get_time() - last_log > 10000000)
        {
            sys_post(log_status, NULL);
            last_log = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Caller holds the short state lock; no filesystem or allocation here. */
static bool sample(void *source, int64_t frame, int16_t stereo[2])
{
    deck_t *d = source;
    if (d->demo)
    {
        int64_t at = frame;
        if (at < 0 || at >= DEMO_FRAMES) at %= DEMO_FRAMES;
        if (at < 0)
        {
            at += DEMO_FRAMES;
        }
        const int16_t *loop = d->track == 0 ? s_pulse_pcm : s_chord_pcm;
        stereo[0] = stereo[1] = loop[at];
        return true;
    }
    if (frame < 0 || frame >= d->length)
    {
        d->miss_frame = (uint32_t)frame;
        d->miss_tag = UINT32_MAX;
        return false;
    }
    uint32_t page = (uint32_t)frame / PAGE;
    /* Most WSOLA reads hit the ordinary cache. Avoid scanning all boundary
     * pages for every sample; the reserved edges are a fallback after wraps. */
    if (d->tags[page % PAGES] == page)
    {
        size_t at = ((size_t)frame % RING) * 2;
        stereo[0] = d->pcm[at];
        stereo[1] = d->pcm[at + 1];
        return true;
    }
    if (d->vinyl.loop_enabled)
    {
        for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES; ++i)
        {
            if (d->loop_tags[i] == page)
            {
                size_t at = (i * PAGE + (uint32_t)frame % PAGE) * 2;
                stereo[0] = d->loop_pcm[at];
                stereo[1] = d->loop_pcm[at + 1];
                return true;
            }
        }
    }
    d->miss_frame = (uint32_t)frame;
    d->miss_tag = d->tags[page % PAGES];
    return false;
}

static bool take(deck_t *d, int16_t *out)
{
    solco_vinyl_tick(&d->vinyl, (uint32_t)(esp_timer_get_time() / 1000));
    bool missing = false;
    if (!d->ready || (!d->playing && !d->vinyl.held && d->vinyl.gate < 0.0001f))
    {
        memset(out, 0, FRAMES * 4);
        return false;
    }
    bool active = solco_vinyl_render(&d->vinyl, out, FRAMES, d->playing, d->length, d->demo, sample,
                                     d, &missing);
    if (missing)
    {
        ++s_status.underruns;
    }
    if (!d->demo && !d->vinyl.held && !d->vinyl.loop_enabled &&
        ((d->vinyl.play_speed >= 0 && d->vinyl.cursor >= (int64_t)d->length * SOLCO_SAMPLE_ONE) ||
         (d->vinyl.play_speed < 0 && d->vinyl.cursor <= 0)))
    {
        d->playing = false;
    }
    return active;
}

static void audio_task(void *arg)
{
    (void)arg;
    power_cpu_boost(true);
    esp_err_t err = audio_start(RATE, SOLCO_OUTPUT_CHANNELS);
    if (err != ESP_OK)
    {
        error_text("Uscita audio non disponibile");
        power_cpu_boost(false);
        vTaskDelete(NULL);
        return;
    }
    /* A single user-facing digital fader; no hidden -22.5 dB codec attenuation. */
    err = audio_set_volume(100);
    if (err != ESP_OK)
    {
        error_text("Regolazione uscita non disponibile");
        audio_stop();
        power_cpu_boost(false);
        vTaskDelete(NULL);
        return;
    }
    uint32_t generation[2] = {0};
    int64_t last_write = 0;
    for (;;)
    {
        int64_t start = esp_timer_get_time();
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        /* A cue/load starts a new timeline: never drain its first samples
         * using the previous source's pause fade. Only the audio task owns mix. */
        if (generation[0] != s_decks[0].generation)
        {
            generation[0] = s_decks[0].generation;
            s_mix.a = 0;
        }
        if (generation[1] != s_decks[1].generation)
        {
            generation[1] = s_decks[1].generation;
            s_mix.b = 0;
        }
        bool a = take(&s_decks[0], s_a);
        bool b = take(&s_decks[1], s_b);
        int cross = s_status.cross, volume = s_status.volume;
        xSemaphoreGiveRecursive(s_lock);
        studio_event_t event;
        while (xQueueReceive(s_studio_commands, &event, 0) == pdTRUE)
        {
            studio_push(&s_studio, event);
        }
        studio_render(&s_studio, s_bus, FRAMES);
        solco_mix_studio(&s_mix, s_a, s_b, s_bus, s_out, FRAMES, cross, volume, a, b,
                         s_studio.status.enabled || s_studio.status.playing ||
                             s_studio.status.voices);
#if SOLCO_OUTPUT_CHANNELS == 1
        s_mix.peak = solco_fold_mono(s_out, FRAMES);
#endif
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
        err = audio_write(s_out, FRAMES * SOLCO_OUTPUT_CHANNELS);
        int64_t written = esp_timer_get_time();
        uint32_t cycle = last_write ? (uint32_t)(written - last_write) : 0;
        last_write = written;
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        uint16_t released_peak = (uint16_t)((uint32_t)s_status.peak * 985 / 1000);
        s_status.peak = s_mix.peak > released_peak ? s_mix.peak : released_peak;
        s_status.clips = s_mix.clipped;
        if (elapsed > s_status.mix_us)
        {
            s_status.mix_us = elapsed;
        }
        /* Diagnostic bound, not an analog underrun detector. Exclude initial DMA priming. */
        if (s_status.blocks > 100)
        {
            if (cycle > s_status.cycle_us)
            {
                s_status.cycle_us = cycle;
            }
            if (cycle > 17415)
            {
                ++s_status.late_cycles;
            }
        }
        s_studio_status = s_studio.status;
        s_anchor_frame = s_studio.status.frame;
        s_anchor_time = written;
        ++s_status.blocks;
        if (err != ESP_OK)
        {
            ++s_status.audio_errors;
        }
        xSemaphoreGiveRecursive(s_lock);
        if (err != ESP_OK)
        {
            error_text("Errore uscita audio: riproduzione fermata");
            audio_stop();
            power_cpu_boost(false);
            vTaskDelete(NULL);
            return;
        }
    }
}

esp_err_t solco_engine_start(void)
{
    for (int i = 0; i < 2; ++i)
    {
        s_decks[i].vinyl.keylock = heap_caps_calloc(1, sizeof(solco_keylock_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_decks[i].vinyl.keylock_enabled = true;
        s_decks[i].loop_staging_pcm =
            heap_caps_calloc(PAGE * SOLCO_LOOP_EDGE_PAGES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        memset(s_decks[i].loop_staging_tags, 0xff, sizeof(s_decks[i].loop_staging_tags));
        s_decks[i].loop_pcm =
            heap_caps_calloc(PAGE * SOLCO_LOOP_EDGE_PAGES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        memset(s_decks[i].loop_tags, 0xff, sizeof(s_decks[i].loop_tags));
        s_decks[i].pcm =
            heap_caps_calloc(RING * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_decks[i].vinyl.keylock || !s_decks[i].pcm || !s_decks[i].loop_pcm || !s_decks[i].loop_staging_pcm)
        {
            for (int j = 0; j <= i; ++j)
            {
                free(s_decks[j].vinyl.keylock);
                s_decks[j].vinyl.keylock = NULL;
                free(s_decks[j].loop_staging_pcm);
                s_decks[j].loop_staging_pcm = NULL;
                free(s_decks[j].loop_pcm);
                s_decks[j].loop_pcm = NULL;
                free(s_decks[j].pcm);
                s_decks[j].pcm = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_decks[i].track = i;
    }
    power_cpu_boost(true);
    int64_t studio_start = esp_timer_get_time();
    bool studio_ready = studio_init(&s_studio);
    s_studio_init_us = (uint32_t)(esp_timer_get_time() - studio_start);
    power_cpu_boost(false);
    if (!studio_ready)
    {
        return ESP_ERR_NO_MEM;
    }
    FILE *pattern = fopen("/sdcard/music/.solco-pattern.bin", "rb");
    if (!pattern)
    {
        pattern = fopen("/sdcard/music/.solco-pattern.bak", "rb");
    }
    if (pattern)
    {
        studio_pattern_t data;
        if (fread(&data, 1, sizeof(data), pattern) == sizeof(data))
        {
            studio_restore(&s_studio, &data);
        }
        fclose(pattern);
    }
    s_studio_status = s_studio.status;
    s_studio_commands = xQueueCreate(64, sizeof(studio_event_t));
    if (!s_studio_commands)
    {
        return ESP_ERR_NO_MEM;
    }
    if (imu_supported() && !sys_task_create(motion_task, "solco_motion", 4096, NULL, 2, 0))
    {
        return ESP_ERR_NO_MEM;
    }
    s_commands = xQueueCreate(16, sizeof(command_t));
    if (!s_commands)
    {
        return ESP_ERR_NO_MEM;
    }
    if (!sys_task_create(storage_task, "solco_sd", 10240, NULL, 4, 0))
    {
        return ESP_ERR_NO_MEM;
    }
    if (!sys_task_create(audio_task, "solco_audio", 4096, NULL, 6, 1))
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool command(int deck, int action)
{
    if (deck < 0 || deck > 1 || !s_commands)
    {
        return false;
    }
    command_t cmd = {.deck = deck, .action = action};
    if (xQueueSend(s_commands, &cmd, 0) == pdTRUE)
    {
        return true;
    }
    error_text("Comandi occupati: riprova");
    return false;
}

bool solco_toggle(int deck) { return command(deck, 0); }
bool solco_cue(int deck) { return command(deck, 1); }
bool solco_next(int deck) { return command(deck, 2); }

void solco_scratch(int deck, int phase, int32_t frames)
{
    if (deck < 0 || deck > 1 || phase < 0 || phase > 2)
    {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    deck_t *d = &s_decks[deck];
    if (d->ready)
    {
        solco_vinyl_touch(&d->vinyl, phase, frames, d->length, d->demo);
    }
    xSemaphoreGiveRecursive(s_lock);
}

void solco_jog(int deck, int steps)
{
    if (deck < 0 || deck > 1)
    {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    deck_t *d = &s_decks[deck];
    if (d->ready)
    {
        solco_vinyl_jog(&d->vinyl, steps, (uint32_t)(esp_timer_get_time() / 1000), d->length,
                        d->demo);
    }
    xSemaphoreGiveRecursive(s_lock);
}

bool solco_loop(int deck, int action)
{
    if (deck < 0 || deck > 1)
        return false;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    deck_t *d = &s_decks[deck];
    solco_vinyl_t proposal = d->vinyl;
    if (d->loop_pending)
    {
        proposal.loop_in = d->loop_proposal.loop_in;
        proposal.loop_out = d->loop_proposal.loop_out;
        proposal.loop_enabled = d->loop_proposal.loop_enabled;
        proposal.loop_marked = d->loop_proposal.loop_marked;
    }
    bool ok = d->ready && !d->vinyl.held && solco_vinyl_loop(&proposal, action, d->length);
    if (ok)
    {
        ++d->loop_request;
        if (d->demo || !proposal.loop_enabled || action == 0)
        {
            d->vinyl = proposal;
            d->loop_pending = false;
        }
        else
        {
            d->loop_proposal = proposal;
            d->loop_pending = true;
            memset(d->loop_staging_tags, 0xff, sizeof(d->loop_staging_tags));
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

bool solco_keylock(int deck, bool enabled)
{
    if (deck < 0 || deck > 1) return false;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_decks[deck].vinyl.keylock_enabled = enabled;
    xSemaphoreGiveRecursive(s_lock);
    return true;
}

bool solco_rate(int deck, int32_t rate)
{
    if (deck < 0 || deck > 1 ||
        (rate != 32768 && rate != 65536 && rate != 98304 && rate != 131072 && rate != -32768 &&
         rate != -65536 && rate != -98304 && rate != -131072))
        return false;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    deck_t *d = &s_decks[deck];
    d->vinyl.play_speed = rate;
    if (!d->playing && !d->vinyl.held)
    {
        d->vinyl.transport_rate = rate;
        d->vinyl.rate_initialized = true;
    }
    xSemaphoreGiveRecursive(s_lock);
    return true;
}

void solco_cross(int value)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_status.cross = value < 0 ? 0 : value > 1000 ? 1000 : value;
    xSemaphoreGiveRecursive(s_lock);
}

void solco_volume(int value)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_status.volume = value < 0 ? 0 : value > 100 ? 100 : value;
    xSemaphoreGiveRecursive(s_lock);
}

void solco_status(solco_status_t *status)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    *status = s_status;
    for (int i = 0; i < 2; ++i)
    {
        status->playing[i] = s_decks[i].playing;
        deck_t *d = &s_decks[i];
        int64_t position = d->vinyl.cursor / SOLCO_SAMPLE_ONE;
        status->frames[i] = position < 0 ? 0 : (uint32_t)position;
        int64_t phase = position % (RATE * 18);
        status->platter_phase[i] = (uint32_t)(phase < 0 ? phase + RATE * 18 : phase);
        status->looping[i] = d->vinyl.loop_enabled;
        status->loop_marked[i] = d->vinyl.loop_marked;
        status->loop_in[i] = d->vinyl.loop_in;
        status->loop_out[i] = d->vinyl.loop_out;
        status->scratching[i] = d->vinyl.held;
        status->ready[i] = d->ready;
        status->preparing[i] = d->preparing;
        status->prepare_percent[i] = solco_prepare_percent(i);
        status->speed[i] = d->vinyl.speed;
        status->keylock[i] = d->vinyl.keylock_enabled;
        status->rate[i] = d->vinyl.play_speed ? d->vinyl.play_speed : 65536;
        status->buffered[i] = 0;
        status->miss_frame[i] = d->miss_frame;
        status->miss_tag[i] = d->miss_tag;
        memcpy(status->title[i], d->title, sizeof(status->title[i]));
        for (uint32_t j = 0; j < 64; ++j)
        {
            uint32_t page = (status->frames[i] + j * PAGE / 2) / PAGE;
            uint32_t slot = page % PAGES;
            if (d->demo)
            {
                uint16_t peak = 0;
                const int16_t *loop = d->track == 0 ? s_pulse_pcm : s_chord_pcm;
                /* Sparse preview only; no analysis in the audio task. */
                for (int k = 0; k < 16; ++k)
                {
                    int16_t value = loop[(status->frames[i] + j * PAGE / 2 + k * 32) % DEMO_FRAMES];
                    uint16_t magnitude = value < 0 ? -(int)value : value;
                    if (magnitude > peak)
                    {
                        peak = magnitude;
                    }
                }
                status->wave[i][j] = peak;
            }
            else
            {
                status->wave[i][j] = d->tags[slot] == page ? d->peaks[slot] : 0;
            }
        }
        for (int j = 0; j < PAGES; ++j)
        {
            if (d->tags[j] != UINT32_MAX)
            {
                status->buffered[i] += PAGE;
            }
        }
        if (d->demo)
        {
            status->buffered[i] = DEMO_FRAMES;
        }
    }
    xSemaphoreGiveRecursive(s_lock);
}

#if CONFIG_WATCH_DEV_CONSOLE
/* Bounded, original test material for real SD/cache tests. Never overwrite. */
bool solco_test_fixture(void)
{
    const char *path = "/sdcard/music/Solco-original-scratch-test.wav";
    mkdir("/sdcard/music", 0775);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file)
    {
        close(fd);
        remove(path);
        return false;
    }
    const uint32_t frames = RATE * 12;
    uint32_t bytes = frames * 4;
    unsigned char header[44] = {'R', 'I', 'F',  'F',  0,   0,   0,    0,    'W', 'A', 'V',
                                'E', 'f', 'm',  't',  ' ', 16,  0,    0,    0,   1,   0,
                                2,   0,   0x44, 0xac, 0,   0,   0x10, 0xb1, 2,   0,   4,
                                0,   16,  0,    'd',  'a', 't', 'a',  0,    0,   0,   0};
    for (int i = 0; i < 4; ++i)
    {
        header[4 + i] = (unsigned char)((bytes + 36) >> (i * 8));
        header[40 + i] = (unsigned char)(bytes >> (i * 8));
    }
    bool ok = fwrite(header, 1, 44, file) == 44;
    int16_t pcm[FRAMES * 2];
    for (uint32_t at = 0; ok && at < frames; at += FRAMES)
    {
        uint32_t count = frames - at < FRAMES ? frames - at : FRAMES;
        for (uint32_t i = 0; i < count; ++i)
        {
            pcm[i * 2] = s_pulse_pcm[(at + i) % DEMO_FRAMES];
            pcm[i * 2 + 1] = s_chord_pcm[(at + i) % DEMO_FRAMES];
        }
        ok = fwrite(pcm, 4, count, file) == count;
    }
    ok = fflush(file) == 0 && ok;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (!ok)
    {
        remove(path);
        return false;
    }
    return true;
}
bool solco_test_load(void) { return command(0, 3); }
#endif

int solco_library_count(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    int count = s_track_count + 2;
    xSemaphoreGiveRecursive(s_lock);
    return count;
}
void solco_library_title(int track, char *title, size_t length)
{
    if (!length)
    {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (track < 2)
    {
        snprintf(title, length, "%s", track == 0 ? "DEMO PULSE" : "DEMO CHORD");
    }
    else if (track < s_track_count + 2)
    {
        snprintf(title, length, "%s", s_tracks[track - 2] + 14);
    }
    else
    {
        title[0] = '\0';
    }
    xSemaphoreGiveRecursive(s_lock);
}
bool solco_select(int deck, int track)
{
    if (deck < 0 || deck > 1 || !s_commands)
    {
        return false;
    }
    command_t request = {.deck = deck, .action = 4, .track = track};
    return xQueueSend(s_commands, &request, 0) == pdTRUE;
}

#if CONFIG_WATCH_DEV_CONSOLE
bool solco_test_mp3_fixture(void)
{
    extern const unsigned char start[] asm("_binary_import_test_mp3_start");
    extern const unsigned char end[] asm("_binary_import_test_mp3_end");
    const char *path = "/sdcard/music/Solco-import-test-48k.mp3";
    mkdir("/sdcard/music", 0775);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file)
    {
        close(fd);
        remove(path);
        return false;
    }
    size_t length = (size_t)(end - start);
    unsigned char chunk[512];
    bool ok = true;
    for (size_t offset = 0; ok && offset < length; offset += sizeof(chunk))
    {
        size_t count = length - offset < sizeof(chunk) ? length - offset : sizeof(chunk);
        memcpy(chunk, start + offset, count);
        ok = fwrite(chunk, 1, count, file) == count;
    }
    ok = fflush(file) == 0 && ok;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (!ok)
    {
        remove(path);
    }
    return ok;
}
bool solco_test_load_mp3(void) { return command(0, 5); }
#endif

#if CONFIG_WATCH_DEV_CONSOLE
void solco_test_mp3_verify(void)
{
    extern const unsigned char start[] asm("_binary_import_test_mp3_start");
    extern const unsigned char end[] asm("_binary_import_test_mp3_end");
    FILE *file = fopen("/sdcard/music/Solco-import-test-48k.mp3", "rb");
    unsigned char data[256];
    size_t offset = 0, got;
    bool match = file != NULL;
    while (file && (got = fread(data, 1, sizeof(data), file)) > 0)
    {
        if (offset + got > (size_t)(end - start) || memcmp(data, start + offset, got))
        {
            match = false;
        }
        offset += got;
    }
    match = match && offset == (size_t)(end - start) && file && !ferror(file);
    if (file)
    {
        fclose(file);
    }
    printf("@MP3CHECK bytes=%lu expected=%lu match=%d\n", (unsigned long)offset,
           (unsigned long)(end - start), match);
}
#endif

#if CONFIG_WATCH_DEV_CONSOLE
bool solco_test_mp3_reset(void)
{
    /* Explicit repair of the original fixture created by this test session. */
    if (remove("/sdcard/music/Solco-import-test-48k.mp3") != 0)
    {
        return false;
    }
    return solco_test_mp3_fixture();
}
#endif

#if CONFIG_WATCH_DEV_CONSOLE
bool solco_test_load_stress(void) { return command(0, 6); }
#endif
