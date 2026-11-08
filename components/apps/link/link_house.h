#ifndef LINK_HOUSE_H
#define LINK_HOUSE_H

#include <stdbool.h>
#include "cJSON.h"

/* Queue one household operation. The function always consumes json and never
 * waits for the household or a peer; true means queued, not completed. direct
 * marks a command whose coordinator was already selected, preventing routing
 * loops. */
bool link_house_scene(cJSON *json, bool direct);
bool link_house_timer(cJSON *json, bool direct);

#endif
