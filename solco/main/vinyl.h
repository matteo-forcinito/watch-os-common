#ifndef SOLCO_VINYL_H
#define SOLCO_VINYL_H
#include "keylock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Q16 sample cursor. A held platter follows the finger; release resumes PLAY. */
#define SOLCO_SAMPLE_ONE 65536LL
typedef struct
{
    solco_keylock_t *keylock;
    bool keylock_enabled;
    int64_t cursor, target;
    int32_t speed, play_speed; /* Zero-initialized play_speed means normal +1x. */
    int32_t transport_rate;
    bool rate_initialized;
    float gate;
    bool held;
    bool jog_active;
    uint32_t jog_deadline_ms;
    uint32_t loop_in, loop_out;
    bool loop_enabled, loop_marked;
    int direction;
} solco_vinyl_t;

typedef bool (*solco_sample_fn)(void *source, int64_t frame, int16_t stereo[2]);
/* IN, OUT+enable, toggle, half, double. Returns false for invalid ranges. */
bool solco_vinyl_loop(solco_vinyl_t *vinyl, int action, uint32_t length);
void solco_vinyl_touch(solco_vinyl_t *vinyl, int phase, int32_t frames, uint32_t length, bool loop);
/* Encoder: 50 ms of source per detent; automatic release after 120 ms idle. */
void solco_vinyl_jog(solco_vinyl_t *vinyl, int steps, uint32_t now_ms, uint32_t length, bool loop);
void solco_vinyl_tick(solco_vinyl_t *vinyl, uint32_t now_ms);
bool solco_vinyl_render(solco_vinyl_t *vinyl, int16_t *out, size_t frames, bool playing,
                        uint32_t length, bool loop, solco_sample_fn sample, void *source,
                        bool *missing);
#endif
