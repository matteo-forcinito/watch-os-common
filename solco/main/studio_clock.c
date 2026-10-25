#include "studio_clock.h"
bool studio_clock_measure(studio_clock_t *c, int64_t t1, int64_t t2, int64_t t3, int64_t t4)
{
    if (t4 < t1 || t3 < t2)
    {
        return false;
    }
    int64_t rtt = (t4 - t1) - (t3 - t2);
    if (rtt < 0 || rtt > 50000)
    {
        return false;
    }
    c->rtt_us = (uint32_t)rtt;
    if (c->samples && rtt > (int64_t)c->best_rtt_us + 2000)
    {
        return false;
    }
    int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
    if (!c->samples)
    {
        c->offset_us = offset;
    }
    else
    {
        c->offset_us += (offset - c->offset_us) / 8;
    }
    if (!c->samples || rtt < c->best_rtt_us)
    {
        c->best_rtt_us = (uint32_t)rtt;
    }
    if (c->samples < 1000000)
    {
        c->samples++;
    }
    return true;
}
