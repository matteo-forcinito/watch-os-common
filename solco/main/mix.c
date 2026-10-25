#include "mix.h"
#include <math.h>

uint16_t solco_fold_mono(int16_t *pcm, size_t frames)
{
    uint16_t peak = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        int32_t value = ((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2;
        pcm[i] = (int16_t)value;
        uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    return peak;
}

void solco_mix_studio(solco_mix_t *mix, const int16_t *a, const int16_t *b, const int16_t *bus, int16_t *out,
                      size_t frames, int cross, int master, int play_a, int play_b, int enabled)
{
    if (cross < 0)
    {
        cross = 0;
    }
    if (cross > 1000)
    {
        cross = 1000;
    }
    if (master < 0)
    {
        master = 0;
    }
    if (master > 100)
    {
        master = 100;
    }
    float angle = cross * (1.57079632679f / 1000);
    float target_a = play_a ? cosf(angle) * 0.70710678f : 0;
    float target_b = play_b ? sinf(angle) * 0.70710678f : 0;
    float target_master = master * 0.01f;
    mix->peak = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        mix->a += (target_a - mix->a) * 0.004535f;
        mix->b += (target_b - mix->b) * 0.004535f;
        mix->studio += ((enabled ? 1.0f : 0.0f) - mix->studio) * 0.004535f;
        mix->master += (target_master - mix->master) * 0.004535f;
        for (int channel = 0; channel < 2; ++channel)
        {
            size_t n = i * 2 + channel;
            float value =
                ((a[n] * mix->a + b[n] * mix->b) * (1 - .5f * mix->studio) + (bus ? bus[n] * .5f * mix->studio : 0)) *
                mix->master;
            if (value > 32767)
            {
                value = 32767;
                ++mix->clipped;
            }
            if (value < -32768)
            {
                value = -32768;
                ++mix->clipped;
            }
            out[n] = (int16_t)value;
            int magnitude = out[n] < 0 ? -(int)out[n] : out[n];
            if (magnitude > mix->peak)
            {
                mix->peak = (uint16_t)magnitude;
            }
        }
    }
}

void solco_mix(solco_mix_t *mix, const int16_t *a, const int16_t *b, int16_t *out, size_t frames, int cross,
               int master, int play_a, int play_b)
{
    solco_mix_studio(mix, a, b, NULL, out, frames, cross, master, play_a, play_b, 0);
}
