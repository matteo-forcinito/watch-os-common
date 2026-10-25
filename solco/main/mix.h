#ifndef SOLCO_MIX_H
#define SOLCO_MIX_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    float a;
    float b;
    float master;
    float studio;
    uint32_t clipped;
    uint16_t peak;
} solco_mix_t;

/* Stereo PCM; equal-power crossfade, 5 ms gain smoothing and saturating output. */
void solco_mix(solco_mix_t *mix, const int16_t *a, const int16_t *b, int16_t *out, size_t frames, int cross,
               int master, int play_a, int play_b);
void solco_mix_studio(solco_mix_t *mix, const int16_t *a, const int16_t *b, const int16_t *bus, int16_t *out,
                      size_t frames, int cross, int master, int play_a, int play_b, int enabled);
/* In-place L+R average for a mono monitor; returns its actual digital peak. */
uint16_t solco_fold_mono(int16_t *pcm, size_t frames);
#endif
