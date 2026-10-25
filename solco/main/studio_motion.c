#include "studio_motion.h"
#include <math.h>

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void normalize(float a[3])
{
    float n = sqrtf(dot(a, a));
    if (n > .001f)
        for (int i = 0; i < 3; ++i)
            a[i] /= n;
}
bool studio_motion_detect(studio_motion_t *m, const float a[3], const float gyro[3], uint32_t now,
                          int mode, int *pad, int *velocity)
{
    if (!m->ready)
    {
        for (int i = 0; i < 3; ++i)
            m->gravity[i] = m->reference[i] = a[i];
        normalize(m->reference);
        m->ready = m->armed = true;
        m->last_ms = now - 300;
        m->previous_ms = now;
        return false;
    }
    float dt = (now - m->previous_ms) / 1000.f;
    m->previous_ms = now;
    if (dt > .05f)
        dt = .01f;
    if (gyro)
    {
        /* Complementary gravity estimate: gyro rotates the sensor reference;
         * acceleration corrects slowly, so a stroke does not become tilt. */
        float w[3] = {gyro[0] * .017453293f, gyro[1] * .017453293f, gyro[2] * .017453293f};
        float g[3] = {m->gravity[0], m->gravity[1], m->gravity[2]};
        m->gravity[0] += (g[1] * w[2] - g[2] * w[1]) * dt;
        m->gravity[1] += (g[2] * w[0] - g[0] * w[2]) * dt;
        m->gravity[2] += (g[0] * w[1] - g[1] * w[0]) * dt;
    }
    float delta[3];
    for (int i = 0; i < 3; ++i)
        delta[i] = a[i] - m->gravity[i];
    float magnitude = sqrtf(dot(delta, delta));
    float correction =
        mode == 2 && magnitude > .25f && fabsf(sqrtf(dot(a, a)) - 1.f) > .12f ? .002f : .025f;
    for (int i = 0; i < 3; ++i)
        m->gravity[i] += delta[i] * correction;
    if (mode == 1)
    {
        if (magnitude < .18f)
            m->armed = true;
        if (!m->armed || magnitude < .5f || (uint32_t)(now - m->last_ms) < 90)
            return false;
        m->armed = false;
        *pad = 6;
    }
    else if (mode == 2)
    {
        if (!m->learned)
        {
            if (magnitude < .9f)
                return false;
            for (int i = 0; i < 3; ++i)
                m->stroke[i] = delta[i] / magnitude;
            m->learned = true;
            m->last_ms = now;
            m->quiet = false;
            m->armed = false;
            return false; /* Teaching stroke is silent. */
        }
        if (magnitude < .22f)
        {
            if (!m->quiet)
                m->quiet_ms = now;
            m->quiet = true;
            if ((uint32_t)(now - m->quiet_ms) >= 60)
                m->armed = true;
        }
        else
            m->quiet = false;
        float forward = dot(delta, m->stroke);
        if (m->pending)
        {
            uint32_t elapsed = now - m->stroke_ms;
            if (forward > m->peak)
                m->peak = forward;
            if (elapsed > 240)
            {
                m->pending = false;
                m->armed = false;
                return false;
            }
            if (elapsed < 20 || forward > -.3f)
                return false;
            /* A return stroke begins with opposite acceleration and cannot arm.
             * Fire only at braking after a learned forward onset. */
            m->pending = false;
            m->armed = false;
            magnitude = m->peak;
            *pad = m->selected_pad;
        }
        else
        {
            if (!m->armed || forward < .7f || (uint32_t)(now - m->last_ms) < 180)
                return false;
            float g[3] = {m->gravity[0], m->gravity[1], m->gravity[2]};
            normalize(g);
            float right[3] = {1 - m->reference[0] * m->reference[0],
                              -m->reference[0] * m->reference[1],
                              -m->reference[0] * m->reference[2]};
            if (dot(right, right) < .01f)
            {
                right[0] = -m->reference[1] * m->reference[0];
                right[1] = 1 - m->reference[1] * m->reference[1];
                right[2] = -m->reference[1] * m->reference[2];
            }
            normalize(right);
            float up[3] = {m->reference[1] * right[2] - m->reference[2] * right[1],
                           m->reference[2] * right[0] - m->reference[0] * right[2],
                           m->reference[0] * right[1] - m->reference[1] * right[0]};
            float x = dot(g, right), y = dot(g, up);
            m->selected_pad = fabsf(x) > fabsf(y) && fabsf(x) > .32f ? (x > 0 ? 4 : 3)
                              : y > .32f                             ? 5
                              : y < -.32f                            ? 0
                                                                     : 1;
            m->peak = forward;
            m->stroke_ms = now;
            m->pending = true;
            m->armed = false;
            return false;
        }
    }
    else
        return false;
    m->last_ms = now;
    m->hits++;
    int strength = (int)(magnitude * 55) + 35;
    *velocity = strength > 127 ? 127 : strength;
    return true;
}
