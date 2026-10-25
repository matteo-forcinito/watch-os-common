#ifndef SOLCO_STUDIO_CLOCK_H
#define SOLCO_STUDIO_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
typedef struct
{
    int64_t offset_us;
    uint32_t best_rtt_us, rtt_us, samples;
} studio_clock_t;
bool studio_clock_measure(studio_clock_t *clock, int64_t t1, int64_t t2, int64_t t3, int64_t t4);
#endif
