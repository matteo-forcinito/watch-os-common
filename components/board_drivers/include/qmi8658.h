#ifndef BOARD_DRIVERS_QMI8658_H
#define BOARD_DRIVERS_QMI8658_H
#include "driver/i2c_master.h"
#include "hardware/imu.h"
typedef struct
{
    i2c_master_dev_handle_t device;
    uint8_t saved[5];
    bool restore;
} qmi8658_t;
esp_err_t qmi8658_start(qmi8658_t *sensor, i2c_master_bus_handle_t bus, uint8_t address);
esp_err_t qmi8658_read(qmi8658_t *sensor, imu_sample_t *sample);
esp_err_t qmi8658_stop(qmi8658_t *sensor);
#endif
