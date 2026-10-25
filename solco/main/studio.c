/* Original drum models and sampled additive instruments. Generate outside audio. */
#include "studio.h"
#include "studio_sine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define calloc(n, size) heap_caps_calloc((n), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#define TAU 6.28318530718f
/* Linear interpolation is below PCM16 quantization for one bounded period. */
static float sine(float phase)
{
    float position = phase * (1024.0f / TAU);
    uint32_t index = (uint32_t)position;
    float fraction = position - index;
    index &= 1023;
    return s_studio_sine[index] + (s_studio_sine[index + 1] - s_studio_sine[index]) * fraction;
}
static float noise(studio_t *s)
{
    s->random ^= s->random << 13;
    s->random ^= s->random >> 17;
    s->random ^= s->random << 5;
    return ((s->random >> 8) * (1.0f / 8388608.0f)) - 1;
}
static bool drum(studio_t *s, int id, int slot, float seconds, bool reference)
{
    studio_sample_t *b = &s->bank[slot];
    b->frames = (uint32_t)(seconds * STUDIO_RATE);
    b->pcm = calloc(b->frames, sizeof(int16_t));
    if (!b->pcm)
    {
        return false;
    }
    float phase = 0, phase2 = 0, phase3 = 0, low = 0, envelope = 1, sweep = 1;
    float decay = expf(-1.0f / (STUDIO_RATE * (id == 0   ? .12f
                                               : id == 1 ? .075f
                                               : id == 2 ? .06f
                                               : id == 3 ? .018f
                                               : id == 4 ? .12f
                                               : id == 5 ? .1f
                                               : id == 6 ? .035f
                                                         : .035f)));
    if (!reference && (id == 0 || id == 5))
    {
        decay = expf(-1.0f / (STUDIO_RATE * (id == 0 ? .075f : .08f)));
    }
    float click = 1, click_decay = expf(-1.0f / (STUDIO_RATE * (id == 0 ? .003f : .004f)));
    float sweep_decay = expf(-1.0f / (STUDIO_RATE * .025f));
    for (uint32_t i = 0; i < b->frames; i++)
    {
        float n = noise(s);
        low += (n - low) * .19f;
        float t = i * (1.0f / STUDIO_RATE), value = 0;
        if (id == 0)
        {
            phase += TAU * ((reference ? 48 : 80) + 110 * sweep) / STUDIO_RATE;
            if (phase > TAU)
            {
                phase -= TAU;
            }
            value =
                reference
                    ? sine(phase) * envelope
                    : (.65f * sine(phase) + .25f * sine(2 * phase) * sweep + .10f * sine(3 * phase) * sweep) *
                              envelope +
                          .05f * (n - low) * click;
        }
        else if (id == 1)
        {
            phase += TAU * 185 / STUDIO_RATE;
            value = (.75f * (n - low) + .25f * sine(phase)) * envelope;
        }
        else if (id == 2)
        {
            float pulses = t < .011f ? 1 : t < .018f ? .15f : t < .029f ? .8f : t < .036f ? .15f : 1;
            value = (n - low) * envelope * pulses;
        }
        else if (id == 3 || id == 4)
        {
            value = (n - low) * envelope * .75f;
        }
        else if (id == 5)
        {
            float hz = (reference ? 105 : 155) + 35 * sweep;
            phase += TAU * hz / STUDIO_RATE;
            if (phase > TAU)
            {
                phase -= TAU;
            }
            phase2 += TAU * hz * 1.59f / STUDIO_RATE;
            phase3 += TAU * hz * 2.30f / STUDIO_RATE;
            if (phase2 > TAU)
            {
                phase2 -= TAU;
            }
            if (phase3 > TAU)
            {
                phase3 -= TAU;
            }
            value = reference
                        ? sine(phase) * envelope
                        : (.72f * sine(phase) + .20f * sine(phase2) * sweep + .08f * sine(phase3) * sweep) *
                                  envelope +
                              .03f * (n - low) * click;
        }
        else if (id == 6)
        {
            value = (n - low) * envelope * (.5f + .5f * sine(TAU * 87 * t)) * .8f;
        }
        else
        {
            value = (sine(TAU * 860 * t) + .5f * sine(TAU * 1730 * t)) * envelope * .6f;
        }
        float attack = i < 32 ? i / 32.0f : 1;
        float tail = b->frames - i < 256 ? (b->frames - i) / 256.0f : 1;
        b->pcm[i] = (int16_t)(value * attack * tail * 10000);
        envelope *= decay;
        click *= click_decay;
        sweep *= sweep_decay;
    }
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
    return true;
}
static bool instrument(studio_t *s, int patch, int note)
{
    static const int scale[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    studio_sample_t *b = &s->bank[8 + patch * 8 + note];
    b->frames = STUDIO_RATE;
    b->pcm = calloc(b->frames, sizeof(int16_t));
    if (!b->pcm)
    {
        return false;
    }
    float hz = (patch == 1 ? 65.4064f : 261.6256f) * powf(2, scale[note] / 12.0f);
    float phase = 0, phase2 = 0, phase3 = 0, env = 1, overtone = 1;
    float decay = expf(-1.0f / (STUDIO_RATE * (patch == 0 ? .32f : patch == 1 ? .18f : .4f)));
    float harmonic_decay = expf(-1.0f / (STUDIO_RATE * .08f));
    for (uint32_t i = 0; i < b->frames; i++)
    {
        phase += TAU * hz / STUDIO_RATE;
        if (phase > TAU)
        {
            phase -= TAU;
        }
        float v;
        if (patch == 0)
        {
            v = .76f * sine(phase) + .24f * sine(phase * 2) * overtone;
        }
        else if (patch == 1)
        {
            v = .7f * sine(phase) + .2f * sine(phase * 2) + .1f * sine(phase * 3) * overtone;
        }
        else
        {
            phase2 += TAU * hz * 2.756f / STUDIO_RATE;
            phase3 += TAU * hz * 5.404f / STUDIO_RATE;
            if (phase2 > TAU)
            {
                phase2 -= TAU;
            }
            if (phase3 > TAU)
            {
                phase3 -= TAU;
            }
            v = .7f * sine(phase) + .2f * sine(phase2) * overtone + .1f * sine(phase3) * overtone;
        }
        float attack = i < 96 ? i / 96.0f : 1;
        float tail = b->frames - i < 256 ? (b->frames - i) / 256.0f : 1;
        b->pcm[i] = (int16_t)(v * env * attack * tail * 10000);
        env *= decay;
        overtone *= harmonic_decay;
    }
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
    return true;
}
bool studio_init(studio_t *s)
{
    memset(s, 0, sizeof(*s));
    s->random = 0x615d42a3;
    s->limiter = 1;
    s->status.bpm = 120;
    s->status.kit = 1;
    s->status.pattern.bpm = 120;
    s->status.pattern.version = 1;
    s->last_step = -1;
    static const float duration[8] = {.6f, .35f, .3f, .09f, .5f, .45f, .18f, .2f};
    for (int i = 0; i < 8; i++)
    {
        if (!drum(s, i, i, duration[i], false))
        {
            studio_free(s);
            return false;
        }
    }
    if (!drum(s, 0, 32, duration[0], true) || !drum(s, 5, 33, duration[5], true))
    {
        studio_free(s);
        return false;
    }
    for (int p = 0; p < 3; p++)
    {
        for (int n = 0; n < 8; n++)
        {
            if (!instrument(s, p, n))
            {
                studio_free(s);
                return false;
            }
        }
    }
    return true;
}
void studio_free(studio_t *s)
{
    for (int i = 0; i < STUDIO_BANKS; i++)
    {
        free(s->bank[i].pcm);
        s->bank[i].pcm = NULL;
    }
}
void studio_restore(studio_t *s, const studio_pattern_t *p)
{
    if (p->version != 1 || p->bpm < 60 || p->bpm > 200)
    {
        return;
    }
    s->status.pattern = *p;
    s->status.bpm = p->bpm;
    memcpy(s->playback, p->pattern, sizeof(s->playback));
}
bool studio_push(studio_t *s, studio_event_t e)
{
    if (s->events == STUDIO_EVENTS)
    {
        s->status.dropped++;
        return false;
    }
    if (!e.frame)
    {
        e.frame = s->status.frame;
    }
    if (e.frame < s->status.frame)
    {
        e.frame = s->status.frame;
        s->status.late++;
    }
    size_t i = s->events++;
    while (i && s->event[i - 1].frame > e.frame)
    {
        s->event[i] = s->event[i - 1];
        --i;
    }
    s->event[i] = e;
    return true;
}
static void hit(studio_t *s, int bank, int velocity)
{
    if (!s->status.kit && (bank == 0 || bank == 5))
    {
        bank = bank == 0 ? 32 : 33;
    }
    if (bank < 0 || bank >= STUDIO_BANKS || velocity <= 0)
    {
        return;
    }
    int selected = 0;
    for (int i = 0; i < STUDIO_VOICES; i++)
    {
        if (!s->voice[i].on)
        {
            selected = i;
            break;
        }
        if (s->voice[i].age < s->voice[selected].age)
        {
            selected = i;
        }
    }
    studio_voice_t *v = &s->voice[selected];
    int tail = v->on ? v->last : 0;
    *v = (studio_voice_t){.on = true,
                          .bank = bank,
                          .velocity = velocity > 127 ? 127 : velocity,
                          .age = ++s->status.hits,
                          .tail = tail,
                          .tail_left = tail ? 64 : 0};
}
static void apply(studio_t *s, studio_event_t e)
{
    if (e.type == STUDIO_HIT)
    {
        hit(s, e.value, e.velocity);
        if (s->status.recording && e.value < 8)
        {
            unsigned step = (unsigned)(s->phase >> 32) & 15;
            s->status.pattern.pattern[e.value] |= (uint16_t)(1u << step);
        }
    }
    else if (e.type == STUDIO_PLAY)
    {
        s->status.playing = !s->status.playing;
        s->phase = 0;
        s->last_step = -1;
        if (!s->status.playing)
        {
            s->status.recording = false;
            s->status.revision++;
        }
    }
    else if (e.type == STUDIO_RECORD)
    {
        s->status.recording = !s->status.recording;
        if (s->status.recording && !s->status.playing)
        {
            s->status.playing = true;
            s->phase = 0;
            s->last_step = -1;
        }
        if (!s->status.recording)
        {
            s->status.revision++;
        }
    }
    else if (e.type == STUDIO_CLEAR)
    {
        memset(s->status.pattern.pattern, 0, sizeof(s->status.pattern.pattern));
        memset(s->playback, 0, sizeof(s->playback));
        s->status.revision++;
    }
    else if (e.type == STUDIO_TEMPO)
    {
        s->status.bpm = e.value < 60 ? 60 : e.value > 200 ? 200 : e.value;
        s->status.pattern.bpm = (uint16_t)s->status.bpm;
        s->status.revision++;
    }
    else if (e.type == STUDIO_KIT)
    {
        s->status.kit = e.value ? 1 : 0;
    }
    else if (e.type == STUDIO_ENABLE)
    {
        s->status.enabled = e.value != 0;
    }
}
void studio_render(studio_t *s, int16_t *out, size_t frames)
{
    uint64_t increment = ((uint64_t)s->status.bpm * 4 << 32) / (60 * STUDIO_RATE);
    for (size_t i = 0; i < frames; i++, s->status.frame++)
    {
        while (s->events && s->event[0].frame <= s->status.frame)
        {
            studio_event_t e = s->event[0];
            memmove(s->event, s->event + 1, (--s->events) * sizeof(*s->event));
            apply(s, e);
            increment = ((uint64_t)s->status.bpm * 4 << 32) / (60 * STUDIO_RATE);
        }
        if (s->status.playing)
        {
            int step = (int)(s->phase >> 32) & 15;
            if (step != s->last_step)
            {
                if (step == 0)
                {
                    memcpy(s->playback, s->status.pattern.pattern, sizeof(s->playback));
                }
                for (int pad = 0; pad < 8; pad++)
                {
                    if (s->playback[pad] & (1u << step))
                    {
                        hit(s, pad, 105);
                    }
                }
                s->last_step = step;
            }
            s->status.step = step;
            s->phase = (s->phase + increment) & 0xfffffffffULL;
        }
        int32_t sum = 0;
        int voices = 0;
        for (int n = 0; n < STUDIO_VOICES; n++)
        {
            studio_voice_t *v = &s->voice[n];
            if (!v->on)
            {
                continue;
            }
            studio_sample_t *b = &s->bank[v->bank];
            int value = b->pcm[v->pos] * v->velocity / 127;
            if (v->pos < 32)
            {
                value = value * (int)v->pos / 32;
            }
            if (v->tail_left)
            {
                value += v->tail * v->tail_left / 64;
                v->tail_left--;
            }
            v->last = value;
            sum += value;
            voices++;
            if (++v->pos >= b->frames)
            {
                v->on = false;
            }
        }
        s->status.voices = voices;
        int32_t magnitude = sum < 0 ? -sum : sum;
        float target = magnitude > 24000 ? 24000.0f / magnitude : 1;
        if (target < s->limiter)
        {
            s->limiter = target;
        }
        else
        {
            s->limiter += (target - s->limiter) * .00045f;
        }
        int16_t value = (int16_t)(sum * s->limiter);
        out[i * 2] = out[i * 2 + 1] = value;
    }
}
