#include "studio.h"
#include "studio_clock.h"
#include "studio_motion.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static studio_t s;
static int16_t out[512];
static void render(size_t frames)
{
    while (frames)
    {
        size_t n = frames > 256 ? 256 : frames;
        studio_render(&s, out, n);
        frames -= n;
    }
}
static void event(int type, int value, int velocity, uint64_t frame)
{
    studio_event_t e = {(uint8_t)type, (uint8_t)value, (uint8_t)velocity, frame};
    assert(studio_push(&s, e));
}
int main(void)
{
    assert(studio_init(&s));
    for (int i = 0; i < STUDIO_BANKS; i++)
    {
        assert(s.bank[i].pcm && s.bank[i].frames > 1000);
        int peak = 0;
        for (uint32_t n = 0; n < s.bank[i].frames; n++)
        {
            int v = abs(s.bank[i].pcm[n]);
            if (v > peak)
            {
                peak = v;
            }
        }
        assert(peak > 500 && peak < 20000);
    }
    event(STUDIO_HIT, 0, 127, 100);
    studio_render(&s, out, 100);
    assert(s.status.hits == 0);
    studio_render(&s, out, 1);
    assert(s.status.hits == 1);
    studio_render(&s, out, 200);
    bool sound = false;
    for (int i = 0; i < 400; i++)
    {
        if (out[i])
        {
            sound = true;
        }
    }
    assert(sound);
    for (int i = 0; i < 32; i++)
    {
        event(STUDIO_HIT, 0, 127, s.status.frame);
    }
    studio_render(&s, out, 256);
    assert(s.status.voices == 16);
    for (int i = 0; i < 512; i++)
    {
        assert(abs(out[i]) <= 24001);
    }
    render(44100);
    event(STUDIO_RECORD, 0, 0, s.status.frame);
    event(STUDIO_HIT, 1, 110, s.status.frame + 6000);
    render(6100);
    assert(s.status.recording && s.status.playing);
    assert(s.status.pattern.pattern[1] == 2);
    event(STUDIO_RECORD, 0, 0, s.status.frame);
    render(1);
    assert(!s.status.recording && s.status.revision == 1);
    uint32_t hits = s.status.hits;
    render(88200);
    assert(s.status.hits == hits + 1);
    event(STUDIO_PLAY, 0, 0, s.status.frame);
    render(1);
    assert(!s.status.playing);
    event(STUDIO_CLEAR, 0, 0, s.status.frame);
    render(1);
    assert(!s.status.pattern.pattern[1]);
    for (int i = 0; i < 64; i++)
    {
        event(STUDIO_HIT, 0, 110, s.status.frame + 100000 + i);
    }
    assert(!studio_push(&s, (studio_event_t){STUDIO_HIT, 0, 110, s.status.frame + 200000}));
    assert(s.status.dropped == 1);
    studio_free(&s);
    studio_clock_t clock = {0};
    assert(studio_clock_measure(&clock, 10000, 16000, 16500, 12500));
    assert(clock.offset_us == 5000 && clock.rtt_us == 2000);
    assert(!studio_clock_measure(&clock, 20000, 30000, 30500, 40500));
    assert(clock.samples == 1 && clock.offset_us == 5000);
    studio_motion_t motion = {0};
    int pad = 0, velocity = 0;
    float rest[3] = {0, 0, 1}, shake[3] = {1.2f, 0, 1};
    for (int i = 0; i < 100; i++)
    {
        assert(!studio_motion_detect(&motion, rest, NULL, (uint32_t)i * 10, 1, &pad, &velocity));
    }
    assert(studio_motion_detect(&motion, shake, NULL, 1000, 1, &pad, &velocity));
    assert(pad == 6 && velocity > 90);
    assert(!studio_motion_detect(&motion, shake, NULL, 1010, 1, &pad, &velocity));
    for (int i = 0; i < 50; i++)
    {
        studio_motion_detect(&motion, rest, NULL, 1020 + (uint32_t)i * 10, 1, &pad, &velocity);
    }
    assert(studio_motion_detect(&motion, shake, NULL, 1600, 1, &pad, &velocity));
    motion = (studio_motion_t){0};
    float forward[3] = {1.2f, 0, 1}, brake[3] = {-1.2f, 0, 1};
    assert(!studio_motion_detect(&motion, rest, NULL, 0, 2, &pad, &velocity));
    assert(!studio_motion_detect(&motion, forward, NULL, 100, 2, &pad, &velocity));
    assert(motion.learned && motion.hits == 0);
    for (int i = 0; i < 60; i++)
        studio_motion_detect(&motion, rest, NULL, 110 + i * 10, 2, &pad, &velocity);
    assert(!studio_motion_detect(&motion, brake, NULL, 720, 2, &pad,
                                 &velocity)); /* Return cannot arm. */
    for (int i = 0; i < 40; i++)
        studio_motion_detect(&motion, rest, NULL, 730 + i * 10, 2, &pad, &velocity);
    assert(!studio_motion_detect(&motion, forward, NULL, 1140, 2, &pad, &velocity));
    assert(!studio_motion_detect(&motion, forward, NULL, 1150, 2, &pad, &velocity));
    assert(studio_motion_detect(&motion, brake, NULL, 1170, 2, &pad, &velocity));
    assert(pad == 1 && motion.hits == 1);
    assert(!studio_motion_detect(&motion, brake, NULL, 1180, 2, &pad, &velocity));
    /* Sustained tilt selects a different kit component before forward onset. */
    float tilt[3] = {0, .6f, .8f};
    for (int i = 0; i < 300; i++)
        studio_motion_detect(&motion, tilt, NULL, 1200 + i * 10, 2, &pad, &velocity);
    float tilted_forward[3] = {1.2f, .6f, .8f}, tilted_brake[3] = {-1.2f, .6f, .8f};
    assert(!studio_motion_detect(&motion, tilted_forward, NULL, 4220, 2, &pad, &velocity));
    assert(studio_motion_detect(&motion, tilted_brake, NULL, 4250, 2, &pad, &velocity));
    assert(pad == 5 && motion.hits == 2);
    puts("PASS studio: 34 banks (32 sounds + 2 A/B references), scheduled onset, 16 "
         "voices/limiter, record/loop/clear, queue bound, clock, "
         "motion");
    return 0;
}
