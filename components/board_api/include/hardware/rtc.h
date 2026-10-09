#ifndef HARDWARE_RTC_H
#define HARDWARE_RTC_H

/*
 * Real-time clock chip (PCF85063, PCF8563...): keeps counting while the ESP32 is off.
 * It always stores UTC; time zones are applied by the time service.
 */

#include <time.h>

#include "esp_err.h"

esp_err_t rtc_chip_init(void);

/* Read the time (UTC). ESP_ERR_INVALID_STATE if it lost power and was never set. */
esp_err_t rtc_chip_get_time(time_t *out);

esp_err_t rtc_chip_set_time(time_t utc);

#endif
