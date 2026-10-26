#ifndef AGENT_DECK_MODEL_H
#define AGENT_DECK_MODEL_H
#include <stdbool.h>
#include "cJSON.h"
/* Returns a bounded canonical document, never the caller's original tree. */
cJSON *agentdeck_validate(const cJSON *input);
#endif
