#pragma once
#include "cJSON.h"

/* Authenticated caller owns the result; relay borrows its envelope. Shared
 * by direct BLE dispatch and the optional dashboard HTTP facade. */
cJSON *link_family_snapshot(void);
cJSON *link_family_relay(const cJSON *envelope, int *status);
