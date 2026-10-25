/* Original bounded WSOLA implementation: one stereo alignment for both channels.
 * No FFT, heap allocation, filesystem or RTOS calls on the render path. */
#include "keylock.h"
#include <string.h>
#define H SOLCO_KEYLOCK_HOP
#define S SOLCO_KEYLOCK_SEEK

static float score(const solco_keylock_t *s, int candidate)
{
    int32_t dot = 0, energy = 1;
    /* Decimated correlation, coarse-to-fine search. Stereo energy avoids
     * cancellation on anti-phase material and keeps the stereo image linked. */
    for (int i = 0; i < 512; i += 8)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            int a = s->grain[(H + i) * 2 + ch] / 128;
            int b = s->staged[(candidate + i) * 2 + ch] / 128;
            dot += a * b;
            energy += b * b;
        }
    }
    return (float)dot * (float)(dot < 0 ? -dot : dot) / (float)energy;
}

bool solco_keylock_sample(solco_keylock_t *s, int64_t frame, int direction,
                          uint32_t length, uint32_t low, uint32_t high, bool wrap,
                          solco_keylock_sample_fn sample, void *source, int16_t out[2])
{
    if (!s->valid || s->direction != direction || s->low != low || s->high != high)
    {
        if (s->valid && s->phase < H)
        {
            /* Preserve the next overlap at an arbitrary output phase. Read
             * backwards like memmove: the predicted tail may overlap grain. */
            for (int i = H - 1; i >= 0; --i)
            {
                unsigned at = s->phase + (unsigned)i;
                for (int ch = 0; ch < 2; ++ch)
                    s->grain[(H + i) * 2 + ch] = at < H
                        ? s->mixed[at * 2 + ch] : s->grain[at * 2 + ch];
            }
        }
        s->phase = H;
    }
    if (s->phase >= H)
    {
        /* Candidate reads are staged once; the search reuses this contiguous
         * array instead of repeatedly touching the SD cache through callbacks. */
        int64_t at = frame - (int64_t)direction * S;
        bool region = wrap && high > low;
        if (region)
        {
            int64_t span = high - low;
            at = low + (at - low) % span;
            if (at < low) at += span;
        }
        for (int i = 0; i < H * 2 + S * 2; ++i)
        {
            int16_t *p = &s->staged[i * 2];
            if (at < 0 || at >= length)
                p[0] = p[1] = 0;
            else if (!sample(source, at, p))
                return false;
            at += direction;
            if (region && at >= high) at = low;
            else if (region && at < low) at = high - 1;
        }
        int best = S;
        if (s->valid)
        {
            float maximum = score(s, S);
            for (int j = 0; j <= S * 2; j += 8)
            {
                float value = score(s, j);
                if (value > maximum)
                {
                    maximum = value;
                    best = j;
                }
            }
            int first = best > 7 ? best - 7 : 0;
            int last = best + 7 < S * 2 ? best + 7 : S * 2;
            for (int j = first; j <= last; ++j)
            {
                float value = score(s, j);
                if (value > maximum)
                {
                    maximum = value;
                    best = j;
                }
            }
        }
        for (int i = 0; i < H; ++i)
        {
            /* Complementary raised-cosine approximation (smoothstep): no
             * per-sample transcendental calls and unity sum at every point. */
            float x = (float)i / (H - 1);
            float w = x * x * (3.f - 2.f * x);
            for (int ch = 0; ch < 2; ++ch)
            {
                int old = s->valid ? s->grain[(H + i) * 2 + ch] : 0;
                int next = s->staged[(best + i) * 2 + ch];
                s->mixed[i * 2 + ch] = (int16_t)(old + (next - old) * w);
            }
        }
        memcpy(s->grain, s->staged + best * 2, sizeof(s->grain));
        s->phase = 0;
        s->direction = direction;
        s->low = low;
        s->high = high;
        s->valid = true;
    }
    out[0] = s->mixed[s->phase * 2];
    out[1] = s->mixed[s->phase * 2 + 1];
    ++s->phase;
    return true;
}
