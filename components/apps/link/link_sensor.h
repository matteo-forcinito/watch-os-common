#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/* Compact cached samples. Values are integers in the named unit, keeping
 * three axes within one RPC even when the floating point source is noisy. */
cJSON *link_sensor_json(const char *part, bool supported, bool running, bool valid,
                        int64_t age_ms, const float acceleration[3], const float angular[3], float temperature);
