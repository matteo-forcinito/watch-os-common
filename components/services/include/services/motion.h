#ifndef SERVICES_MOTION_H
#define SERVICES_MOTION_H
#include "hardware/imu.h"
typedef struct
{
    imu_sample_t sample;
    uint32_t samples;
    bool running;
    bool valid;
    esp_err_t error;
} motion_snapshot_t;
esp_err_t motion_start(void);
void motion_stop(void);
void motion_snapshot(motion_snapshot_t *out);
#endif
