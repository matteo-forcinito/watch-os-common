/* Storage and synthesis live on core 0. The audio task never touches files.
 *
 * Ownership (no lock is ever taken by the audio task):
 *   audio task   deck transport (vinyl, play, loop edges in use), mixer, studio;
 *                receives commands through a lock-free queue (cmdq.h) and
 *                publishes a double-buffered snapshot with a sequence counter.
 *   storage task files, page cache, track sources, loop edge staging; publishes
 *                each page by tag (invalidate, copy, publish) and each source by
 *                generation (release/acquire).
 *   s_lock       storage/UI-only data: library, titles, error text. */
#include "engine.h"
#include "cmdq.h"
#if SOLCO_LINK
#include "link.h"
void solco_stream_delay(int16_t *stereo, int frames);
#endif
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
#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Largest engine block; the active one comes from the latency profile. */
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
/* Latency profiles: engine block and DMA queue (audio_start_ex). The block divides
 * the descriptor, so every write returns at a regular period. Live hits are
 * scheduled one block after their arrival: constant latency, no quantization. */
typedef struct
{
    const char *name;
    int block, desc_frames, desc_num;
} profile_t;
static const profile_t s_profiles[SOLCO_PROFILES] = {
    {"safe", 256, 256, 4}, /* 23.2 ms queue: practice, long recordings, SPI SD */
    {"live", 128, 128, 4}, /* 11.6 ms queue: hub, instruments */
};
#ifndef SOLCO_DEFAULT_PROFILE
/* The stereo line output (Knob) is the hub: live. Speaker boards: safe. */
#define SOLCO_DEFAULT_PROFILE (SOLCO_OUTPUT_CHANNELS == 2 ? 1 : 0)
#endif

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define RELAXED(p) __atomic_load_n((p), __ATOMIC_RELAXED)

extern const int16_t s_pulse_pcm[] asm("_binary_pulse_pcm_start");
extern const int16_t s_chord_pcm[] asm("_binary_chord_pcm_start");

typedef struct
{
    /* ---- storage-owned, published to audio ---- */
    int16_t *pcm;
    uint16_t peaks[PAGES];
    uint8_t bands[PAGES][3];            /* low / mid / high peaks of each page, 0..255 */
    uint32_t tags[PAGES];               /* UINT32_MAX while a page is being written */
    int16_t *loop_staging_pcm;          /* pointer swapped by audio, see loop_stage */
    uint32_t loop_staging_tags[SOLCO_LOOP_EDGE_PAGES];
    uint32_t staged_id;                 /* storage: stage the staging tags belong to */
    uint32_t loop_ready;                /* storage -> audio: staging complete for this id */
    uint32_t generation;                /* source generation, release after the fields below */
    uint32_t ready_generation;          /* == generation once playable */
    uint32_t length;                    /* frames; grows 0 -> N once within a generation */
    bool demo;
    int track;
    /* storage-only */
    FILE *file;
    solco_wav_t wav;
    bool mp3, preparing;
    char title[64];                     /* s_lock */
    /* ---- audio-owned ---- */
    solco_vinyl_t vinyl;
    bool playing;
    uint32_t audio_generation;
    bool a_demo, a_ready;
    int a_track;
    uint32_t a_length;
    int16_t *loop_pcm;
    bool loop_shift_now;                /* audio: apply a staged shift at once, not at the edge */
    uint8_t cue_set;                    /* audio: hot cues 1-4 (bit mask) */
    uint32_t cue_frame[SOLCO_HOTCUES];
    uint32_t loop_tags[SOLCO_LOOP_EDGE_PAGES];
    bool loop_pending;
    uint32_t loop_ids;                  /* audio: last id issued */
    /* audio -> storage */
    uint32_t loop_stage;                /* id that needs staging, 0 = none */
    solco_vinyl_t loop_proposal;        /* valid while loop_stage is unchanged */
    uint32_t miss_frame, miss_tag;      /* diagnostics, racy reads */
} deck_t;

typedef struct
{
    int deck;
    int action;
    int track;
} command_t;

enum
{
    CMD_TOGGLE = 1,
    CMD_SCRATCH,
    CMD_JOG,
    CMD_LOOP,
    CMD_KEYLOCK,
    CMD_RATE,
    CMD_STUDIO,
    CMD_LOOP_RANGE,
    CMD_HOTCUE,
};

/* Audio -> everyone. Double buffer: the writer fills cell (seq + 1) & 1, then
 * increments seq; a reader retries if seq moved while it copied. */
typedef struct
{
    struct
    {
        bool playing, held, looping, loop_marked, keylock, ready, shift_pending;
        uint32_t generation, frame, loop_in, loop_out;
        int64_t speed, rate;
        uint8_t cue_set;
        uint32_t cue_frame[SOLCO_HOTCUES];
    } deck[2];
    uint16_t peak;
    uint32_t underruns, audio_errors, restarts, mix_us, cycle_us, late_cycles, blocks, clips;
    uint32_t mix_hist[8]; /* mix time in eighths of the block budget */
    int profile, block, queue_frames;
    studio_status_t studio;
    int64_t anchor_offset_us; /* frame(t) = (t - offset) * RATE / 1e6 */
    bool anchor_valid;
} snapshot_t;

static const char *TAG = "solco";
static deck_t s_decks[2];
static char s_tracks[TRACKS][256];
static int s_track_count;
static QueueHandle_t s_commands; /* UI -> storage (file operations) */
static solco_cmdq_t s_audio_commands;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock; /* never taken by the audio task */
static char s_error[64];
static int s_cross = 500, s_volume = 0;
static uint32_t s_audio_fault; /* 0 ok, 1 output unavailable, 2 restarting */
static uint32_t s_force_restart; /* development: exercise the recovery path */
static int s_profile_request = SOLCO_DEFAULT_PROFILE;
static snapshot_t s_snapshot[2];
static uint32_t s_snapshot_seq;
static uint32_t s_studio_input_drops, s_studio_init_us;

/* Audio-task private state. */
static int16_t s_a[FRAMES * 2], s_b[FRAMES * 2], s_out[FRAMES * 2];
static solco_mix_t s_mix;
static studio_t s_studio;
static int16_t s_bus[FRAMES * 2];
static snapshot_t s_live;
static float s_gain; /* user volume, smoothed (5 ms) */

static int s_motion_mode;
static unsigned s_motion_calibration;
static uint32_t s_motion_reads, s_motion_errors, s_motion_hits;
static int s_motion_learned, s_motion_pad;

void solco_engine_sync_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    configASSERT(s_lock);
    solco_cmdq_init(&s_audio_commands);
}

static void snapshot_read(snapshot_t *out)
{
    for (;;)
    {
        uint32_t seq = LOAD(&s_snapshot_seq);
        *out = s_snapshot[seq & 1];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (RELAXED(&s_snapshot_seq) == seq)
            return;
    }
}
static void snapshot_publish(void)
{
    uint32_t seq = RELAXED(&s_snapshot_seq) + 1;
    /* The previous sequence store must be visible before this buffer changes. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_snapshot[seq & 1] = s_live;
    STORE(&s_snapshot_seq, seq);
}

static bool audio_command(const solco_cmd_t *cmd)
{
    if (solco_cmdq_push(&s_audio_commands, cmd))
        return true;
    return false;
}

static void touched(void);

bool solco_studio_submit_at(studio_event_t event, int64_t time_us)
{
    if (event.type < STUDIO_HIT || event.type > STUDIO_KIT ||
        (event.type == STUDIO_HIT &&
         (event.value >= STUDIO_BANKS || !event.velocity || event.velocity > 127)))
    {
        return false;
    }
    bool live = !time_us && event.type == STUDIO_HIT;
    if (live)
        time_us = esp_timer_get_time();
    event.frame = 0;
    if (time_us > 0)
    {
        snapshot_t snap;
        snapshot_read(&snap);
        if (snap.anchor_valid)
        {
            int64_t frame = (time_us - snap.anchor_offset_us) * RATE / 1000000;
            if (live)
                frame += snap.block;
            event.frame = frame > 0 ? (uint64_t)frame : 0;
        }
    }
    if (event.type == STUDIO_HIT)
        touched();
    solco_cmd_t cmd = {.type = CMD_STUDIO, .event = event};
    if (audio_command(&cmd))
        return true;
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
    snapshot_t snap;
    snapshot_read(&snap);
    *status = snap.studio;
    status->dropped += __atomic_load_n(&s_studio_input_drops, __ATOMIC_RELAXED);
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
    snprintf(s_error, sizeof(s_error), "%s", text);
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

/* Storage: start a new source generation. The audio task resets the transport
 * (keeping rate and key lock) when it observes the new generation. */
static void publish_source(deck_t *d, bool demo, uint32_t length, bool ready)
{
    d->demo = demo;
    STORE(&d->length, length);
    uint32_t generation = RELAXED(&d->generation) + 1;
    if (!generation)
        generation = 1;
    STORE(&d->generation, generation);
    if (ready)
        STORE(&d->ready_generation, generation);
    solco_prepare_cancel((int)(d - s_decks), generation); /* jobs of older generations stop */
}

static void reset_deck(int index, bool next)
{
    deck_t *d = &s_decks[index];
    STORE(&d->ready_generation, 0); /* audio stops this deck at its next block */
    bool reuse = !next && d->mp3 && d->file && RELAXED(&d->length);
    d->preparing = false;
    for (int i = 0; i < PAGES; ++i)
        __atomic_store_n(&d->tags[i], UINT32_MAX, __ATOMIC_RELAXED);
    memset(d->peaks, 0, sizeof(d->peaks));
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_error[0] = '\0';
    xSemaphoreGiveRecursive(s_lock);
    if (reuse)
    {
        /* Same prepared cache: a new timeline, ready again once page 0 is back. */
        publish_source(d, false, RELAXED(&d->length), false);
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
            snprintf(d->title, sizeof(d->title), "%.63s", path + 14);
            xSemaphoreGiveRecursive(s_lock);
            d->preparing = true;
            publish_source(d, false, 0, false);
            if (!solco_prepare_request(index, RELAXED(&d->generation), path))
            {
                d->preparing = false;
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
    memcpy(d->title, title, sizeof(title));
    xSemaphoreGiveRecursive(s_lock);
    publish_source(d, demo, length, demo);
}

/* Page publication protocol: invalidate the tag, copy, publish the new tag. The
 * audio task re-reads the tag after its sample reads (seqlock per page). */
static void publish_page(uint32_t *tag, int16_t *dst, const int16_t *src, size_t frames,
                         uint32_t page)
{
    __atomic_store_n(tag, UINT32_MAX, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(dst, src, frames * 2 * sizeof(int16_t));
    STORE(tag, page);
}

/* Bidirectional page cache, filled outside the audio task. Each tag identifies
 * an absolute WAV page; modulo placement never aliases stale samples. */
static void fill(int index, const snapshot_t *snap)
{
    deck_t *d = &s_decks[index];
    uint32_t length = RELAXED(&d->length);
    if (d->demo || !d->file || !length)
    {
        return;
    }
    uint32_t generation = RELAXED(&d->generation);
    /* Until audio has adopted this generation its cursor belongs to the old source. */
    uint32_t position = snap->deck[index].generation == generation ? snap->deck[index].frame : 0;
    uint32_t current = position / PAGE;
    uint32_t total = (length + PAGE - 1) / PAGE;
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
    /* Loop edges requested by the audio task: staged, then swapped by audio. */
    uint32_t stage = LOAD(&d->loop_stage);
    solco_vinyl_t proposal = {0};
    if (stage && stage != RELAXED(&d->loop_ready))
    {
        proposal = d->loop_proposal;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (RELAXED(&d->loop_stage) != stage)
            stage = 0; /* superseded while copying; next pass */
        if (stage && d->staged_id != stage)
        {
            memset(d->loop_staging_tags, 0xff, sizeof(d->loop_staging_tags));
            d->staged_id = stage;
        }
    }
    else
    {
        stage = 0;
    }
    if (stage && !proposal.loop_marked)
    {
        STORE(&d->loop_ready, stage); /* nothing to stage */
        return;
    }
    if (stage)
    {
        uint32_t head = proposal.loop_in / PAGE;
        uint32_t tail = proposal.loop_out > proposal.loop_in ? (proposal.loop_out - 1) / PAGE : head;
        uint32_t edges[SOLCO_LOOP_EDGE_PAGES];
        for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES / 2; ++i)
        {
            edges[i] = head + i < total ? head + i : total - 1;
            edges[i + SOLCO_LOOP_EDGE_PAGES / 2] = tail >= (uint32_t)i ? tail - i : 0;
        }
        for (int i = 0; i < SOLCO_LOOP_EDGE_PAGES; ++i)
        {
            if (d->loop_staging_tags[i] != edges[i])
            {
                wanted = edges[i];
                edge_slot = i;
                break;
            }
        }
        if (wanted == UINT32_MAX)
        {
            /* Every new edge page is staged: hand it over to the audio task. */
            STORE(&d->loop_ready, stage);
            return;
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
        if (RELAXED(&d->tags[page % PAGES]) != (uint32_t)page)
        {
            wanted = (uint32_t)page;
            break;
        }
    }
    if (wanted == UINT32_MAX)
    {
        return;
    }
    int16_t pcm[PAGE * 2];
    uint32_t first = wanted * PAGE;
    size_t frames = length - first < PAGE ? length - first : PAGE;
    size_t unit = d->wav.channels * sizeof(int16_t);
    if (fseek(d->file, (long)(d->wav.offset + first * unit), SEEK_SET) != 0 ||
        fread(pcm, unit, frames, d->file) != frames)
    {
        error_text("Errore lettura microSD");
        fclose(d->file);
        d->file = NULL;
        STORE(&d->ready_generation, 0); /* audio stops and releases this deck */
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
    /* Display bands on the mono sum: one-pole splits at ~250 Hz and ~2.5 kHz. */
    float low = 0, wide = 0, peak_low = 0, peak_mid = 0, peak_high = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        float x = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;
        low += (x - low) * 0.035f;
        wide += (x - wide) * 0.30f;
        float l = fabsf(low), m = fabsf(wide - low), h = fabsf(x - wide);
        peak_low = l > peak_low ? l : peak_low;
        peak_mid = m > peak_mid ? m : peak_mid;
        peak_high = h > peak_high ? h : peak_high;
    }
    uint8_t bands[3] = {(uint8_t)fminf(peak_low / 128.f, 255.f), (uint8_t)fminf(peak_mid / 128.f, 255.f),
                        (uint8_t)fminf(peak_high / 128.f, 255.f)};
    if (edge_slot >= 0)
    {
        /* Staging belongs to storage until loop_ready == stage: plain copy. */
        memcpy(d->loop_staging_pcm + edge_slot * PAGE * 2, pcm, frames * 2 * sizeof(int16_t));
        d->loop_staging_tags[edge_slot] = wanted;
        return;
    }
    uint32_t slot = wanted % PAGES;
    d->peaks[slot] = peak;
    memcpy(d->bands[slot], bands, sizeof(bands));
    publish_page(&d->tags[slot], d->pcm + slot * PAGE * 2, pcm, frames, wanted);
    if (wanted == 0)
    {
        STORE(&d->ready_generation, generation);
    }
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

static bool deck_busy(const snapshot_t *snap, int deck)
{
    return snap->deck[deck].playing || snap->deck[deck].held;
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
        snapshot_t snap;
        snapshot_read(&snap);
        studio_status_t studio = snap.studio;
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
            if (RELAXED(&d->generation) != prepared.generation || !d->preparing)
            {
                continue;
            }
            if (prepared.error[0])
            {
                d->preparing = false;
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
            d->preparing = false;
            STORE(&d->length, valid ? wav.bytes / (wav.channels * 2) : 0);
            if (!valid)
            {
                error_text("Cache PCM non disponibile");
            }
        }
        command_t command;
        while (xQueueReceive(s_commands, &command, 0) == pdTRUE)
        {
            snapshot_read(&snap);
            if (command.action == 3 || command.action == 5 || command.action == 6)
            {
                bool busy = deck_busy(&snap, 0) || deck_busy(&snap, 1) ||
                            (command.action == 6 && (s_decks[0].preparing || s_decks[1].preparing));
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
            if ((command.action == 2 || command.action == 4) && deck_busy(&snap, command.deck))
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
                s_decks[command.deck].track = command.track;
            }
            reset_deck(command.deck, command.action == 2);
        }
        snapshot_read(&snap);
        fill(0, &snap);
        fill(1, &snap);
        if (esp_timer_get_time() - last_log > 10000000)
        {
            sys_post(log_status, NULL);
            last_log = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Audio task only; no filesystem, allocation or lock here. */
static bool sample(void *source, int64_t frame, int16_t stereo[2])
{
    deck_t *d = source;
    if (d->a_demo)
    {
        int64_t at = frame;
        if (at < 0 || at >= DEMO_FRAMES) at %= DEMO_FRAMES;
        if (at < 0)
        {
            at += DEMO_FRAMES;
        }
        const int16_t *loop = d->a_track == 0 ? s_pulse_pcm : s_chord_pcm;
        stereo[0] = stereo[1] = loop[at];
        return true;
    }
    if (frame < 0 || frame >= d->a_length)
    {
        d->miss_frame = (uint32_t)frame;
        d->miss_tag = UINT32_MAX;
        return false;
    }
    uint32_t page = (uint32_t)frame / PAGE;
    /* Most WSOLA reads hit the ordinary cache. Avoid scanning all boundary
     * pages for every sample; the reserved edges are a fallback after wraps. */
    uint32_t *tag = &d->tags[page % PAGES];
    if (LOAD(tag) == page)
    {
        size_t at = ((size_t)frame % RING) * 2;
        stereo[0] = d->pcm[at];
        stereo[1] = d->pcm[at + 1];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (RELAXED(tag) == page)
            return true; /* not replaced while we read it */
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
    d->miss_tag = RELAXED(tag);
    return false;
}

static void issue_loop(deck_t *d, const solco_vinyl_t *proposal, bool staged)
{
    uint32_t id = ++d->loop_ids ? d->loop_ids : ++d->loop_ids;
    if (staged)
    {
        /* Seqlock for storage's copy of the proposal: 0 while it is rewritten. */
        __atomic_store_n(&d->loop_stage, 0, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->loop_proposal = *proposal;
        d->loop_pending = true;
        STORE(&d->loop_stage, id);
    }
    else
    {
        d->vinyl = *proposal;
        d->loop_pending = false;
        STORE(&d->loop_stage, 0);
    }
}

/* Loop shift on the live transport: at the next loop edge while playing (no jump
 * forward, same phase backward), or at once with the playhead moved by the same
 * amount (short fade through the gate). */
static void apply_shift(deck_t *d, const solco_vinyl_t *proposal, bool now)
{
    if (d->playing && !now)
    {
        solco_vinyl_arm_shift(&d->vinyl, proposal->loop_in, proposal->loop_out);
        return;
    }
    d->vinyl.pending_shift = false;
    d->vinyl.loop_in = proposal->loop_in;
    d->vinyl.loop_out = proposal->loop_out;
    d->vinyl.cursor += (int64_t)proposal->shift_delta * SOLCO_SAMPLE_ONE;
    d->vinyl.target = d->vinyl.cursor;
    if (d->playing)
        d->vinyl.gate = 0;
}

/* Audio: adopt a new source generation, the ready state and staged loop edges. */
static void deck_sync(deck_t *d, int index)
{
    uint32_t generation = LOAD(&d->generation);
    if (generation != d->audio_generation)
    {
        d->audio_generation = generation;
        d->a_demo = d->demo;
        d->a_track = d->track;
        d->playing = false;
        int64_t rate = d->vinyl.play_speed;
        solco_keylock_t *lock = d->vinyl.keylock;
        bool keylock_enabled = d->vinyl.keylock_enabled;
        if (lock)
            memset(lock, 0, sizeof(*lock));
        memset(&d->vinyl, 0, sizeof(d->vinyl));
        d->vinyl.play_speed = rate;
        d->vinyl.keylock = lock;
        d->vinyl.keylock_enabled = keylock_enabled;
        memset(d->loop_tags, 0xff, sizeof(d->loop_tags));
        d->loop_pending = false;
        d->cue_set = 0;
        STORE(&d->loop_stage, 0);
        /* A cue/load starts a new timeline: never drain its first samples
         * using the previous source's pause fade. */
        if (index == 0)
            s_mix.a = 0;
        else
            s_mix.b = 0;
    }
    d->a_length = LOAD(&d->length);
    d->a_ready = LOAD(&d->ready_generation) == generation && generation;
    if (!d->a_ready)
    {
        d->playing = false;
        d->vinyl.held = false;
    }
    uint32_t stage = RELAXED(&d->loop_stage);
    if (d->loop_pending && stage && LOAD(&d->loop_ready) == stage)
    {
        /* Publish only once every new edge page is available. The old loop kept
         * playing during SD reads; swap buffers, no copy, no uncached jump. */
        int16_t *previous = d->loop_pcm;
        d->loop_pcm = d->loop_staging_pcm;
        d->loop_staging_pcm = previous;
        memcpy(d->loop_tags, d->loop_staging_tags, sizeof(d->loop_tags));
        if (d->loop_proposal.shift_delta)
        {
            apply_shift(d, &d->loop_proposal, d->loop_shift_now);
        }
        else
        {
            d->vinyl.pending_shift = false;
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
        }
        d->loop_pending = false;
        /* A new id: storage restarts staging from scratch for the next request,
         * into the buffer we just released. */
        uint32_t id = ++d->loop_ids ? d->loop_ids : ++d->loop_ids;
        (void)id;
        STORE(&d->loop_stage, 0);
    }
}

static void apply_command(const solco_cmd_t *cmd)
{
    if (cmd->type == CMD_STUDIO)
    {
        studio_push(&s_studio, cmd->event);
        return;
    }
    if (cmd->deck > 1)
        return;
    deck_t *d = &s_decks[cmd->deck];
    switch (cmd->type)
    {
    case CMD_TOGGLE:
        if (d->a_ready)
            d->playing = !d->playing;
        break;
    case CMD_SCRATCH:
        if (d->a_ready)
            solco_vinyl_touch(&d->vinyl, (int)cmd->wide, cmd->value, d->a_length, d->a_demo);
        break;
    case CMD_JOG:
        if (d->a_ready)
            solco_vinyl_jog(&d->vinyl, cmd->value, (uint32_t)cmd->wide, d->a_length, d->a_demo);
        break;
    case CMD_LOOP:
    {
        solco_vinyl_t proposal = d->vinyl;
        if (d->vinyl.pending_shift)
        {
            /* Shifts pressed before the edge accumulate on the armed window. */
            proposal.loop_in = d->vinyl.pending_in;
            proposal.loop_out = d->vinyl.pending_out;
        }
        if (d->loop_pending)
        {
            proposal.loop_in = d->loop_proposal.loop_in;
            proposal.loop_out = d->loop_proposal.loop_out;
            proposal.loop_enabled = d->loop_proposal.loop_enabled;
            proposal.loop_marked = d->loop_proposal.loop_marked;
        }
        if (!d->a_ready || d->vinyl.held || !solco_vinyl_loop(&proposal, cmd->value, d->a_length))
            break;
        d->loop_shift_now = cmd->wide != 0;
        if (proposal.shift_delta && d->a_demo)
            apply_shift(d, &proposal, d->loop_shift_now); /* demo loops need no edge pages */
        else if (proposal.shift_delta)
            issue_loop(d, &proposal, true);
        else
        {
            proposal.pending_shift = false;
            issue_loop(d, &proposal, !(d->a_demo || !proposal.loop_enabled || cmd->value == 0));
        }
        break;
    }
    case CMD_LOOP_RANGE:
    {
        /* Beat loop: an explicit window from the caller (IN at the playhead). */
        solco_vinyl_t proposal = d->vinyl;
        uint32_t in = (uint32_t)cmd->value, out = (uint32_t)cmd->wide;
        if (!d->a_ready || d->vinyl.held || out <= in + 256 || out > d->a_length)
            break;
        proposal.loop_in = in;
        proposal.loop_out = out;
        proposal.loop_marked = proposal.loop_enabled = true;
        proposal.pending_shift = false;
        proposal.shift_delta = 0;
        issue_loop(d, &proposal, !d->a_demo);
        break;
    }
    case CMD_HOTCUE:
    {
        int slot = cmd->value;
        if (slot < 0 || slot >= SOLCO_HOTCUES || !d->a_ready)
            break;
        if (cmd->wide == 0)
        {
            int64_t frame = d->vinyl.cursor / SOLCO_SAMPLE_ONE;
            d->cue_frame[slot] = frame < 0 ? 0 : (uint32_t)frame;
            d->cue_set |= (uint8_t)(1u << slot);
        }
        else if (cmd->wide == 2)
        {
            d->cue_set &= (uint8_t)~(1u << slot);
        }
        else if (d->cue_set & (1u << slot) && !d->vinyl.held)
        {
            uint32_t frame = d->cue_frame[slot];
            if (d->vinyl.loop_enabled && (frame < d->vinyl.loop_in || frame >= d->vinyl.loop_out))
                d->vinyl.loop_enabled = false; /* jumping out of the loop leaves it */
            d->vinyl.pending_shift = false;
            d->vinyl.cursor = d->vinyl.target = (int64_t)frame * SOLCO_SAMPLE_ONE;
            d->vinyl.gate = 0; /* ~3 ms fade-in through the gate, no click */
        }
        break;
    }
    case CMD_KEYLOCK:
        d->vinyl.keylock_enabled = cmd->value != 0;
        break;
    case CMD_RATE:
        d->vinyl.play_speed = cmd->wide;
        if (!d->playing && !d->vinyl.held)
        {
            d->vinyl.transport_rate = cmd->wide;
            d->vinyl.rate_initialized = true;
        }
        break;
    }
}

static bool take(deck_t *d, int16_t *out, int block)
{
    solco_vinyl_tick(&d->vinyl, (uint32_t)(esp_timer_get_time() / 1000));
    bool missing = false;
    if (!d->a_ready || (!d->playing && !d->vinyl.held && d->vinyl.gate < 0.0001f))
    {
        memset(out, 0, (size_t)block * 4);
        return false;
    }
    bool active = solco_vinyl_render(&d->vinyl, out, (size_t)block, d->playing, d->a_length, d->a_demo,
                                     sample, d, &missing);
    if (missing)
    {
        ++s_live.underruns;
    }
    if (!d->a_demo && !d->vinyl.held && !d->vinyl.loop_enabled &&
        ((d->vinyl.play_speed >= 0 && d->vinyl.cursor >= (int64_t)d->a_length * SOLCO_SAMPLE_ONE) ||
         (d->vinyl.play_speed < 0 && d->vinyl.cursor <= 0)))
    {
        d->playing = false;
    }
    return active;
}

static bool output_open(const profile_t *profile)
{
    if (audio_start_ex(RATE, SOLCO_OUTPUT_CHANNELS, profile->desc_frames, profile->desc_num) != ESP_OK)
        return false;
    /* A single user-facing digital fader; no hidden -22.5 dB codec attenuation. */
    if (audio_set_volume(100) != ESP_OK)
    {
        audio_stop();
        return false;
    }
    return true;
}

/* Never gives up: retries with backoff, the decks keep their state meanwhile. */
static void output_recover(bool first, const profile_t *profile)
{
    uint32_t backoff = 20;
    if (!first)
    {
        audio_stop();
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }
    while (!output_open(profile))
    {
        STORE(&s_audio_fault, 1);
        snapshot_publish();
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff < 1000 ? backoff * 2 : 1000;
    }
    STORE(&s_audio_fault, 0);
    /* Fade every gain in from silence: no click after an output restart. */
    s_mix.a = s_mix.b = s_mix.master = s_mix.studio = 0;
    s_gain = 0;
}

static void publish_decks(void)
{
    for (int i = 0; i < 2; ++i)
    {
        deck_t *d = &s_decks[i];
        int64_t position = d->vinyl.cursor / SOLCO_SAMPLE_ONE;
        s_live.deck[i].playing = d->playing;
        s_live.deck[i].held = d->vinyl.held;
        s_live.deck[i].looping = d->vinyl.loop_enabled;
        s_live.deck[i].loop_marked = d->vinyl.loop_marked;
        s_live.deck[i].keylock = d->vinyl.keylock_enabled;
        s_live.deck[i].ready = d->a_ready;
        s_live.deck[i].generation = d->audio_generation;
        s_live.deck[i].frame = position < 0 ? 0 : (uint32_t)position;
        s_live.deck[i].loop_in = d->vinyl.loop_in;
        s_live.deck[i].loop_out = d->vinyl.loop_out;
        s_live.deck[i].speed = d->vinyl.speed;
        s_live.deck[i].rate = d->vinyl.play_speed ? d->vinyl.play_speed : SOLCO_SAMPLE_ONE;
        s_live.deck[i].shift_pending = d->vinyl.pending_shift;
        s_live.deck[i].cue_set = d->cue_set;
        memcpy(s_live.deck[i].cue_frame, d->cue_frame, sizeof(d->cue_frame));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    power_cpu_boost(true);
    int profile = RELAXED(&s_profile_request);
    output_recover(true, &s_profiles[profile]);
    int64_t last_write = 0;
    for (;;)
    {
        const int block = s_profiles[profile].block;
        s_live.profile = profile;
        s_live.block = block;
        s_live.queue_frames = audio_queue_frames();
        const uint32_t budget_us = (uint32_t)block * 1000000 / RATE;
        const uint32_t queue_us = (uint32_t)((int64_t)s_live.queue_frames * 1000000 / RATE);
        int64_t start = esp_timer_get_time();
        deck_sync(&s_decks[0], 0);
        deck_sync(&s_decks[1], 1);
        solco_cmd_t cmd;
        while (solco_cmdq_pop(&s_audio_commands, &cmd))
        {
            apply_command(&cmd);
        }
        bool a = take(&s_decks[0], s_a, block);
        bool b = take(&s_decks[1], s_b, block);
        int cross = RELAXED(&s_cross), volume = RELAXED(&s_volume);
        studio_render(&s_studio, s_bus, (size_t)block);
        /* Mix at full level: the stream tap is pre-volume, the user volume is the
         * last gain of this device's own output. */
        solco_mix_studio(&s_mix, s_a, s_b, s_bus, s_out, (size_t)block, cross, 100, a, b,
                         s_studio.status.enabled || s_studio.status.playing ||
                             s_studio.status.voices);
#if SOLCO_LINK
        uint32_t frame0 = (uint32_t)s_studio.status.frame - (uint32_t)block;
        solco_stream_tap(s_out, block, frame0);
        solco_stream_render(s_out, block, frame0, s_live.anchor_offset_us);
        solco_stream_delay(s_out, block);
#endif
        float target_gain = volume * 0.01f;
        uint16_t peak = 0;
        for (int i = 0; i < block; ++i)
        {
            s_gain += (target_gain - s_gain) * 0.004535f;
            for (int ch = 0; ch < 2; ++ch)
            {
                int16_t v = (int16_t)(s_out[i * 2 + ch] * s_gain);
                s_out[i * 2 + ch] = v;
                uint16_t magnitude = (uint16_t)(v < 0 ? -(int)v : v);
                peak = magnitude > peak ? magnitude : peak;
            }
        }
        s_mix.peak = peak;
#if SOLCO_OUTPUT_CHANNELS == 1
        s_mix.peak = solco_fold_mono(s_out, (size_t)block);
#endif
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
        esp_err_t err = audio_write(s_out, (size_t)block * SOLCO_OUTPUT_CHANNELS);
        int64_t written = esp_timer_get_time();
        uint32_t cycle = last_write ? (uint32_t)(written - last_write) : 0;
        last_write = written;
        uint16_t released_peak = (uint16_t)((uint32_t)s_live.peak * 985 / 1000);
        s_live.peak = s_mix.peak > released_peak ? s_mix.peak : released_peak;
        s_live.clips = s_mix.clipped;
        if (elapsed > s_live.mix_us)
        {
            s_live.mix_us = elapsed;
        }
        uint32_t bucket = elapsed * 8 / budget_us;
        ++s_live.mix_hist[bucket > 7 ? 7 : bucket];
        /* Diagnostic bound, not an analog underrun detector. Exclude initial DMA priming. */
        if (s_live.blocks > 100)
        {
            if (cycle > s_live.cycle_us)
            {
                s_live.cycle_us = cycle;
            }
            if (queue_us && cycle > queue_us)
            {
                ++s_live.late_cycles;
            }
        }
        /* Frame/time line for event scheduling: the lower envelope of the write
         * returns, so an event mapped through it is never behind the block being
         * rendered. Relaxed by 1 us per block to follow slow drift. */
        int64_t offset = written - (int64_t)(s_studio.status.frame * 1000000 / RATE);
        if (err != ESP_OK)
            s_live.anchor_valid = false; /* the line restarts after recovery */
        else if (!s_live.anchor_valid || offset < s_live.anchor_offset_us + 1)
        {
            s_live.anchor_offset_us = offset;
            s_live.anchor_valid = true;
        }
        else
            s_live.anchor_offset_us += 1;
        s_live.studio = s_studio.status;
        ++s_live.blocks;
        if (err != ESP_OK)
        {
            ++s_live.audio_errors;
        }
        publish_decks();
        snapshot_publish();
        int requested = RELAXED(&s_profile_request);
        if (requested != profile)
        {
            /* New latency profile: reopen with its queue, gains fade in again. */
            profile = requested;
            s_live.anchor_valid = false;
            s_live.mix_us = s_live.cycle_us = 0;
            memset(s_live.mix_hist, 0, sizeof(s_live.mix_hist));
            output_recover(false, &s_profiles[profile]);
            last_write = 0;
            s_live.blocks = 0;
        }
        else if (err != ESP_OK || __atomic_exchange_n(&s_force_restart, 0, __ATOMIC_ACQUIRE))
        {
            ++s_live.restarts;
            s_live.anchor_valid = false;
            STORE(&s_audio_fault, 2);
            output_recover(false, &s_profiles[profile]);
            last_write = 0;
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
        memset(s_decks[i].tags, 0xff, sizeof(s_decks[i].tags));
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
    s_live.studio = s_studio.status;
    snapshot_publish();
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
    touched();
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

/* Last user action on decks, mixer or Studio: "follow the mixer" routing. */
static uint32_t s_touch_ms;
static void touched(void) { __atomic_store_n(&s_touch_ms, (uint32_t)(esp_timer_get_time() / 1000) | 1, __ATOMIC_RELAXED); }
uint32_t solco_engine_idle_ms(void)
{
    uint32_t at = __atomic_load_n(&s_touch_ms, __ATOMIC_RELAXED);
    return at ? (uint32_t)(esp_timer_get_time() / 1000) - at : UINT32_MAX;
}

static bool deck_command(int type, int deck, int32_t value, int64_t wide)
{
    if (deck < 0 || deck > 1)
        return false;
    touched();
    solco_cmd_t cmd = {.type = (uint8_t)type, .deck = (uint8_t)deck, .value = value, .wide = wide};
    if (audio_command(&cmd))
        return true;
    error_text("Comandi occupati: riprova");
    return false;
}

bool solco_toggle(int deck) { return deck_command(CMD_TOGGLE, deck, 0, 0); }
bool solco_cue(int deck) { return command(deck, 1); }
bool solco_next(int deck) { return command(deck, 2); }

void solco_scratch(int deck, int phase, int32_t frames)
{
    if (phase < 0 || phase > 2)
    {
        return;
    }
    deck_command(CMD_SCRATCH, deck, frames, phase);
}

void solco_jog(int deck, int steps)
{
    deck_command(CMD_JOG, deck, steps, (int64_t)(uint32_t)(esp_timer_get_time() / 1000));
}

/* Validated here against the published state for the UI's answer; the audio task
 * applies it to the live transport (same rules, a few ms later). */
bool solco_loop(int deck, int action)
{
    if (deck < 0 || deck > 1)
        return false;
    snapshot_t snap;
    snapshot_read(&snap);
    solco_vinyl_t probe = {
        .cursor = (int64_t)snap.deck[deck].frame * SOLCO_SAMPLE_ONE,
        .loop_in = snap.deck[deck].loop_in,
        .loop_out = snap.deck[deck].loop_out,
        .loop_enabled = snap.deck[deck].looping,
        .loop_marked = snap.deck[deck].loop_marked,
    };
    uint32_t length = RELAXED(&s_decks[deck].length);
    if (!snap.deck[deck].ready || snap.deck[deck].held || !solco_vinyl_loop(&probe, action, length))
        return false;
    return deck_command(CMD_LOOP, deck, action, 0);
}

bool solco_keylock(int deck, bool enabled)
{
    return deck_command(CMD_KEYLOCK, deck, enabled, 0);
}

bool solco_rate_fine(int deck, int64_t rate_q32)
{
    int64_t magnitude = rate_q32 < 0 ? -rate_q32 : rate_q32;
    if (magnitude < SOLCO_SAMPLE_ONE / 16 || magnitude > 4 * SOLCO_SAMPLE_ONE)
        return false;
    return deck_command(CMD_RATE, deck, 0, rate_q32);
}

bool solco_rate(int deck, int32_t rate) { return solco_rate_fine(deck, SOLCO_RATE_FROM_Q16(rate)); }

void solco_cross(int value)
{
    touched();
    __atomic_store_n(&s_cross, value < 0 ? 0 : value > 1000 ? 1000 : value, __ATOMIC_RELAXED);
}

void solco_volume(int value)
{
    __atomic_store_n(&s_volume, value < 0 ? 0 : value > 100 ? 100 : value, __ATOMIC_RELAXED);
}

void solco_test_audio_restart(void) { STORE(&s_force_restart, 1); }

static int deck_bpm_q16(int deck)
{
    /* Demos are 120 BPM; analysed metadata (F4) will fill this for the library. */
    return s_decks[deck].demo ? 120 << 16 : 0;
}

bool solco_track_info(int deck, solco_track_info_t *info)
{
    if (deck < 0 || deck > 1)
        return false;
    deck_t *d = &s_decks[deck];
    memset(info, 0, sizeof(*info));
    info->track = d->track;
    info->length_frames = RELAXED(&d->length);
    info->bpm_q16 = deck_bpm_q16(deck);
    info->grid_offset_frames = 0;
    info->key = -1;
    return true;
}

bool solco_loop_shift(int deck, int direction, bool half, bool now)
{
    int action = direction > 0 ? (half ? SOLCO_LOOP_SHIFT_HALF_FWD : SOLCO_LOOP_SHIFT_FWD)
                               : (half ? SOLCO_LOOP_SHIFT_HALF_BACK : SOLCO_LOOP_SHIFT_BACK);
    snapshot_t snap;
    snapshot_read(&snap);
    if (deck < 0 || deck > 1 || !snap.deck[deck].looping)
        return false;
    return deck_command(CMD_LOOP, deck, action, now);
}

bool solco_loop_beats(int deck, int beats)
{
    if (deck < 0 || deck > 1 || beats < 1 || beats > 32)
        return false;
    int bpm = deck_bpm_q16(deck);
    if (!bpm)
        return false; /* no tempo known yet: manual IN/OUT */
    snapshot_t snap;
    snapshot_read(&snap);
    uint64_t span = (uint64_t)beats * 60 * RATE * 65536 / (uint64_t)bpm;
    uint32_t in = snap.deck[deck].frame;
    uint32_t length = RELAXED(&s_decks[deck].length);
    if (!snap.deck[deck].ready || in + span > length)
        return false;
    return deck_command(CMD_LOOP_RANGE, deck, (int32_t)in, (int64_t)(in + span));
}

bool solco_hotcue(int deck, int slot, int action)
{
    if (slot < 0 || slot >= SOLCO_HOTCUES || action < 0 || action > 2)
        return false;
    return deck_command(CMD_HOTCUE, deck, slot, action);
}

bool solco_engine_timeline(int64_t *anchor_offset_us, int *block, int *queue_frames)
{
    snapshot_t snap;
    snapshot_read(&snap);
    *anchor_offset_us = snap.anchor_offset_us;
    *block = snap.block;
    *queue_frames = snap.queue_frames;
    return snap.anchor_valid;
}

bool solco_audio_profile(int profile)
{
    if (profile < 0 || profile >= SOLCO_PROFILES)
        return false;
    STORE(&s_profile_request, profile);
    return true;
}

const char *solco_audio_profile_name(int profile)
{
    return profile >= 0 && profile < SOLCO_PROFILES ? s_profiles[profile].name : "?";
}

void solco_mix_histogram(uint32_t histogram[8], uint32_t *restarts, uint32_t *dropped)
{
    snapshot_t snap;
    snapshot_read(&snap);
    memcpy(histogram, snap.mix_hist, sizeof(snap.mix_hist));
    *restarts = snap.restarts;
    *dropped = atomic_load_explicit(&s_audio_commands.dropped, memory_order_relaxed);
}

void solco_status(solco_status_t *status)
{
    snapshot_t snap;
    snapshot_read(&snap);
    memset(status, 0, sizeof(*status));
    status->cross = RELAXED(&s_cross);
    status->volume = RELAXED(&s_volume);
    status->peak = snap.peak;
    status->underruns = snap.underruns;
    status->audio_errors = snap.audio_errors;
    status->mix_us = snap.mix_us;
    status->cycle_us = snap.cycle_us;
    status->late_cycles = snap.late_cycles;
    status->blocks = snap.blocks;
    status->clips = snap.clips;
    status->profile = snap.profile;
    status->block = snap.block;
    status->queue_frames = snap.queue_frames;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    memcpy(status->error, s_error, sizeof(status->error));
    for (int i = 0; i < 2; ++i)
        memcpy(status->title[i], s_decks[i].title, sizeof(status->title[i]));
    xSemaphoreGiveRecursive(s_lock);
    uint32_t fault = RELAXED(&s_audio_fault);
    if (!status->error[0] && fault)
    {
        snprintf(status->error, sizeof(status->error), "%s",
                 fault == 1 ? "Uscita audio non disponibile: nuovo tentativo"
                            : "Uscita audio riavviata");
    }
    for (int i = 0; i < 2; ++i)
    {
        deck_t *d = &s_decks[i];
        status->playing[i] = snap.deck[i].playing;
        status->frames[i] = snap.deck[i].frame;
        int64_t phase = (int64_t)status->frames[i] % (RATE * 18);
        status->platter_phase[i] = (uint32_t)phase;
        status->looping[i] = snap.deck[i].looping;
        status->loop_marked[i] = snap.deck[i].loop_marked;
        status->loop_in[i] = snap.deck[i].loop_in;
        status->loop_out[i] = snap.deck[i].loop_out;
        status->scratching[i] = snap.deck[i].held;
        status->ready[i] = snap.deck[i].ready;
        status->preparing[i] = d->preparing;
        status->prepare_percent[i] = solco_prepare_percent(i);
        status->speed[i] = SOLCO_RATE_TO_Q16(snap.deck[i].speed);
        status->keylock[i] = snap.deck[i].keylock;
        status->rate[i] = SOLCO_RATE_TO_Q16(snap.deck[i].rate);
        status->rate_ppm[i] =
            (int32_t)((snap.deck[i].rate - (snap.deck[i].rate < 0 ? -SOLCO_SAMPLE_ONE : SOLCO_SAMPLE_ONE)) *
                      1000000 / SOLCO_SAMPLE_ONE);
        status->miss_frame[i] = d->miss_frame;
        status->miss_tag[i] = d->miss_tag;
        status->track[i] = d->track;
        status->length[i] = RELAXED(&d->length);
        status->shift_pending[i] = snap.deck[i].shift_pending;
        status->hotcue_set[i] = snap.deck[i].cue_set;
        memcpy(status->hotcue_frame[i], snap.deck[i].cue_frame, sizeof(status->hotcue_frame[i]));
        bool demo = d->demo;
        /* Display only: racy reads of the cache directory are harmless here. */
        for (uint32_t j = 0; j < 64; ++j)
        {
            uint32_t page = (status->frames[i] + j * PAGE / 2) / PAGE;
            uint32_t slot = page % PAGES;
            if (demo)
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
                bool cached = RELAXED(&d->tags[slot]) == page;
                status->wave[i][j] = cached ? d->peaks[slot] : 0;
                for (int k = 0; k < 3; ++k)
                    status->wave_bands[i][j][k] = cached ? d->bands[slot][k] : 0;
            }
        }
        for (int j = 0; j < PAGES; ++j)
        {
            if (RELAXED(&d->tags[j]) != UINT32_MAX)
            {
                status->buffered[i] += PAGE;
            }
        }
        if (demo)
        {
            status->buffered[i] = DEMO_FRAMES;
        }
    }
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
