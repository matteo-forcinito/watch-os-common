#include "studio_pro.h"

#include "hardware/microphone.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "studio_pro";

#define RATE           44100
#define MAX_BLOCK      1024
#define SAMPLE_VOICES  12
#define SYNTH_VOICES   6
#define EVENTS         64
#define ECHO_FRAMES    (RATE * 3 / 2)
#define REC_FRAMES     (PRO_REC_RATE * PRO_REC_SECONDS)
#define DIR_SAMPLES    "/sdcard/solco/samples"
#define DIR_STUDIO     "/sdcard/solco/studio"
#define PROJECT_FILE   DIR_STUDIO "/pro.bin"
#define PROJECT_MAGIC  0x50524F31u   /* "PRO1" */

/* ------------------------------------------------------------------ state */

typedef struct
{
    int16_t *pcm;
    uint32_t frames;
    uint32_t rate;
    int pitch;
    int gain;
    char name[24];
} slot_t;

typedef struct
{
    bool on;
    int slot;
    double pos;
    double step;
    float gain;
} sample_voice_t;

typedef enum
{
    ENV_OFF,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
} env_stage_t;

typedef struct
{
    int note;
    float freq;
    float phase1, phase2;
    env_stage_t stage;
    float env;
    float low, band;
    float velocity;
    int32_t gate_left;      /* sequencer notes release by themselves; -1 = held */
    uint32_t age;
} synth_voice_t;

typedef enum
{
    EV_HIT,
    EV_NOTE_ON,
    EV_NOTE_OFF,
    EV_SEQ_NOTE,
    EV_STOP_SLOT,
} event_type_t;

typedef struct
{
    uint8_t type;
    uint8_t a;
    uint8_t b;
    int32_t c;
} event_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    pro_synth_t synth;
    uint16_t tracks[PRO_SLOTS];
    int8_t melody[PRO_STEPS];
    int8_t root;
    int8_t swing;
    int8_t pitch[PRO_SLOTS];
    uint8_t gain[PRO_SLOTS];
} project_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static slot_t s_slot[PRO_SLOTS];
static sample_voice_t s_svoice[SAMPLE_VOICES];
static synth_voice_t s_voice[SYNTH_VOICES];
static event_t s_events[EVENTS];
static volatile int s_ev_head, s_ev_tail;
static pro_synth_t s_synth;
static uint16_t s_tracks[PRO_SLOTS];
static int8_t s_melody[PRO_STEPS];
static int s_root = 48;
static int s_swing;
static volatile int s_step = -1;
static volatile uint32_t s_epoch;   /* render blocks done: deferred frees wait on it */
static uint32_t s_voice_age;
static uint32_t s_noise = 0x12345678u;
static float *s_mix;
static float *s_echo;
static uint32_t s_echo_pos;
static bool s_ready;
static bool s_booting;   /* init: no audio task yet, buffers are swapped directly */

/* sequencer timing */
static int s_last_step = -1;
static int32_t s_swing_wait = -1;   /* frames until a swung step fires */
static int s_swing_step = -1;

/* capture */
static int16_t *s_rec_buf;
static volatile pro_capture_state_t s_cap_state = PRO_CAPTURE_IDLE;
static volatile bool s_cap_stop;
static volatile int s_cap_slot;
static volatile int s_cap_elapsed_ms;
static volatile int s_cap_level;
static int16_t *volatile s_graveyard;   /* old buffer replaced by a capture, freed by the UI task */

/* --------------------------------------------------------------- helpers */

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline float rnd(void)
{
    s_noise ^= s_noise << 13;
    s_noise ^= s_noise >> 17;
    s_noise ^= s_noise << 5;
    return (float)(int32_t)s_noise / 2147483648.0f;
}

static float note_freq(int note)
{
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static void push_event(uint8_t type, uint8_t a, uint8_t b, int32_t c)
{
    portENTER_CRITICAL(&s_mux);
    int next = (s_ev_head + 1) % EVENTS;

    if (next != s_ev_tail)
    {
        s_events[s_ev_head] = (event_t){.type = type, .a = a, .b = b, .c = c};
        s_ev_head = next;
    }

    portEXIT_CRITICAL(&s_mux);
}

/* Free a buffer the audio task may still read: wait until it finished two blocks. */
static void retire(void *pcm)
{
    if (pcm == NULL)
    {
        return;
    }

    uint32_t start = s_epoch;

    for (int i = 0; i < 40 && s_epoch - start < 2; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_epoch - start >= 2)
    {
        heap_caps_free(pcm);
    }
    /* else: the audio task is paused (capture): leak once rather than risk a crash */
}

static void stop_slot_voices(int slot)
{
    push_event(EV_STOP_SLOT, (uint8_t)slot, 0, 0);
    uint32_t start = s_epoch;

    for (int i = 0; i < 20 && s_epoch - start < 2; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* Swap a slot's buffer (UI task); the old one is freed safely. */
static void slot_replace(int slot, int16_t *pcm, uint32_t frames, uint32_t rate)
{
    if (s_booting)
    {
        heap_caps_free(s_slot[slot].pcm);
        s_slot[slot].pcm = pcm;
        s_slot[slot].frames = frames;
        s_slot[slot].rate = rate;
        return;
    }

    stop_slot_voices(slot);
    portENTER_CRITICAL(&s_mux);
    int16_t *old = s_slot[slot].pcm;
    s_slot[slot].pcm = pcm;
    s_slot[slot].frames = frames;
    s_slot[slot].rate = rate;
    portEXIT_CRITICAL(&s_mux);
    retire(old);
}

/* ---------------------------------------------------------------- presets */

static const struct
{
    const char *name;
    pro_synth_t p;
} s_presets[PRO_PRESETS] = {
    {"BASSO", {PRO_WAVE_SAW, 22, 45, 55, 0, 30, 40, 12, 10, 25, 0, 85}},
    {"LEAD", {PRO_WAVE_SQUARE, 55, 35, 30, 2, 25, 70, 20, 28, 15, 25, 70}},
    {"PAD", {PRO_WAVE_SAW, 40, 20, 20, 55, 50, 75, 60, 45, 0, 40, 60}},
    {"PLUCK", {PRO_WAVE_TRIANGLE, 50, 40, 70, 0, 18, 0, 18, 6, 10, 30, 80}},
    {"ACID", {PRO_WAVE_SAW, 18, 85, 80, 0, 22, 20, 10, 0, 45, 15, 70}},
    {"CAMPANA", {PRO_WAVE_SINE, 80, 10, 10, 0, 45, 15, 55, 60, 0, 35, 75}},
    {"FIATI", {PRO_WAVE_SAW, 35, 25, 45, 15, 35, 70, 25, 20, 20, 10, 70}},
    {"RUMORE", {PRO_WAVE_NOISE, 45, 60, 60, 0, 12, 0, 15, 0, 30, 30, 65}},
};

const char *studio_pro_preset_name(int preset)
{
    return preset >= 0 && preset < PRO_PRESETS ? s_presets[preset].name : "";
}

void studio_pro_preset(int preset)
{
    if (preset >= 0 && preset < PRO_PRESETS)
    {
        studio_pro_synth_set(&s_presets[preset].p);
    }
}

void studio_pro_synth_get(pro_synth_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_synth;
    portEXIT_CRITICAL(&s_mux);
}

void studio_pro_synth_set(const pro_synth_t *params)
{
    portENTER_CRITICAL(&s_mux);
    s_synth = *params;
    portEXIT_CRITICAL(&s_mux);
}

void studio_pro_note_on(int note, int velocity)
{
    if (note >= 0 && note < 128)
    {
        push_event(EV_NOTE_ON, (uint8_t)note, (uint8_t)(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity), -1);
    }
}

void studio_pro_note_off(int note)
{
    if (note >= 0 && note < 128)
    {
        push_event(EV_NOTE_OFF, (uint8_t)note, 0, 0);
    }
}

void studio_pro_hit(int slot, int velocity)
{
    if (slot >= 0 && slot < PRO_SLOTS)
    {
        push_event(EV_HIT, (uint8_t)slot, (uint8_t)(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity), 0);
    }
}

/* -------------------------------------------------------------- sequencer */

uint16_t studio_pro_track(int slot)
{
    return slot >= 0 && slot < PRO_SLOTS ? s_tracks[slot] : 0;
}

void studio_pro_toggle(int slot, int step)
{
    if (slot >= 0 && slot < PRO_SLOTS && step >= 0 && step < PRO_STEPS)
    {
        s_tracks[slot] ^= (uint16_t)(1u << step);   /* 16-bit store: atomic for the audio task */
    }
}

int studio_pro_melody(int step)
{
    return step >= 0 && step < PRO_STEPS ? s_melody[step] : -1;
}

void studio_pro_set_melody(int step, int row)
{
    if (step >= 0 && step < PRO_STEPS)
    {
        s_melody[step] = (int8_t)(row >= 0 && row < PRO_NOTE_ROWS ? row : -1);
    }
}

/* Minor pentatonic over two octaves: always sounds musical, whatever is tapped. */
int studio_pro_row_note(int row)
{
    static const int degrees[PRO_NOTE_ROWS] = {0, 3, 5, 7, 10, 12, 15, 17};
    return s_root + degrees[row < 0 ? 0 : row >= PRO_NOTE_ROWS ? PRO_NOTE_ROWS - 1 : row];
}

void studio_pro_set_root(int midi_note)
{
    s_root = midi_note < 24 ? 24 : midi_note > 72 ? 72 : midi_note;
}

int studio_pro_root(void)
{
    return s_root;
}

void studio_pro_set_swing(int percent)
{
    s_swing = percent < 0 ? 0 : percent > 60 ? 60 : percent;
}

int studio_pro_swing(void)
{
    return s_swing;
}

void studio_pro_clear_tracks(void)
{
    memset(s_tracks, 0, sizeof(s_tracks));
    memset(s_melody, -1, sizeof(s_melody));
}

int studio_pro_step(void)
{
    return s_step;
}

/* ----------------------------------------------------------------- slots */

void studio_pro_slot_info(int slot, pro_slot_info_t *out)
{
    memset(out, 0, sizeof(*out));

    if (slot < 0 || slot >= PRO_SLOTS)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    out->used = s_slot[slot].pcm != NULL && s_slot[slot].frames > 0;
    out->frames = s_slot[slot].frames;
    out->rate = s_slot[slot].rate;
    out->pitch = s_slot[slot].pitch;
    out->gain = s_slot[slot].gain;
    memcpy(out->name, s_slot[slot].name, sizeof(out->name));
    portEXIT_CRITICAL(&s_mux);
}

bool studio_pro_slot_peaks(int slot, uint16_t *peaks, int count)
{
    if (slot < 0 || slot >= PRO_SLOTS || count <= 0)
    {
        return false;
    }

    /* UI task: the buffer is only replaced by this task or by a capture (which defers the free). */
    const int16_t *pcm = s_slot[slot].pcm;
    uint32_t frames = s_slot[slot].frames;

    if (pcm == NULL || frames == 0)
    {
        memset(peaks, 0, (size_t)count * sizeof(uint16_t));
        return false;
    }

    for (int c = 0; c < count; c++)
    {
        uint32_t a = (uint32_t)((uint64_t)frames * c / count);
        uint32_t b = (uint32_t)((uint64_t)frames * (c + 1) / count);
        int peak = 0;

        for (uint32_t i = a; i < b; i += (b - a > 64 ? (b - a) / 64 : 1))
        {
            int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            peak = v > peak ? v : peak;
        }

        peaks[c] = (uint16_t)(peak > 32767 ? 32767 : peak);
    }

    return true;
}

void studio_pro_set_pitch(int slot, int semitones)
{
    if (slot >= 0 && slot < PRO_SLOTS)
    {
        s_slot[slot].pitch = semitones < -24 ? -24 : semitones > 24 ? 24 : semitones;
    }
}

void studio_pro_set_gain(int slot, int percent)
{
    if (slot >= 0 && slot < PRO_SLOTS)
    {
        s_slot[slot].gain = percent < 0 ? 0 : percent > 200 ? 200 : percent;
    }
}

static int16_t *copy_range(const int16_t *pcm, uint32_t from, uint32_t count)
{
    int16_t *out = heap_caps_malloc((size_t)count * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (out != NULL)
    {
        memcpy(out, pcm + from, (size_t)count * sizeof(int16_t));
    }

    return out;
}

static void one_pole(int16_t *pcm, uint32_t frames, uint32_t rate, float hz, bool high)
{
    float a = expf(-2.0f * 3.14159265f * hz / rate);
    float state = 0;

    for (uint32_t i = 0; i < frames; i++)
    {
        float x = pcm[i];
        state = (1.0f - a) * x + a * state;
        float y = high ? x - state : state;
        pcm[i] = (int16_t)clampf(y, -32768, 32767);
    }
}

esp_err_t studio_pro_edit(int slot, pro_edit_t edit, int a, int b)
{
    if (slot < 0 || slot >= PRO_SLOTS || s_cap_state == PRO_CAPTURE_RECORDING || s_cap_state == PRO_CAPTURE_WAITING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    slot_t *s = &s_slot[slot];

    if (edit == PRO_EDIT_CLEAR)
    {
        slot_replace(slot, NULL, 0, 0);
        s->name[0] = '\0';
        return ESP_OK;
    }

    if (s->pcm == NULL || s->frames < 64)
    {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t frames = s->frames;

    switch (edit)
    {
        case PRO_EDIT_TRIM:
        {
            a = a < 0 ? 0 : a > 1000 ? 1000 : a;
            b = b < 0 ? 0 : b > 1000 ? 1000 : b;
            uint32_t from = (uint32_t)((uint64_t)frames * a / 1000);
            uint32_t to = (uint32_t)((uint64_t)frames * b / 1000);

            if (to <= from + 64)
            {
                return ESP_ERR_INVALID_ARG;
            }

            int16_t *pcm = copy_range(s->pcm, from, to - from);

            if (pcm == NULL)
            {
                return ESP_ERR_NO_MEM;
            }

            slot_replace(slot, pcm, to - from, s->rate);
            return ESP_OK;
        }

        case PRO_EDIT_CHOP:
        {
            uint32_t part = frames / PRO_SLOTS;
            uint32_t rate = s->rate;

            if (part < 256)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            int16_t *src = copy_range(s->pcm, 0, frames);   /* the source slot is replaced too */

            if (src == NULL)
            {
                return ESP_ERR_NO_MEM;
            }

            for (int i = 0; i < PRO_SLOTS; i++)
            {
                int16_t *pcm = copy_range(src, part * i, part);

                if (pcm == NULL)
                {
                    break;
                }

                /* 5 ms fades: slices start and end without a click. */
                uint32_t fade = rate / 200;

                for (uint32_t k = 0; k < fade && k < part / 2; k++)
                {
                    pcm[k] = (int16_t)(pcm[k] * (float)k / fade);
                    pcm[part - 1 - k] = (int16_t)(pcm[part - 1 - k] * (float)k / fade);
                }

                slot_replace(i, pcm, part, rate);
                snprintf(s_slot[i].name, sizeof(s_slot[i].name), "FETTA %d", i + 1);
                s_slot[i].pitch = 0;
                s_slot[i].gain = 100;
            }

            heap_caps_free(src);
            return ESP_OK;
        }

        default:
            break;
    }

    /* In-place edits: silence the slot meanwhile. */
    stop_slot_voices(slot);
    int16_t *pcm = s->pcm;

    if (edit == PRO_EDIT_NORMALIZE)
    {
        int peak = 1;

        for (uint32_t i = 0; i < frames; i++)
        {
            int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            peak = v > peak ? v : peak;
        }

        float g = 31000.0f / peak;

        for (uint32_t i = 0; i < frames; i++)
        {
            pcm[i] = (int16_t)clampf(pcm[i] * g, -32768, 32767);
        }
    }
    else if (edit == PRO_EDIT_REVERSE)
    {
        for (uint32_t i = 0, j = frames - 1; i < j; i++, j--)
        {
            int16_t t = pcm[i];
            pcm[i] = pcm[j];
            pcm[j] = t;
        }
    }
    else if (edit == PRO_EDIT_FADE)
    {
        uint32_t in = s->rate / 100;       /* 10 ms */
        uint32_t out = frames / 4;         /* last quarter */

        for (uint32_t i = 0; i < in && i < frames; i++)
        {
            pcm[i] = (int16_t)(pcm[i] * (float)i / in);
        }

        for (uint32_t i = 0; i < out; i++)
        {
            float g = (float)i / out;
            pcm[frames - 1 - i] = (int16_t)(pcm[frames - 1 - i] * g * g);
        }
    }
    else if (edit == PRO_EDIT_LOWPASS)
    {
        one_pole(pcm, frames, s->rate, 1800.0f, false);
    }
    else if (edit == PRO_EDIT_HIGHPASS)
    {
        one_pole(pcm, frames, s->rate, 400.0f, true);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------- WAV */

static void write_le16(FILE *f, uint16_t v)
{
    fputc(v & 0xFF, f);
    fputc(v >> 8, f);
}

static void write_le32(FILE *f, uint32_t v)
{
    write_le16(f, (uint16_t)(v & 0xFFFF));
    write_le16(f, (uint16_t)(v >> 16));
}

static void ensure_dirs(void)
{
    mkdir("/sdcard/solco", 0775);
    mkdir(DIR_SAMPLES, 0775);
    mkdir(DIR_STUDIO, 0775);
}

esp_err_t studio_pro_save(int slot)
{
    if (slot < 0 || slot >= PRO_SLOTS || s_slot[slot].pcm == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    ensure_dirs();
    char path[64];
    snprintf(path, sizeof(path), DIR_SAMPLES "/slot%d.wav", slot + 1);
    FILE *f = fopen(path, "wb");

    if (f == NULL)
    {
        return ESP_FAIL;
    }

    uint32_t bytes = s_slot[slot].frames * 2;
    uint32_t rate = s_slot[slot].rate;
    fwrite("RIFF", 1, 4, f);
    write_le32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    write_le32(f, 16);
    write_le16(f, 1);
    write_le16(f, 1);
    write_le32(f, rate);
    write_le32(f, rate * 2);
    write_le16(f, 2);
    write_le16(f, 16);
    fwrite("data", 1, 4, f);
    write_le32(f, bytes);
    size_t written = fwrite(s_slot[slot].pcm, 1, bytes, f);
    int closed = fclose(f);
    ESP_LOGI(TAG, "Saved %s (%lu frames)", path, (unsigned long)s_slot[slot].frames);
    return written == bytes && closed == 0 ? ESP_OK : ESP_FAIL;
}

static uint32_t read_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t load_wav(int slot, const char *path, bool quiet)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t head[12];
    uint32_t rate = 0, data_bytes = 0;
    int channels = 0, bits = 0;
    esp_err_t err = ESP_FAIL;

    if (fread(head, 1, 12, f) == 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4))
    {
        uint8_t chunk[8];

        while (fread(chunk, 1, 8, f) == 8)
        {
            uint32_t size = read_le32(chunk + 4);

            if (!memcmp(chunk, "fmt ", 4))
            {
                uint8_t fmt[16];

                if (size < 16 || fread(fmt, 1, 16, f) != 16)
                {
                    break;
                }

                channels = fmt[2] | (fmt[3] << 8);
                rate = read_le32(fmt + 4);
                bits = fmt[14] | (fmt[15] << 8);
                fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
            }
            else if (!memcmp(chunk, "data", 4))
            {
                data_bytes = size;
                break;
            }
            else
            {
                fseek(f, (long)(size + (size & 1)), SEEK_CUR);
            }
        }
    }

    if (data_bytes && bits == 16 && (channels == 1 || channels == 2) && rate >= 8000 && rate <= 48000)
    {
        uint32_t frames = data_bytes / (2u * channels);
        uint32_t max = rate * 20;   /* 20 s per slot */
        frames = frames > max ? max : frames;
        int16_t *pcm = heap_caps_malloc((size_t)frames * 2, MALLOC_CAP_SPIRAM);

        if (pcm != NULL)
        {
            int16_t buf[512];
            uint32_t done = 0;

            while (done < frames)
            {
                uint32_t want = frames - done;
                want = want > 512 / channels ? 512 / channels : want;
                size_t got = fread(buf, 2 * channels, want, f);

                if (got == 0)
                {
                    break;
                }

                for (size_t i = 0; i < got; i++)
                {
                    pcm[done + i] = channels == 2 ? (int16_t)((buf[i * 2] + buf[i * 2 + 1]) / 2) : buf[i];
                }

                done += (uint32_t)got;
            }

            if (done > 64)
            {
                slot_replace(slot, pcm, done, rate);
                const char *base = strrchr(path, '/');
                snprintf(s_slot[slot].name, sizeof(s_slot[slot].name), "%.23s", base ? base + 1 : path);
                err = ESP_OK;
            }
            else
            {
                heap_caps_free(pcm);
            }
        }
        else
        {
            err = ESP_ERR_NO_MEM;
        }
    }

    fclose(f);

    if (err != ESP_OK && !quiet)
    {
        ESP_LOGW(TAG, "Cannot load %s", path);
    }

    return err;
}

esp_err_t studio_pro_load(int slot)
{
    if (slot < 0 || slot >= PRO_SLOTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char path[64];
    snprintf(path, sizeof(path), DIR_SAMPLES "/slot%d.wav", slot + 1);
    return load_wav(slot, path, false);
}

/* --------------------------------------------------------------- project */

void studio_pro_save_project(void)
{
    project_t p = {.magic = PROJECT_MAGIC, .version = 1};
    studio_pro_synth_get(&p.synth);
    memcpy(p.tracks, s_tracks, sizeof(p.tracks));
    memcpy(p.melody, s_melody, sizeof(p.melody));
    p.root = (int8_t)s_root;
    p.swing = (int8_t)s_swing;

    for (int i = 0; i < PRO_SLOTS; i++)
    {
        p.pitch[i] = (int8_t)s_slot[i].pitch;
        p.gain[i] = (uint8_t)(s_slot[i].gain > 200 ? 200 : s_slot[i].gain);
    }

    ensure_dirs();
    FILE *f = fopen(PROJECT_FILE, "wb");

    if (f != NULL)
    {
        fwrite(&p, sizeof(p), 1, f);
        fclose(f);
    }
}

static void load_project(void)
{
    project_t p;
    FILE *f = fopen(PROJECT_FILE, "rb");

    if (f == NULL)
    {
        return;
    }

    bool ok = fread(&p, sizeof(p), 1, f) == 1 && p.magic == PROJECT_MAGIC && p.version == 1;
    fclose(f);

    if (!ok)
    {
        return;
    }

    s_synth = p.synth;
    memcpy(s_tracks, p.tracks, sizeof(s_tracks));
    memcpy(s_melody, p.melody, sizeof(s_melody));
    studio_pro_set_root(p.root);
    studio_pro_set_swing(p.swing);

    for (int i = 0; i < PRO_SLOTS; i++)
    {
        studio_pro_set_pitch(i, p.pitch[i]);
        studio_pro_set_gain(i, p.gain[i]);
    }
}

/* --------------------------------------------------------------- capture */

esp_err_t studio_pro_record(int slot)
{
    if (!s_ready || s_rec_buf == NULL || slot < 0 || slot >= PRO_SLOTS || !microphone_supported())
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_cap_state == PRO_CAPTURE_WAITING || s_cap_state == PRO_CAPTURE_RECORDING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_cap_slot = slot;
    s_cap_stop = false;
    s_cap_elapsed_ms = 0;
    s_cap_level = 0;
    s_cap_state = PRO_CAPTURE_WAITING;   /* the audio task takes it at its next block */
    return ESP_OK;
}

void studio_pro_record_stop(void)
{
    s_cap_stop = true;
}

pro_capture_state_t studio_pro_capture_state(int *elapsed_ms, int *level)
{
    if (elapsed_ms != NULL)
    {
        *elapsed_ms = s_cap_elapsed_ms;
    }

    if (level != NULL)
    {
        *level = s_cap_level;
    }

    /* UI task: free what a capture replaced, now that the audio task runs again. */
    int16_t *dead = s_graveyard;

    if (dead != NULL && s_cap_state != PRO_CAPTURE_RECORDING && s_cap_state != PRO_CAPTURE_WAITING)
    {
        s_graveyard = NULL;
        retire(dead);
    }

    return s_cap_state;
}

bool studio_pro_capture_pending(void)
{
    return s_cap_state == PRO_CAPTURE_WAITING;
}

/* Audio task, with the output closed: the microphone has the codec. */
void studio_pro_capture_run(void)
{
    if (microphone_start(PRO_REC_RATE, 1) != ESP_OK)
    {
        ESP_LOGW(TAG, "Microphone busy");
        s_cap_state = PRO_CAPTURE_FAILED;
        return;
    }

    s_cap_state = PRO_CAPTURE_RECORDING;
    int64_t start = esp_timer_get_time();
    uint32_t total = 0;
    static int16_t block[512];   /* the audio task has a small stack */

    /* The first 60 ms of the ADC are the codec settling: dropped. */
    for (int i = 0; i < 2; i++)
    {
        size_t n = 0;
        microphone_read(block, 512, &n);
    }

    while (!s_cap_stop && total < REC_FRAMES)
    {
        size_t n = 0;

        if (microphone_read(block, 512, &n) != ESP_OK || n == 0)
        {
            vTaskDelay(1);
            continue;
        }

        n = total + n > REC_FRAMES ? REC_FRAMES - total : n;
        int peak = 0;

        for (size_t i = 0; i < n; i++)
        {
            int v = block[i] < 0 ? -block[i] : block[i];
            peak = v > peak ? v : peak;
        }

        memcpy(s_rec_buf + total, block, n * sizeof(int16_t));
        total += (uint32_t)n;
        s_cap_level = peak * 100 / 32768;
        s_cap_elapsed_ms = (int)((esp_timer_get_time() - start) / 1000);
    }

    microphone_stop();

    /* Leading silence off (below about -42 dBFS), 20 ms of lead-in kept. */
    uint32_t first = 0;

    while (first < total && abs(s_rec_buf[first]) < 260)
    {
        first++;
    }

    first = first > PRO_REC_RATE / 50 ? first - PRO_REC_RATE / 50 : 0;

    if (total <= first + PRO_REC_RATE / 20)
    {
        s_cap_state = PRO_CAPTURE_FAILED;   /* nothing but silence */
        return;
    }

    uint32_t frames = total - first;
    int16_t *pcm = heap_caps_malloc((size_t)frames * 2, MALLOC_CAP_SPIRAM);

    if (pcm == NULL)
    {
        s_cap_state = PRO_CAPTURE_FAILED;
        return;
    }

    memcpy(pcm, s_rec_buf + first, (size_t)frames * 2);

    /* Auto level: bring the take near full scale, at most +24 dB so silence stays silence. */
    int peak = 1;

    for (uint32_t i = 0; i < frames; i++)
    {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        peak = v > peak ? v : peak;
    }

    float gain = 30000.0f / peak;
    gain = gain > 16.0f ? 16.0f : gain;

    if (peak > 300 && gain > 1.05f)
    {
        for (uint32_t i = 0; i < frames; i++)
        {
            pcm[i] = (int16_t)clampf(pcm[i] * gain, -32768, 32767);
        }
    }

    int slot = s_cap_slot;

    /* No render runs meanwhile (this is the audio task): stop the slot's voices directly. */
    for (int v = 0; v < SAMPLE_VOICES; v++)
    {
        if (s_svoice[v].slot == slot)
        {
            s_svoice[v].on = false;
        }
    }

    portENTER_CRITICAL(&s_mux);
    int16_t *old = s_slot[slot].pcm;
    s_slot[slot].pcm = pcm;
    s_slot[slot].frames = frames;
    s_slot[slot].rate = PRO_REC_RATE;
    s_slot[slot].pitch = 0;
    s_slot[slot].gain = 100;
    snprintf(s_slot[slot].name, sizeof(s_slot[slot].name), "REC %d", slot + 1);
    portEXIT_CRITICAL(&s_mux);

    if (old != NULL)
    {
        if (s_graveyard != NULL)
        {
            heap_caps_free(old);   /* nobody reads it: this task is the only renderer */
        }
        else
        {
            s_graveyard = old;
        }
    }

    ESP_LOGI(TAG, "Recorded %lu ms into slot %d", (unsigned long)(frames * 1000 / PRO_REC_RATE), slot + 1);
    s_cap_state = PRO_CAPTURE_DONE;
}

/* ----------------------------------------------------------------- render */

static float env_rate(int value)
{
    float ms = 1.0f + (value / 100.0f) * (value / 100.0f) * 2000.0f;
    return 1.0f / (ms * RATE / 1000.0f);
}

static void start_note(int note, int velocity, int32_t gate)
{
    int pick = 0;
    uint32_t oldest = UINT32_MAX;

    for (int v = 0; v < SYNTH_VOICES; v++)
    {
        if (s_voice[v].stage == ENV_OFF)
        {
            pick = v;
            break;
        }

        if (s_voice[v].age < oldest)
        {
            oldest = s_voice[v].age;
            pick = v;
        }
    }

    synth_voice_t *sv = &s_voice[pick];
    sv->note = note;
    sv->freq = note_freq(note);
    sv->stage = ENV_ATTACK;
    sv->velocity = velocity / 127.0f;
    sv->gate_left = gate;
    sv->age = ++s_voice_age;

    if (sv->env <= 0.0001f)
    {
        sv->phase1 = 0;
        sv->phase2 = 0.37f;
        sv->low = sv->band = 0;
    }
}

static void release_note(int note)
{
    for (int v = 0; v < SYNTH_VOICES; v++)
    {
        if (s_voice[v].note == note && s_voice[v].stage != ENV_OFF && s_voice[v].stage != ENV_RELEASE &&
            s_voice[v].gate_left < 0)
        {
            s_voice[v].stage = ENV_RELEASE;
        }
    }
}

static void start_sample(int slot, int velocity)
{
    const slot_t *s = &s_slot[slot];

    if (s->pcm == NULL || s->frames == 0)
    {
        return;
    }

    int pick = 0;
    double furthest = -1;

    for (int v = 0; v < SAMPLE_VOICES; v++)
    {
        if (!s_svoice[v].on)
        {
            pick = v;
            furthest = -2;
            break;
        }

        if (s_svoice[v].pos > furthest)
        {
            furthest = s_svoice[v].pos;
            pick = v;
        }
    }

    sample_voice_t *sv = &s_svoice[pick];
    sv->slot = slot;
    sv->pos = 0;
    sv->step = (double)s->rate / RATE * pow(2.0, s->pitch / 12.0);
    sv->gain = (s->gain / 100.0f) * (velocity / 127.0f);
    sv->on = true;

}

static void drain_events(void)
{
    for (;;)
    {
        event_t e;
        portENTER_CRITICAL(&s_mux);
        bool any = s_ev_tail != s_ev_head;

        if (any)
        {
            e = s_events[s_ev_tail];
            s_ev_tail = (s_ev_tail + 1) % EVENTS;
        }

        portEXIT_CRITICAL(&s_mux);

        if (!any)
        {
            return;
        }

        switch (e.type)
        {
            case EV_HIT:
                start_sample(e.a, e.b);
                break;
            case EV_NOTE_ON:
            case EV_SEQ_NOTE:
                start_note(e.a, e.b, e.c);
                break;
            case EV_NOTE_OFF:
                release_note(e.a);
                break;
            case EV_STOP_SLOT:
                for (int v = 0; v < SAMPLE_VOICES; v++)
                {
                    if (s_svoice[v].slot == e.a)
                    {
                        s_svoice[v].on = false;
                    }
                }
                break;
            default:
                break;
        }
    }
}

static void fire_step(int step, int32_t step_frames)
{
    for (int slot = 0; slot < PRO_SLOTS; slot++)
    {
        if (s_tracks[slot] & (1u << step))
        {
            start_sample(slot, step % 4 == 0 ? 118 : 100);   /* downbeats a little louder */
        }
    }

    int row = s_melody[step];

    if (row >= 0)
    {
        start_note(studio_pro_row_note(row), 100, step_frames * 3 / 4);
    }
}

static void sequencer(bool playing, int step, int bpm, size_t frames)
{
    int32_t step_frames = (int32_t)((int64_t)RATE * 60 / (bpm > 0 ? bpm : 120) / 4);

    if (!playing)
    {
        s_last_step = -1;
        s_step = -1;
        s_swing_wait = -1;
        return;
    }

    if (s_swing_wait >= 0)
    {
        s_swing_wait -= (int32_t)frames;

        if (s_swing_wait < 0)
        {
            fire_step(s_swing_step, step_frames);
        }
    }

    if (step != s_last_step && step >= 0 && step < PRO_STEPS)
    {
        s_last_step = step;
        s_step = step;

        if ((step & 1) && s_swing > 0)
        {
            s_swing_step = step;   /* off-beats late by swing % of half a step */
            s_swing_wait = step_frames * s_swing / 200;
        }
        else
        {
            fire_step(step, step_frames);
        }
    }
}

void studio_pro_render(int16_t *stereo, size_t frames, bool playing, int step, int bpm)
{
    if (!s_ready)
    {
        return;
    }

    frames = frames > MAX_BLOCK ? MAX_BLOCK : frames;
    drain_events();
    sequencer(playing, step, bpm, frames);

    pro_synth_t p;
    portENTER_CRITICAL(&s_mux);
    p = s_synth;
    portEXIT_CRITICAL(&s_mux);

    float *mix = s_mix;
    memset(mix, 0, frames * sizeof(float));
    bool sounding = false;

    /* ---- samples */
    for (int v = 0; v < SAMPLE_VOICES; v++)
    {
        sample_voice_t *sv = &s_svoice[v];

        if (!sv->on)
        {
            continue;
        }

        const slot_t *s = &s_slot[sv->slot];
        const int16_t *pcm = s->pcm;
        uint32_t len = s->frames;

        if (pcm == NULL || len < 2)
        {
            sv->on = false;
            continue;
        }

        sounding = true;
        float g = sv->gain * 0.8f;   /* the mix is in 16-bit units, like the synth; headroom for chords */

        for (size_t i = 0; i < frames; i++)
        {
            uint32_t idx = (uint32_t)sv->pos;

            if (idx + 1 >= len)
            {
                sv->on = false;
                break;
            }

            float frac = (float)(sv->pos - idx);
            mix[i] += (pcm[idx] + (pcm[idx + 1] - pcm[idx]) * frac) * g;
            sv->pos += sv->step;
        }
    }

    /* ---- synth */
    float att = env_rate(p.attack), dec = env_rate(p.decay), rel = env_rate(p.release);
    float sus = p.sustain / 100.0f;
    float base_cut = 60.0f * powf(2.0f, p.cutoff / 100.0f * 7.0f);
    float reso = 1.0f - p.resonance / 100.0f * 0.92f;
    float env_amt = p.env_amount / 100.0f * 4.0f;
    float detune = powf(2.0f, p.detune / 100.0f * 0.5f / 12.0f);
    float drive = 1.0f + p.drive / 100.0f * 5.0f;
    float level = p.volume / 100.0f * 0.35f;

    for (int v = 0; v < SYNTH_VOICES; v++)
    {
        synth_voice_t *sv = &s_voice[v];

        if (sv->stage == ENV_OFF)
        {
            continue;
        }

        sounding = true;
        float dt1 = sv->freq / RATE;
        float dt2 = dt1 * detune;
        float f = 0.1f;

        for (size_t i = 0; i < frames; i++)
        {
            /* envelope */
            switch (sv->stage)
            {
                case ENV_ATTACK:
                    sv->env += att;
                    if (sv->env >= 1.0f)
                    {
                        sv->env = 1.0f;
                        sv->stage = ENV_DECAY;
                    }
                    break;
                case ENV_DECAY:
                    sv->env -= dec * (sv->env - sus + 0.01f);
                    if (sv->env <= sus + 0.001f)
                    {
                        sv->env = sus;
                        sv->stage = ENV_SUSTAIN;
                    }
                    break;
                case ENV_SUSTAIN:
                    break;
                case ENV_RELEASE:
                    sv->env -= rel * (sv->env + 0.01f);
                    if (sv->env <= 0.0005f)
                    {
                        sv->env = 0;
                        sv->stage = ENV_OFF;
                    }
                    break;
                default:
                    break;
            }

            if (sv->gate_left > 0 && --sv->gate_left == 0 && sv->stage != ENV_OFF)
            {
                sv->stage = ENV_RELEASE;
            }

            if (sv->stage == ENV_OFF)
            {
                break;
            }

            /* oscillators (polyBLEP for the edges) */
            float o1, o2;
            float p1 = sv->phase1, p2 = sv->phase2;

            switch (p.wave)
            {
                case PRO_WAVE_SQUARE:
                {
                    o1 = p1 < 0.5f ? 1.0f : -1.0f;
                    o2 = p2 < 0.5f ? 1.0f : -1.0f;
                    break;
                }
                case PRO_WAVE_TRIANGLE:
                    o1 = 4.0f * fabsf(p1 - 0.5f) - 1.0f;
                    o2 = 4.0f * fabsf(p2 - 0.5f) - 1.0f;
                    break;
                case PRO_WAVE_SINE:
                    o1 = sinf(6.2831853f * p1);
                    o2 = sinf(6.2831853f * p2);
                    break;
                case PRO_WAVE_NOISE:
                    o1 = rnd();
                    o2 = 0;
                    break;
                default:
                    o1 = 2.0f * p1 - 1.0f;
                    o2 = 2.0f * p2 - 1.0f;
                    break;
            }

            if (p.wave == PRO_WAVE_SAW || p.wave == PRO_WAVE_SQUARE)
            {
                /* polyBLEP: soften the discontinuity at phase 0 (and 0.5 for the square) */
#define BLEP(t, dt) ((t) < (dt) ? ((t) / (dt)) * 2.0f - ((t) / (dt)) * ((t) / (dt)) - 1.0f : \
                     (t) > 1.0f - (dt) ? (((t) - 1.0f) / (dt)) * (((t) - 1.0f) / (dt)) + 2.0f * (((t) - 1.0f) / (dt)) + 1.0f : 0.0f)
                o1 -= BLEP(p1, dt1);
                o2 -= BLEP(p2, dt2);

                if (p.wave == PRO_WAVE_SQUARE)
                {
                    float h1 = p1 + 0.5f, h2 = p2 + 0.5f;
                    h1 -= h1 >= 1.0f ? 1.0f : 0.0f;
                    h2 -= h2 >= 1.0f ? 1.0f : 0.0f;
                    o1 = (p1 < 0.5f ? 1.0f : -1.0f) + BLEP(p1, dt1) - BLEP(h1, dt1);
                    o2 = (p2 < 0.5f ? 1.0f : -1.0f) + BLEP(p2, dt2) - BLEP(h2, dt2);
                }
#undef BLEP
            }

            p1 += dt1;
            p2 += dt2;
            sv->phase1 = p1 >= 1.0f ? p1 - 1.0f : p1;
            sv->phase2 = p2 >= 1.0f ? p2 - 1.0f : p2;

            float x = p.wave == PRO_WAVE_NOISE ? o1 : (o1 + o2) * 0.5f;

            /* state-variable low-pass, cutoff follows the envelope */
            if ((i & 15) == 0)   /* control rate: every 16 samples is plenty for the envelope */
            {
                float fc = base_cut * powf(2.0f, env_amt * sv->env);
                f = clampf(2.0f * sinf(3.14159265f * clampf(fc, 20.0f, 8000.0f) / RATE), 0.0f, 0.9f);
            }

            sv->low += f * sv->band;
            float high = x - sv->low - reso * sv->band;
            sv->band += f * high;
            float y = sv->low;

            if (drive > 1.01f)
            {
                y *= drive;
                y = y / (1.0f + fabsf(y));
            }

            mix[i] += y * sv->env * sv->velocity * level * 32767.0f;
        }
    }

    /* ---- tempo echo (3/16 note), on the whole Pro bus */
    if (p.echo > 0 && s_echo != NULL)
    {
        uint32_t delay = (uint32_t)((int64_t)RATE * 60 / (bpm > 0 ? bpm : 120) * 3 / 4);
        delay = delay >= ECHO_FRAMES ? ECHO_FRAMES - 1 : delay;
        float wet = p.echo / 100.0f * 0.6f;

        for (size_t i = 0; i < frames; i++)
        {
            uint32_t rd = (s_echo_pos + ECHO_FRAMES - delay) % ECHO_FRAMES;
            float e = s_echo[rd];
            s_echo[s_echo_pos] = mix[i] + e * 0.42f;
            s_echo_pos = (s_echo_pos + 1) % ECHO_FRAMES;
            mix[i] += e * wet;

            if (fabsf(e) > 4.0f)
            {
                sounding = true;   /* the tail keeps the bus open */
            }
        }
    }

    if (sounding)
    {
        for (size_t i = 0; i < frames; i++)
        {
            float v = mix[i];

            for (int ch = 0; ch < 2; ch++)
            {
                float out = stereo[i * 2 + ch] + v;
                stereo[i * 2 + ch] = (int16_t)clampf(out, -32768.0f, 32767.0f);
            }
        }
    }

    s_epoch++;
}

bool studio_pro_active(void)
{
    if (!s_ready)
    {
        return false;
    }

    for (int v = 0; v < SAMPLE_VOICES; v++)
    {
        if (s_svoice[v].on)
        {
            return true;
        }
    }

    for (int v = 0; v < SYNTH_VOICES; v++)
    {
        if (s_voice[v].stage != ENV_OFF)
        {
            return true;
        }
    }

    return s_ev_head != s_ev_tail || s_synth.echo > 0;
}

bool studio_pro_init(void)
{
    s_mix = heap_caps_calloc(MAX_BLOCK, sizeof(float), MALLOC_CAP_INTERNAL);
    s_echo = heap_caps_calloc(ECHO_FRAMES, sizeof(float), MALLOC_CAP_SPIRAM);
    s_rec_buf = heap_caps_malloc(REC_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (s_mix == NULL)
    {
        return false;
    }

    s_synth = s_presets[0].p;
    memset(s_melody, -1, sizeof(s_melody));

    for (int i = 0; i < PRO_SLOTS; i++)
    {
        s_slot[i].gain = 100;
    }

    s_noise ^= esp_random();
    s_ready = true;
    s_booting = true;

    /* Saved slots and pattern from the card (quiet if missing). */
    for (int i = 0; i < PRO_SLOTS; i++)
    {
        char path[64];
        snprintf(path, sizeof(path), DIR_SAMPLES "/slot%d.wav", i + 1);
        load_wav(i, path, true);
    }

    load_project();
    s_booting = false;
    ESP_LOGI(TAG, "Studio Pro ready (%d sampler slots, %d synth voices)", PRO_SLOTS, SYNTH_VOICES);
    return true;
}
