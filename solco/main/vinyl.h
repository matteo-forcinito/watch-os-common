#ifndef SOLCO_VINYL_H
#define SOLCO_VINYL_H
#include "keylock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Q32.32 sample cursor and rates: 2.3e-4 ppm speed resolution for pitch faders and
 * clock trims. A held platter follows the finger; release resumes PLAY. */
#define SOLCO_SAMPLE_ONE 4294967296LL
/* Public rates stay Q16 (1x = 65536); internal ones are Q32. */
#define SOLCO_RATE_FROM_Q16(rate) ((int64_t)(rate) * 65536)
#define SOLCO_RATE_TO_Q16(rate) ((int32_t)((rate) / 65536))
typedef struct
{
    solco_keylock_t *keylock;
    bool keylock_enabled;
    int64_t cursor, target;
    int64_t speed, play_speed; /* Q32; zero-initialized play_speed means normal +1x. */
    int64_t transport_rate;
    bool rate_initialized;
    float gate;
    bool held;
    bool jog_active;
    uint32_t jog_deadline_ms;
    uint32_t loop_in, loop_out;
    bool loop_enabled, loop_marked;
    int direction;
    /* Loop shift (M10b): the next window replaces the current one when the
     * playhead crosses a loop edge, so forward shifts continue without a jump. */
    uint32_t pending_in, pending_out;
    bool pending_shift;
    int32_t shift_delta; /* set by actions 5-8 on a proposal, frames */
} solco_vinyl_t;

typedef bool (*solco_sample_fn)(void *source, int64_t frame, int16_t stereo[2]);
/* 0 IN, 1 OUT+enable, 2 toggle, 3 half, 4 double, 5/6 shift forward/back by the loop
 * length, 7/8 shift by half the length. Returns false for invalid ranges. */
#define SOLCO_LOOP_SHIFT_FWD 5
#define SOLCO_LOOP_SHIFT_BACK 6
#define SOLCO_LOOP_SHIFT_HALF_FWD 7
#define SOLCO_LOOP_SHIFT_HALF_BACK 8
/* Arm a shift on the live transport: applied at the next loop edge. */
void solco_vinyl_arm_shift(solco_vinyl_t *vinyl, uint32_t loop_in, uint32_t loop_out);
bool solco_vinyl_loop(solco_vinyl_t *vinyl, int action, uint32_t length);
void solco_vinyl_touch(solco_vinyl_t *vinyl, int phase, int32_t frames, uint32_t length, bool loop);
/* Encoder: 50 ms of source per detent; automatic release after 120 ms idle. */
void solco_vinyl_jog(solco_vinyl_t *vinyl, int steps, uint32_t now_ms, uint32_t length, bool loop);
void solco_vinyl_tick(solco_vinyl_t *vinyl, uint32_t now_ms);
bool solco_vinyl_render(solco_vinyl_t *vinyl, int16_t *out, size_t frames, bool playing,
                        uint32_t length, bool loop, solco_sample_fn sample, void *source,
                        bool *missing);
#endif
