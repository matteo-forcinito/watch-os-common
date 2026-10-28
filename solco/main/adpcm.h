#ifndef SOLCO_ADPCM_H
#define SOLCO_ADPCM_H
/* IMA ADPCM, 4 bits per sample, mono. Each block starts from an explicit state
 * (predictor + step index), so blocks decode on their own and a lost packet never
 * corrupts the next one. Pure C, used by the stream and by the host tests. */
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int16_t predictor;
    uint8_t index;
} solco_adpcm_t;

static const int16_t solco_adpcm_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t solco_adpcm_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static inline int16_t solco_adpcm_step(solco_adpcm_t *s, uint8_t code)
{
    int step = solco_adpcm_steps[s->index];
    int diff = step >> 3;
    if (code & 4)
        diff += step;
    if (code & 2)
        diff += step >> 1;
    if (code & 1)
        diff += step >> 2;
    int value = s->predictor + (code & 8 ? -diff : diff);
    value = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
    s->predictor = (int16_t)value;
    int index = s->index + solco_adpcm_index[code & 15];
    s->index = (uint8_t)(index < 0 ? 0 : index > 88 ? 88 : index);
    return s->predictor;
}

/* Encodes n samples (n even) into n/2 bytes, low nibble first. Updates the state. */
static inline void solco_adpcm_encode(solco_adpcm_t *s, const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; ++i)
    {
        int step = solco_adpcm_steps[s->index];
        int diff = pcm[i] - s->predictor;
        uint8_t code = 0;
        if (diff < 0)
        {
            code = 8;
            diff = -diff;
        }
        if (diff >= step)
        {
            code |= 4;
            diff -= step;
        }
        if (diff >= step >> 1)
        {
            code |= 2;
            diff -= step >> 1;
        }
        if (diff >= step >> 2)
            code |= 1;
        solco_adpcm_step(s, code); /* track the decoder exactly */
        if (i & 1)
            out[i >> 1] |= (uint8_t)(code << 4);
        else
            out[i >> 1] = code;
    }
}

static inline void solco_adpcm_decode(solco_adpcm_t *s, const uint8_t *in, size_t n, int16_t *pcm)
{
    for (size_t i = 0; i < n; ++i)
        pcm[i] = solco_adpcm_step(s, (uint8_t)(i & 1 ? in[i >> 1] >> 4 : in[i >> 1] & 15));
}
#endif
