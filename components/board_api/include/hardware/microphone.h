#ifndef HARDWARE_MICROPHONE_H
#define HARDWARE_MICROPHONE_H

/* Local PCM input. Worker task only; exclusive with speaker output.
 * Read is bounded, reports actual samples, and never closes another owner.
 * No network, automatic capture, or microphone activation at boot. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

bool microphone_supported(void);
esp_err_t microphone_start(uint32_t rate, int channels);
esp_err_t microphone_read(int16_t *samples, size_t capacity, size_t *count);
void microphone_stop(void);

#endif
