#pragma once
#include <stdbool.h>
#include "cJSON.h"

typedef bool (*ble_ecos_send_t)(const char *line, unsigned epoch);
void ble_ecos_init(ble_ecos_send_t send);
/* Call only after checking that the NUS connection is bonded and encrypted. */
void ble_ecos_request(const cJSON *request, unsigned epoch);
void ble_ecos_disconnect(unsigned epoch);
