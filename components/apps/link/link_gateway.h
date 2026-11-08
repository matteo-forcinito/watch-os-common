#pragma once
#include "cJSON.h"

/* Translate the supported HTTP facade to bounded Orbit commands. The caller
 * owns returned JSON. Unknown paths/methods return NULL, never a local fallback. */
cJSON *link_gateway_command(const char *path, const cJSON *body);
cJSON *link_gateway_response(const char *path, cJSON *reply); /* consumes reply */
