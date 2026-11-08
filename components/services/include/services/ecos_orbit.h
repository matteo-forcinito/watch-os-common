#pragma once

#include "services/ecos.h"

/* Large Orbit commands are ordered authenticated RPC chunks. Only the final
 * chunk executes the command. Replies remain bounded/paged (ECOS_ORBIT_MAX).
 * Blocking, task-only, never call from LVGL or a family receive handler.
 * Timeout/failure is an unknown outcome; do not blindly repeat side effects. */
#define ECOS_ORBIT_REQUEST_MAX 1024
int ecos_family_orbit_large(const char *id, const uint8_t *request, size_t length,
                            uint8_t *reply, size_t max, int timeout_ms);
