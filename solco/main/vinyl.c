/* Bidirectional linear resampling: no filesystem, allocation, or RTOS calls. */
#include "vinyl.h"
#include <string.h>

static int64_t bounded(int64_t value, uint32_t length, bool loop)
{
    if (loop)
    {
        return value;
    }
    int64_t end = (int64_t)length * SOLCO_SAMPLE_ONE;
    return value < 0 ? 0 : value > end ? end : value;
}

static void land_loop(solco_vinyl_t *v)
{
    int64_t low = (int64_t)v->loop_in * SOLCO_SAMPLE_ONE;
    int64_t high = (int64_t)v->loop_out * SOLCO_SAMPLE_ONE;
    if (v->loop_enabled && high > low && (v->cursor < low || v->cursor >= high))
    {
        /* Explicit range changes and scratch release land at the prefetched IN,
         * rather than a modulo point in an arbitrary uncached interior page. */
        v->cursor = v->target = low;
        v->gate = 0;
    }
}

bool solco_vinyl_loop(solco_vinyl_t *v, int action, uint32_t length)
{
    if (!length)
        return false;
    uint32_t position = (uint32_t)((v->cursor / SOLCO_SAMPLE_ONE % length + length) % length);
    if (action == 0)
    {
        v->loop_in = position;
        v->loop_out = 0;
        v->loop_marked = true;
        v->loop_enabled = false;
        return true;
    }
    if (action == 1)
    {
        if (!v->loop_marked || position < v->loop_in + 256)
            return false;
        v->loop_out = position;
        v->loop_enabled = true;
        land_loop(v);
        return true;
    }
    if (v->loop_out <= v->loop_in)
        return false;
    uint32_t span = v->loop_out - v->loop_in;
    v->shift_delta = 0;
    if (action >= SOLCO_LOOP_SHIFT_FWD && action <= SOLCO_LOOP_SHIFT_HALF_BACK)
    {
        uint32_t step = action >= SOLCO_LOOP_SHIFT_HALF_FWD ? span / 2 : span;
        bool forward = action == SOLCO_LOOP_SHIFT_FWD || action == SOLCO_LOOP_SHIFT_HALF_FWD;
        if (!v->loop_enabled || step < 256 ||
            (forward ? v->loop_out + step > length : v->loop_in < step))
            return false;
        v->loop_in = forward ? v->loop_in + step : v->loop_in - step;
        v->loop_out = v->loop_in + span;
        v->shift_delta = forward ? (int32_t)step : -(int32_t)step;
        return true; /* the cursor is moved by solco_vinyl_arm_shift at the edge */
    }
    if (action == 2)
        v->loop_enabled = !v->loop_enabled;
    else if (action == 3 && span >= 512)
        v->loop_out = v->loop_in + span / 2;
    else if (action == 4 && span <= (length - v->loop_in) / 2)
        v->loop_out += span;
    else
        return false;
    land_loop(v);
    return true;
}

void solco_vinyl_arm_shift(solco_vinyl_t *v, uint32_t loop_in, uint32_t loop_out)
{
    v->pending_in = loop_in;
    v->pending_out = loop_out;
    v->pending_shift = loop_out > loop_in;
}

void solco_vinyl_touch(solco_vinyl_t *v, int phase, int32_t frames, uint32_t length, bool loop)
{
    if (phase == 0)
    {
        v->jog_active = false;
        v->held = true;
        v->target = v->cursor;
        v->direction = 0;
    }
    else if (phase == 1 && v->held)
    {
        int direction = frames > 0 ? 1 : frames < 0 ? -1 : 0;
        if (direction && v->direction && direction != v->direction)
        {
            /* A reversal follows the new gesture, not the previous target backlog. */
            v->target = v->cursor;
            v->speed = 0;
        }
        if (direction)
            v->direction = direction;
        v->target = bounded(v->target + (int64_t)frames * SOLCO_SAMPLE_ONE, length, loop);
    }
    else if (phase == 2)
    {
        v->jog_active = false;
        v->held = false;
        land_loop(v);
    }
}

void solco_vinyl_jog(solco_vinyl_t *v, int steps, uint32_t now_ms, uint32_t length, bool loop)
{
    if (!steps || !length)
    {
        return;
    }
    steps = steps < -32 ? -32 : steps > 32 ? 32 : steps;
    bool touch_owns = v->held && !v->jog_active;
    if (!v->held)
    {
        solco_vinyl_touch(v, 0, 0, length, loop);
    }
    solco_vinyl_touch(v, 1, steps * 2205, length, loop);
    if (!touch_owns)
    {
        v->jog_active = true;
        v->jog_deadline_ms = now_ms + 120;
    }
}
void solco_vinyl_tick(solco_vinyl_t *v, uint32_t now_ms)
{
    if (v->jog_active && (int32_t)(now_ms - v->jog_deadline_ms) >= 0)
    {
        solco_vinyl_touch(v, 2, 0, 0, false);
    }
}

bool solco_vinyl_render(solco_vinyl_t *v, int16_t *out, size_t frames, bool playing,
                        uint32_t length, bool loop, solco_sample_fn sample, void *source,
                        bool *missing)
{
    memset(out, 0, frames * 2 * sizeof(*out));
    *missing = false;
    if (!length)
    {
        return false;
    }
    /* Q16 thresholds of the original servo, scaled to the Q32 cursor. */
    const int64_t q = SOLCO_SAMPLE_ONE / 65536;
    int64_t base_rate = v->play_speed ? v->play_speed : SOLCO_SAMPLE_ONE;
    if (!v->rate_initialized)
    {
        v->transport_rate = base_rate;
        v->rate_initialized = true;
    }
    int64_t desired = playing ? base_rate : 0;
    if (v->held)
    {
        /* Position servo ~25 ms, bounded at +/-4x. No queued UI seek commands. */
        int64_t velocity = (v->target - v->cursor) / 1102;
        const int64_t limit = 4 * SOLCO_SAMPLE_ONE;
        desired = velocity < -limit ? -limit : velocity > limit ? limit : velocity;
    }
    bool active = playing || v->held;
    for (size_t i = 0; i < frames; ++i)
    {
        int64_t rate_change = base_rate - v->transport_rate;
        v->transport_rate += rate_change / 1024;
        if (rate_change && rate_change < 1024 * q && rate_change > -1024 * q)
            v->transport_rate = base_rate;
        /* Roughly 120 ms rate transition; platter servo and pause stay fast. */
        int64_t goal = playing && !v->held ? v->transport_rate : desired;
        int64_t change = goal - v->speed;
        v->speed += change / 128;
        if (change && change < 128 * q && change > -128 * q)
        {
            v->speed = goal;
        }
        bool region = v->loop_enabled && !v->held && v->loop_out > v->loop_in;
        int64_t low = (int64_t)v->loop_in * SOLCO_SAMPLE_ONE;
        int64_t high = (int64_t)v->loop_out * SOLCO_SAMPLE_ONE;
        if (region && v->pending_shift && (v->cursor >= high || v->cursor < low))
        {
            /* Edge reached: switch to the shifted window. Forward: the playhead is
             * already inside it (no jump). Backward: the wrap below lands at the
             * same phase of the new window. */
            v->loop_in = v->pending_in;
            v->loop_out = v->pending_out;
            v->pending_shift = false;
            low = (int64_t)v->loop_in * SOLCO_SAMPLE_ONE;
            high = (int64_t)v->loop_out * SOLCO_SAMPLE_ONE;
        }
        if (region && (v->cursor >= high || v->cursor < low))
        {
            int64_t span = high - low;
            v->cursor = low + ((v->cursor - low) % span + span) % span;
        }
        v->cursor = bounded(v->cursor, length, loop);
        if (!loop && v->cursor >= (int64_t)length * SOLCO_SAMPLE_ONE)
        {
            if (v->speed >= 0)
            {
                v->gate = 0;
                continue;
            }
            /* EOF is one past the last sample. Reverse must enter valid data
             * before the cache lookup, or a missing sample pins the cursor here. */
            v->cursor = (int64_t)(length - 1) * SOLCO_SAMPLE_ONE;
        }
        if (!loop && !region && v->cursor == 0 && v->speed < 0)
        {
            v->gate = 0;
            continue;
        }
        int64_t frame = v->cursor / SOLCO_SAMPLE_ONE;
        int64_t fraction = v->cursor % SOLCO_SAMPLE_ONE;
        if (fraction < 0)
        {
            --frame;
            fraction += SOLCO_SAMPLE_ONE;
        }
        int16_t locked[2] = {0};
        solco_keylock_t *lock = v->keylock;
        /* Within 0.1 % of +/-1x the pitch error is under 2 cents: plain resampling,
         * no WSOLA (it was ~25 % of a block per deck). Clock trims stay here too. */
        int64_t transport = v->transport_rate < 0 ? -v->transport_rate : v->transport_rate;
        bool near_unity = transport > SOLCO_SAMPLE_ONE - SOLCO_SAMPLE_ONE / 1000 &&
                          transport < SOLCO_SAMPLE_ONE + SOLCO_SAMPLE_ONE / 1000;
        bool use_lock = lock && v->keylock_enabled && playing && !v->held && !near_unity;
        if (lock)
        {
            if (use_lock || lock->wet > 0.001f)
            {
                if (!solco_keylock_sample(lock, frame, v->speed < 0 ? -1 : 1, length,
                                         region ? v->loop_in : 0,
                                         region ? v->loop_out : length, region || loop,
                                         sample, source, locked))
                {
                    *missing = true;
                    continue;
                }
            }
            else
                lock->valid = false;
            lock->wet += ((use_lock ? 1.f : 0.f) - lock->wet) / 128.f;
            if (use_lock && lock->wet > 0.999f) lock->wet = 1.f;
            if (!use_lock && lock->wet < 0.001f) lock->wet = 0.f;
        }
        int16_t a[2] = {0}, b[2] = {0};
        int64_t next = !loop && frame + 1 >= length ? frame : frame + 1;
        if (region && next >= v->loop_out)
            next = v->loop_in;
        if ((!lock || lock->wet < 1.f) &&
            (!sample(source, frame, a) || !sample(source, next, b)))
        {
            *missing = true;
            continue; /* Hold position on a cache miss instead of skipping music. */
        }
        int64_t magnitude = v->speed < 0 ? -v->speed : v->speed;
        float target_gate = (playing || v->held) ? 1.0f : 0.0f;
        if (magnitude < 1311 * q)
        {
            target_gate *= (float)magnitude / (float)(1311 * q);
        }
        v->gate += (target_gate - v->gate) * 0.008f;
        float seam = 1.f;
        if (region)
        {
            /* Short edge taper bounds clicks on arbitrary, unquantized loop points. */
            int64_t distance = frame - v->loop_in;
            int64_t tail = (int64_t)v->loop_out - 1 - frame;
            if (tail < distance)
                distance = tail;
            if (distance < 64)
                seam = distance / 64.f;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            int32_t value =
                a[ch] + (int32_t)((int64_t)(b[ch] - a[ch]) * fraction / SOLCO_SAMPLE_ONE);
            float output = lock ? value + (locked[ch] - value) * lock->wet : (float)value;
            out[i * 2 + ch] = (int16_t)(output * v->gate * seam);
        }
        v->cursor = region ? v->cursor + v->speed : bounded(v->cursor + v->speed, length, loop);
    }
    return active;
}
