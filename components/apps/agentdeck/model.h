#ifndef AGENT_DECK_MODEL_H
#define AGENT_DECK_MODEL_H
#include <stdbool.h>
#include "cJSON.h"
/* Returns a bounded canonical document, never the caller's original tree. */
cJSON *agentdeck_validate(const cJSON *input);
/* Original indices, so an incoming reorder cannot change a selected card. */
int agentdeck_indices(const cJSON *cards, bool all, bool recent, int *indices, int capacity);
bool agentdeck_action_allowed(const char *action);
#endif
