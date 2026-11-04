/* Orbit sound engine: see orbit_audio.h. All synthesis state lives in the
 * engine task; other tasks only touch the atomics and the tone queue. */
#include "orbit_audio.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "hardware/board.h"
#include "services/sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define RATE        22050
#define BLOCK       256
#define QUEUE_DESCS 4
#define FADE_S      0.6f   /* crossfade between soundscapes */
#define SLEEP_FADE_US (30LL * 1000000)
#define DROPS       4
#define VOICES      2

static const char *TAG = "orbit_audio";

typedef struct
{
    float freq0, freq1;  /* glide from freq0 to freq1 over `glide` samples */
    int glide;
    int delay;           /* samples before the voice starts */
    int length;          /* samples, 0 = idle */
    int pos;
    float level, decay;  /* decay: per-sample multiplier after the attack */
    float partial;       /* level of the 2.76x bell partial */
    float phase, phase2, env;
} voice_t;

typedef struct
{
    float c, s, step_c, step_s, env, decay, level;
} drop_t;

typedef struct
{
    uint32_t rng;
    float b0, b1, b2, brown, lp, lp2, hp_prev;
    /* waves */
    float wave_pos, wave_len;
    /* wind */
    float svf_low, svf_band, wind_fc, wind_target, wind_gain, wind_gain_target;
    int wind_countdown;
    drop_t drop[DROPS];
} synth_t;

static TaskHandle_t s_task;
static QueueHandle_t s_tones;
static atomic_int s_target = ORBIT_AMBIENT_OFF;
static atomic_int s_volume = 45;
static atomic_bool s_volume_dirty = true;
static atomic_bool s_hold;
static atomic_bool s_open;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_stop_at; /* esp_timer time, 0 = no sleep timer; under s_lock */
static int16_t s_out[BLOCK];
static synth_t s_synth;
static voice_t s_voice[VOICES];

static inline float noise(synth_t *n)
{
    n->rng ^= n->rng << 13;
    n->rng ^= n->rng >> 17;
    n->rng ^= n->rng << 5;
    return (float)(int32_t)n->rng * (1.0f / 2147483648.0f);
}

static inline float pink(synth_t *n, float white)
{
    n->b0 = 0.99765f * n->b0 + white * 0.0990460f;
    n->b1 = 0.96300f * n->b1 + white * 0.2965164f;
    n->b2 = 0.57000f * n->b2 + white * 1.0526913f;
    return (n->b0 + n->b1 + n->b2 + white * 0.1848f) * 0.16f;
}

static inline float brown(synth_t *n, float white)
{
    n->brown = (n->brown + 0.02f * white) / 1.02f;
    return n->brown * 3.2f;
}

static float frand(synth_t *n, float lo, float hi)
{
    return lo + (noise(n) * 0.5f + 0.5f) * (hi - lo);
}

static void synth_reset(synth_t *n)
{
    uint32_t seed = (uint32_t)esp_timer_get_time() | 1;
    memset(n, 0, sizeof(*n));
    n->rng = seed;
    n->wave_len = 9.0f;
    n->wind_fc = n->wind_target = 450.0f;
    n->wind_gain = n->wind_gain_target = 0.6f;
}

/* One block of the soundscape into `out` (float, about -1..1). */
static void synth_block(synth_t *n, orbit_ambient_t kind, float *out)
{
    switch (kind)
    {
    case ORBIT_AMBIENT_WHITE:
        for (int i = 0; i < BLOCK; i++) out[i] = noise(n) * 0.30f;
        break;
    case ORBIT_AMBIENT_PINK:
        for (int i = 0; i < BLOCK; i++) out[i] = pink(n, noise(n)) * 0.9f;
        break;
    case ORBIT_AMBIENT_BROWN:
        for (int i = 0; i < BLOCK; i++) out[i] = brown(n, noise(n)) * 0.9f;
        break;
    case ORBIT_AMBIENT_RAIN:
        for (int i = 0; i < BLOCK; i++)
        {
            float w = noise(n);
            float p = pink(n, w);
            n->lp += 0.25f * (p - n->lp);
            float hiss = (p - n->lp) * 0.9f + n->lp * 0.25f;
            /* ~45 drops per second, each a short decaying ping with a bit of noise */
            if ((n->rng & 0xFFFF) < 0xFFFF * 45 / RATE)
            {
                for (int d = 0; d < DROPS; d++)
                {
                    drop_t *drop = &n->drop[d];
                    if (drop->env > 0.02f) continue;
                    float f = frand(n, 1400.0f, 4200.0f) * 2.0f * (float)M_PI / RATE;
                    drop->c = 1.0f;
                    drop->s = 0.0f;
                    drop->step_c = cosf(f);
                    drop->step_s = sinf(f);
                    drop->env = 1.0f;
                    drop->decay = expf(-1.0f / (RATE * frand(n, 0.004f, 0.014f)));
                    drop->level = frand(n, 0.05f, 0.30f);
                    break;
                }
            }
            float pings = 0.0f;
            for (int d = 0; d < DROPS; d++)
            {
                drop_t *drop = &n->drop[d];
                if (drop->env <= 0.001f) continue;
                float c = drop->c * drop->step_c - drop->s * drop->step_s;
                drop->s = drop->c * drop->step_s + drop->s * drop->step_c;
                drop->c = c;
                pings += (drop->s * 0.7f + w * 0.3f) * drop->env * drop->level;
                drop->env *= drop->decay;
            }
            out[i] = hiss + pings;
        }
        break;
    case ORBIT_AMBIENT_WAVES:
    {
        /* A swell: quick rise, long wash back; length varies wave to wave. */
        n->wave_pos += (float)BLOCK / RATE;
        if (n->wave_pos >= n->wave_len)
        {
            n->wave_pos = 0.0f;
            n->wave_len = frand(n, 7.0f, 12.0f);
        }
        float x = n->wave_pos / n->wave_len;
        float e = x < 0.35f ? sinf(x / 0.35f * (float)M_PI * 0.5f) : 0.5f + 0.5f * cosf((x - 0.35f) / 0.65f * (float)M_PI);
        e = e * e;
        float a = 0.015f + 0.22f * e;
        float gain = 0.22f + 0.95f * e;
        for (int i = 0; i < BLOCK; i++)
        {
            float w = noise(n);
            float src = brown(n, w) * 0.6f + pink(n, w) * 0.8f;
            n->lp += a * (src - n->lp);
            n->lp2 += a * (n->lp - n->lp2);
            out[i] = n->lp2 * gain * 1.6f;
        }
        break;
    }
    case ORBIT_AMBIENT_WIND:
    {
        if (--n->wind_countdown <= 0)
        {
            n->wind_target = frand(n, 220.0f, 850.0f);
            n->wind_gain_target = frand(n, 0.25f, 1.0f);
            n->wind_countdown = (int)frand(n, 80.0f, 260.0f); /* 1..3 s in blocks */
        }
        n->wind_fc += (n->wind_target - n->wind_fc) * 0.01f;
        n->wind_gain += (n->wind_gain_target - n->wind_gain) * 0.006f;
        float f = 2.0f * sinf((float)M_PI * n->wind_fc / RATE);
        const float q = 0.45f;
        for (int i = 0; i < BLOCK; i++)
        {
            float w = noise(n);
            n->svf_low += f * n->svf_band;
            float high = w - n->svf_low - q * n->svf_band;
            n->svf_band += f * high;
            out[i] = (n->svf_band * 0.55f + pink(n, w) * 0.25f) * n->wind_gain;
        }
        break;
    }
    default:
        memset(out, 0, BLOCK * sizeof(float));
        break;
    }
}

static void voice_start(voice_t *v, orbit_tone_t tone, int delay)
{
    memset(v, 0, sizeof(*v));
    v->delay = delay;
    float seconds;
    switch (tone)
    {
    case ORBIT_TONE_INHALE:
        v->freq0 = 392.0f, v->freq1 = 523.25f, seconds = 1.4f, v->level = 0.30f, v->glide = RATE * 7 / 10;
        v->decay = expf(-1.0f / (RATE * 0.45f));
        break;
    case ORBIT_TONE_EXHALE:
        v->freq0 = 523.25f, v->freq1 = 392.0f, seconds = 1.4f, v->level = 0.30f, v->glide = RATE * 7 / 10;
        v->decay = expf(-1.0f / (RATE * 0.45f));
        break;
    case ORBIT_TONE_HOLD:
        v->freq0 = v->freq1 = 880.0f, seconds = 0.35f, v->level = 0.16f;
        v->decay = expf(-1.0f / (RATE * 0.07f));
        break;
    case ORBIT_TONE_BELL:
        v->freq0 = v->freq1 = 523.25f, seconds = 3.0f, v->level = 0.30f, v->partial = 0.35f;
        v->decay = expf(-1.0f / (RATE * 0.9f));
        break;
    default: /* END: second note via the other voice */
        v->freq0 = v->freq1 = 659.25f, seconds = 2.5f, v->level = 0.30f, v->partial = 0.3f;
        v->decay = expf(-1.0f / (RATE * 0.7f));
        break;
    }
    v->length = (int)(seconds * RATE);
}

/* Adds the tone voices to `out`; returns true while any voice sounds. */
static bool voices_block(float *out)
{
    bool busy = false;
    for (int k = 0; k < VOICES; k++)
    {
        voice_t *v = &s_voice[k];
        if (v->length == 0) continue;
        busy = true;
        for (int i = 0; i < BLOCK; i++)
        {
            if (v->delay > 0)
            {
                v->delay--;
                continue;
            }
            if (v->pos >= v->length)
            {
                v->length = 0;
                break;
            }
            int attack = RATE / 40; /* 25 ms */
            if (v->pos < attack) v->env = (float)v->pos / attack;
            else v->env *= v->decay;
            float t = v->glide > 0 && v->pos < v->glide ? (float)v->pos / v->glide : 1.0f;
            t = t * t * (3.0f - 2.0f * t);
            float f = v->freq0 + (v->freq1 - v->freq0) * (v->glide > 0 ? t : 1.0f);
            v->phase += 2.0f * (float)M_PI * f / RATE;
            if (v->phase > 2.0f * (float)M_PI) v->phase -= 2.0f * (float)M_PI;
            float sample = sinf(v->phase);
            if (v->partial > 0.0f)
            {
                v->phase2 += 2.0f * (float)M_PI * f * 2.76f / RATE;
                if (v->phase2 > 2.0f * (float)M_PI) v->phase2 -= 2.0f * (float)M_PI;
                sample += sinf(v->phase2) * v->partial * v->env;
            }
            out[i] += sample * v->env * v->level;
            v->pos++;
        }
    }
    return busy;
}

static void close_speaker(void)
{
    if (atomic_load(&s_open))
    {
        audio_stop();
        atomic_store(&s_open, false);
    }
}

static void engine(void *arg)
{
    (void)arg;
    static float mix[BLOCK];
    orbit_ambient_t current = ORBIT_AMBIENT_OFF;
    float gain = 0.0f;
    const float step = 1.0f / (FADE_S * RATE / BLOCK);
    for (;;)
    {
        orbit_tone_t tone;
        while (xQueueReceive(s_tones, &tone, 0) == pdTRUE)
        {
            int slot = s_voice[0].length == 0 ? 0 : 1;
            voice_start(&s_voice[slot], tone, 0);
            if (tone == ORBIT_TONE_END)
            {
                voice_start(&s_voice[slot ^ 1], ORBIT_TONE_BELL, RATE / 4);
                s_voice[slot ^ 1].freq0 = s_voice[slot ^ 1].freq1 = 880.0f;
            }
        }
        orbit_ambient_t target = (orbit_ambient_t)atomic_load(&s_target);
        float limit = 1.0f;
        int64_t stop_at;
        portENTER_CRITICAL(&s_lock);
        stop_at = s_stop_at;
        portEXIT_CRITICAL(&s_lock);
        if (stop_at != 0 && target != ORBIT_AMBIENT_OFF)
        {
            int64_t left = stop_at - esp_timer_get_time();
            if (left <= 0)
            {
                atomic_store(&s_target, ORBIT_AMBIENT_OFF);
                portENTER_CRITICAL(&s_lock);
                s_stop_at = 0;
                portEXIT_CRITICAL(&s_lock);
                target = ORBIT_AMBIENT_OFF;
                gain = 0.0f;
            }
            else if (left < SLEEP_FADE_US) limit = (float)left / SLEEP_FADE_US;
        }
        bool tones = s_voice[0].length != 0 || s_voice[1].length != 0;
        bool wanted = target != ORBIT_AMBIENT_OFF || gain > 0.0f || tones || atomic_load(&s_hold);
        if (!wanted)
        {
            close_speaker();
            current = ORBIT_AMBIENT_OFF;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (sound_alarm_playing())
        {
            /* The alarm must ring: release the speaker and drop the tones. */
            close_speaker();
            memset(s_voice, 0, sizeof(s_voice));
            gain = 0.0f;
            current = ORBIT_AMBIENT_OFF;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (!atomic_load(&s_open))
        {
            if (audio_is_running())
            {
                /* A system chime is finishing: try again shortly. */
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            esp_err_t err = audio_start_ex(RATE, 1, BLOCK, QUEUE_DESCS);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Speaker unavailable (%s)", esp_err_to_name(err));
                audio_stop();
                memset(s_voice, 0, sizeof(s_voice));
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
            atomic_store(&s_open, true);
            atomic_store(&s_volume_dirty, true);
        }
        if (atomic_exchange(&s_volume_dirty, false)) audio_set_volume(atomic_load(&s_volume));

        /* Fade the old soundscape out before the new one fades in. */
        if (current != target)
        {
            gain -= step;
            if (gain <= 0.0f)
            {
                gain = 0.0f;
                current = target;
                synth_reset(&s_synth);
            }
        }
        else if (current != ORBIT_AMBIENT_OFF && gain < 1.0f) gain = gain + step > 1.0f ? 1.0f : gain + step;
        else if (current == ORBIT_AMBIENT_OFF) gain = 0.0f;

        if (current != ORBIT_AMBIENT_OFF && gain > 0.0f) synth_block(&s_synth, current, mix);
        else memset(mix, 0, sizeof(mix));
        float level = gain * gain * limit;
        for (int i = 0; i < BLOCK; i++) mix[i] *= level;
        voices_block(mix);
        for (int i = 0; i < BLOCK; i++)
        {
            float v = mix[i];
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            s_out[i] = (int16_t)(v * 26000.0f);
        }
        if (audio_write(s_out, BLOCK) != ESP_OK)
        {
            close_speaker();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        }
    }
}

static void wake(void)
{
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

bool orbit_audio_available(void)
{
    return board_info()->has_audio;
}

void orbit_audio_init(void)
{
    if (!orbit_audio_available() || s_task != NULL) return;
    s_tones = xQueueCreate(4, sizeof(orbit_tone_t));
    if (s_tones == NULL || !sys_task_create(engine, "orbit_audio", 4096, NULL, 6, 1))
        ESP_LOGE(TAG, "Sound engine not started");
    else
    {
        /* sys_task_create() does not hand back the handle. */
        s_task = xTaskGetHandle("orbit_audio");
    }
}

void orbit_audio_set_ambient(orbit_ambient_t ambient)
{
    if (ambient < 0 || ambient >= ORBIT_AMBIENT_COUNT) ambient = ORBIT_AMBIENT_OFF;
    atomic_store(&s_target, ambient);
    wake();
}

orbit_ambient_t orbit_audio_ambient(void)
{
    return (orbit_ambient_t)atomic_load(&s_target);
}

const char *orbit_audio_name(orbit_ambient_t ambient)
{
    static const char *names[] = {"Spento", "Pioggia", "Onde", "Vento", "Rumore rosa", "Rumore marrone", "Rumore bianco"};
    return ambient >= 0 && ambient < ORBIT_AMBIENT_COUNT ? names[ambient] : names[0];
}

void orbit_audio_set_volume(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    atomic_store(&s_volume, percent);
    atomic_store(&s_volume_dirty, true);
}

int orbit_audio_volume(void)
{
    return atomic_load(&s_volume);
}

void orbit_audio_set_sleep_timer(int minutes)
{
    portENTER_CRITICAL(&s_lock);
    s_stop_at = minutes > 0 ? esp_timer_get_time() + (int64_t)minutes * 60 * 1000000 : 0;
    portEXIT_CRITICAL(&s_lock);
}

int orbit_audio_sleep_left(void)
{
    int64_t stop_at;
    portENTER_CRITICAL(&s_lock);
    stop_at = s_stop_at;
    portEXIT_CRITICAL(&s_lock);
    if (stop_at == 0) return -1;
    int64_t left = stop_at - esp_timer_get_time();
    return left > 0 ? (int)(left / 1000000) : 0;
}

void orbit_audio_tone(orbit_tone_t tone)
{
    if (s_tones == NULL) return;
    xQueueSend(s_tones, &tone, 0);
    wake();
}

void orbit_audio_hold(bool hold)
{
    atomic_store(&s_hold, hold);
    wake();
}

bool orbit_audio_active(void)
{
    return atomic_load(&s_open);
}
