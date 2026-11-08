#ifndef LINK_ROUTES_H
#define LINK_ROUTES_H

#include <stdbool.h>
#include "cJSON.h"

/* Call once during app startup with the local 12 digit MAC identifier. */
bool link_routes_init(const char *self_id);
/* LWW route table: {"type":null|string,"target":null|string}. */
cJSON *link_routes_config(const cJSON *request);
/* Queue a best-effort event forward. Always returns without waiting for radio. */
void link_routes_emit(const char *type, const char *text, int value);

#endif
