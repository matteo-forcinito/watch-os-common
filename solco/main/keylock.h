#ifndef SOLCO_KEYLOCK_H
#define SOLCO_KEYLOCK_H
#include <stdbool.h>
#include <stdint.h>
#define SOLCO_KEYLOCK_HOP 1024
#define SOLCO_KEYLOCK_SEEK 512
/* Stereo-linked WSOLA. Storage is allocated once, outside the audio task. */
typedef struct solco_keylock
{
    int16_t grain[SOLCO_KEYLOCK_HOP * 4];
    int16_t staged[(SOLCO_KEYLOCK_HOP * 2 + SOLCO_KEYLOCK_SEEK * 2) * 2];
    int16_t mixed[SOLCO_KEYLOCK_HOP * 2];
    unsigned phase;
    int direction;
    uint32_t low, high;
    bool valid;
    float wet;
} solco_keylock_t;
typedef bool (*solco_keylock_sample_fn)(void *, int64_t, int16_t [2]);
bool solco_keylock_sample(solco_keylock_t *state, int64_t frame, int direction,
                          uint32_t length, uint32_t low, uint32_t high, bool wrap,
                          solco_keylock_sample_fn sample, void *source, int16_t out[2]);
#endif
