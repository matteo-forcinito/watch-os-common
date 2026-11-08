#ifndef INTERCOM_TEST_AUDIO_H
#define INTERCOM_TEST_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t audio_start(uint32_t rate, int channels);
esp_err_t audio_write(const int16_t *samples, size_t count);
void audio_stop(void);
#endif
