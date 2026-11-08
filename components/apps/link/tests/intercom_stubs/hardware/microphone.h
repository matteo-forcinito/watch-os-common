#ifndef INTERCOM_TEST_MIC_H
#define INTERCOM_TEST_MIC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
bool microphone_supported(void);
esp_err_t microphone_start(uint32_t rate, int channels);
esp_err_t microphone_read(int16_t *samples, size_t capacity, size_t *count);
void microphone_stop(void);
#endif
