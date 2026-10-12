#ifndef HARDWARE_IMU_H
#define HARDWARE_IMU_H

/* Sensor-frame axes, without orientation remapping or calibration.
 * One worker owns start/read/stop; no calls from the LVGL task. */
#include <stdbool.h>
#include "esp_err.h"
typedef struct
{
    float acceleration_g[3];
    float angular_dps[3];
    float temperature_c;
} imu_sample_t;
bool imu_supported(void);
esp_err_t imu_start(void);
esp_err_t imu_read(imu_sample_t *sample);
esp_err_t imu_stop(void);
#endif
