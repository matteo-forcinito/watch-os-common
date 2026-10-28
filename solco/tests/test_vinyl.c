/* Behavioral checks for bidirectional transport, boundaries, release and misses. */
#include "vinyl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static bool source(void *context, int64_t frame, int16_t out[2])
{
    (void)context;
    int64_t n = frame % 1000;
    if (n < 0)
    {
        n += 1000;
    }
    out[0] = (int16_t)((n - 500) * 40);
    out[1] = (int16_t)-out[0];
    return true;
}
static bool bounded_source(void *context, int64_t frame, int16_t out[2])
{
    uint32_t length = *(uint32_t *)context;
    return frame >= 0 && frame < length && source(NULL, frame, out);
}

static bool unavailable(void *context, int64_t frame, int16_t out[2])
{
    (void)context;
    (void)frame;
    (void)out;
    return false;
}

int main(void)
{
    solco_vinyl_t v = {0};
    int16_t pcm[512];
    bool missing;
    for (int i = 0; i < 100; ++i)
    {
        assert(solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing));
    }
    assert(!missing && v.cursor > 25000 * SOLCO_SAMPLE_ONE);
    solco_vinyl_touch(&v, 0, 0, 100000, false);
    solco_vinyl_touch(&v, 1, -10000, 100000, false);
    int64_t before = v.cursor;
    for (int i = 0; i < 20; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing);
    }
    assert(v.cursor < before - 8000 * SOLCO_SAMPLE_ONE);
    assert(v.speed < 0);
    for (int i = 0; i < 100; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing);
    }
    assert(llabs(v.cursor - v.target) < SOLCO_SAMPLE_ONE);
    for (int i = 0; i < 512; ++i)
    {
        assert(abs(pcm[i]) < 100);
    }
    solco_vinyl_touch(&v, 2, 0, 100000, false);
    before = v.cursor;
    for (int i = 0; i < 20; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing);
    }
    assert(!v.held && v.cursor > before + 4000 * SOLCO_SAMPLE_ONE);
    solco_vinyl_touch(&v, 0, 0, 100000, false);
    solco_vinyl_touch(&v, 1, -1000000, 100000, false);
    assert(v.target == 0);
    for (int i = 0; i < 100; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, false, 100000, false, source, NULL, &missing);
    }
    assert(v.cursor >= 0);
    solco_vinyl_touch(&v, 2, 0, 100000, false);
    for (int i = 0; i < 30; ++i)
    {
        assert(!solco_vinyl_render(&v, pcm, 256, false, 100000, false, source, NULL, &missing));
    }
    for (int i = 0; i < 512; ++i)
    {
        assert(pcm[i] == 0);
    }
    before = v.cursor;
    solco_vinyl_render(&v, pcm, 256, true, 100000, false, unavailable, NULL, &missing);
    assert(missing && v.cursor == before);
    for (int i = 0; i < 512; ++i)
    {
        assert(pcm[i] == 0);
    }
    v = (solco_vinyl_t){0};
    solco_vinyl_touch(&v, 0, 0, 1000, true);
    solco_vinyl_touch(&v, 1, -3000, 1000, true);
    for (int i = 0; i < 50; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, false, 1000, true, source, NULL, &missing);
    }
    assert(!missing && v.cursor < -2900 * SOLCO_SAMPLE_ONE);
    v = (solco_vinyl_t){.cursor = 99990 * SOLCO_SAMPLE_ONE};
    for (int i = 0; i < 10; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing);
    }
    assert(v.cursor == 100000 * SOLCO_SAMPLE_ONE && !missing);
    /* Jog release must be timed by audio, independent of UI refresh or PLAY. */
    v = (solco_vinyl_t){.cursor = 30000 * SOLCO_SAMPLE_ONE};
    solco_vinyl_jog(&v, -4, 100, 100000, false);
    assert(v.held && v.jog_active && v.target == 21180 * SOLCO_SAMPLE_ONE);
    solco_vinyl_tick(&v, 219);
    assert(v.held);
    solco_vinyl_jog(&v, 2, 200, 100000, false);
    assert(v.target == 34410 * SOLCO_SAMPLE_ONE); /* Direction change rebases on cursor. */
    solco_vinyl_tick(&v, 319);
    assert(v.held);
    solco_vinyl_tick(&v, 320);
    assert(!v.held && !v.jog_active);
    before = v.cursor;
    solco_vinyl_render(&v, pcm, 256, false, 100000, false, source, NULL, &missing);
    assert(v.cursor == before); /* Paused remains paused. */
    solco_vinyl_render(&v, pcm, 256, true, 100000, false, source, NULL, &missing);
    assert(v.cursor > before); /* PLAY resumes without a toggle. */
    solco_vinyl_jog(&v, -1, UINT32_MAX - 50, 100000, false);
    solco_vinyl_tick(&v, 68);
    assert(v.held);
    solco_vinyl_tick(&v, 69);
    assert(!v.held); /* Millisecond counter wrap. */
    solco_vinyl_touch(&v, 0, 0, 100000, false);
    solco_vinyl_jog(&v, 1, 1000, 100000, false);
    solco_vinyl_tick(&v, 2000);
    assert(v.held && !v.jog_active); /* A finger resting on the disc owns hold. */
    solco_vinyl_jog(&v, 0, 2000, 100000, false);
    solco_vinyl_touch(&v, 2, 0, 100000, false);
    assert(!v.held);
    uint32_t length = 100000;
    v = (solco_vinyl_t){.cursor = (int64_t)length * SOLCO_SAMPLE_ONE};
    solco_vinyl_touch(&v, 0, 0, length, false);
    solco_vinyl_touch(&v, 1, -5000, length, false);
    for (int i = 0; i < 30; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, false, length, false, bounded_source, &length, &missing);
        assert(!missing);
    }
    assert(v.cursor < 96000 * SOLCO_SAMPLE_ONE);
    assert(v.cursor >= 0);
    /* Reversal while the preceding target is still far ahead. */
    v = (solco_vinyl_t){.cursor = 40000 * SOLCO_SAMPLE_ONE};
    solco_vinyl_touch(&v, 0, 0, length, false);
    solco_vinyl_touch(&v, 1, 12000, length, false);
    solco_vinyl_render(&v, pcm, 256, false, length, false, bounded_source, &length, &missing);
    before = v.cursor;
    solco_vinyl_touch(&v, 1, -2000, length, false);
    solco_vinyl_render(&v, pcm, 256, false, length, false, bounded_source, &length, &missing);
    assert(v.speed < 0 && v.cursor < before);
    /* Manual loop: exact range, boundary interpolation, resizing and exit. */
    v = (solco_vinyl_t){.cursor = 10000 * SOLCO_SAMPLE_ONE};
    assert(!solco_vinyl_loop(&v, 1, length));
    assert(solco_vinyl_loop(&v, 0, length));
    v.cursor = 14000 * SOLCO_SAMPLE_ONE;
    assert(solco_vinyl_loop(&v, 1, length));
    assert(v.loop_enabled && v.loop_in == 10000 && v.loop_out == 14000);
    v.cursor = 13990 * SOLCO_SAMPLE_ONE;
    v.speed = SOLCO_SAMPLE_ONE;
    for (int i = 0; i < 50; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length, &missing);
        assert(!missing && v.cursor >= 10000 * SOLCO_SAMPLE_ONE &&
               v.cursor < 14000 * SOLCO_SAMPLE_ONE);
    }
    v.cursor = 13500 * SOLCO_SAMPLE_ONE;
    assert(solco_vinyl_loop(&v, 3, length) && v.loop_out == 12000);
    assert(v.cursor == 10000 * SOLCO_SAMPLE_ONE); /* Resize lands at cached IN. */
    assert(solco_vinyl_loop(&v, 4, length) && v.loop_out == 14000);
    assert(solco_vinyl_loop(&v, 2, length) && !v.loop_enabled);
    assert(!solco_vinyl_loop(&v, 2, 0));
    /* Fixed transport rates are separate from scratch and preserve pause. */
    const int32_t rates[] = {-131072, -98304, -65536, -32768, 32768, 65536, 98304, 131072};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r)
    {
        v = (solco_vinyl_t){.cursor = 50000 * SOLCO_SAMPLE_ONE, .play_speed = SOLCO_RATE_FROM_Q16(rates[r])};
        before = v.cursor;
        for (int i = 0; i < 10; ++i)
            solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length,
                               &missing);
        assert(!missing && v.speed == SOLCO_RATE_FROM_Q16(rates[r]));
        assert(rates[r] > 0 ? v.cursor > before : v.cursor < before);
        for (int i = 0; i < 30; ++i)
            solco_vinyl_render(&v, pcm, 256, false, length, false, bounded_source, &length,
                               &missing);
        assert(v.speed == 0 && v.play_speed == SOLCO_RATE_FROM_Q16(rates[r]));
        solco_vinyl_touch(&v, 0, 0, length, false);
        solco_vinyl_touch(&v, 1, 1000, length, false);
        solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length, &missing);
        assert(v.speed > 0); /* A forward gesture overrides reverse playback. */
        solco_vinyl_touch(&v, 2, 0, length, false);
        for (int i = 0; i < 10; ++i)
            solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length,
                               &missing);
        assert(v.speed == SOLCO_RATE_FROM_Q16(rates[r]));
    }
    v = (solco_vinyl_t){.cursor = 10 * SOLCO_SAMPLE_ONE,
                        .play_speed = SOLCO_RATE_FROM_Q16(-131072),
                        .loop_enabled = true,
                        .loop_marked = true,
                        .loop_in = 0,
                        .loop_out = 4000};
    for (int i = 0; i < 20; ++i)
        solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length, &missing);
    assert(!missing && v.cursor > 0 && v.cursor < 4000 * SOLCO_SAMPLE_ONE);
    /* Fine pitch: +1 ppm and +0.01 % advance exactly, without 15 ppm quantization. */
    const int64_t fine[] = {SOLCO_SAMPLE_ONE + SOLCO_SAMPLE_ONE / 1000000,
                            SOLCO_SAMPLE_ONE + SOLCO_SAMPLE_ONE / 10000,
                            -(SOLCO_SAMPLE_ONE - SOLCO_SAMPLE_ONE / 1250)};
    for (unsigned r = 0; r < sizeof(fine) / sizeof(fine[0]); ++r)
    {
        v = (solco_vinyl_t){.cursor = 50000 * SOLCO_SAMPLE_ONE,
                            .play_speed = fine[r],
                            .speed = fine[r],
                            .transport_rate = fine[r],
                            .rate_initialized = true,
                            .gate = 1.f};
        before = v.cursor;
        for (int i = 0; i < 100; ++i)
            solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length,
                               &missing);
        assert(!missing && v.cursor == before + fine[r] * 25600);
    }
    /* Loop shift: forward continues across the old OUT, backward keeps the phase. */
    v = (solco_vinyl_t){.cursor = 13000 * SOLCO_SAMPLE_ONE, .speed = SOLCO_SAMPLE_ONE,
                        .transport_rate = SOLCO_SAMPLE_ONE, .rate_initialized = true,
                        .loop_in = 10000, .loop_out = 14000, .loop_enabled = true,
                        .loop_marked = true, .gate = 1.f};
    solco_vinyl_t proposal = v;
    assert(solco_vinyl_loop(&proposal, SOLCO_LOOP_SHIFT_FWD, length));
    assert(proposal.loop_in == 14000 && proposal.loop_out == 18000 && proposal.shift_delta == 4000);
    solco_vinyl_arm_shift(&v, proposal.loop_in, proposal.loop_out);
    int64_t previous = v.cursor;
    for (int i = 0; i < 12; ++i)
    {
        solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length, &missing);
        assert(!missing && v.cursor > previous); /* never wraps back to 10000 */
        previous = v.cursor;
    }
    assert(!v.pending_shift && v.loop_in == 14000 && v.cursor >= 14000 * SOLCO_SAMPLE_ONE);
    proposal = v;
    assert(solco_vinyl_loop(&proposal, SOLCO_LOOP_SHIFT_BACK, length) && proposal.loop_in == 10000);
    v.cursor = 17900 * SOLCO_SAMPLE_ONE;
    solco_vinyl_arm_shift(&v, proposal.loop_in, proposal.loop_out);
    solco_vinyl_render(&v, pcm, 256, true, length, false, bounded_source, &length, &missing);
    /* 100 frames before OUT + 256 played: 156 frames into the new window. */
    assert(v.loop_in == 10000 && llabs(v.cursor - 10156 * SOLCO_SAMPLE_ONE) < SOLCO_SAMPLE_ONE);
    proposal = v;
    proposal.loop_in = 2000;
    proposal.loop_out = 6000;
    assert(!solco_vinyl_loop(&proposal, SOLCO_LOOP_SHIFT_BACK, length)); /* would go below 0 */
    puts("Vinyl behavioral, encoder jog and reverse-from-EOF tests passed");
}
