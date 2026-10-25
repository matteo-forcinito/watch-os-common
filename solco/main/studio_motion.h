#ifndef SOLCO_MOTION_H
#define SOLCO_MOTION_H
#include <stdbool.h>
#include <stdint.h>
typedef struct
{
    float gravity[3], reference[3], stroke[3];
    float peak;
    bool learned, pending, quiet;
    uint32_t stroke_ms, quiet_ms, previous_ms;
    int selected_pad;
    bool ready, armed;
    uint32_t last_ms, hits;
} studio_motion_t;
/* Mode 2 learns a forward stroke, triggers on braking, maps relative tilt to kit. */
bool studio_motion_detect(studio_motion_t *motion, const float acceleration[3],
                          const float angular_dps[3], uint32_t now_ms, int mode, int *pad,
                          int *velocity);
#endif
