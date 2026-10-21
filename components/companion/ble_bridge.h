#ifndef BLE_BRIDGE_H
#define BLE_BRIDGE_H

/* HTTP API over the Bluetooth link, see ble_bridge.c. */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* Sends one complete line (with its '\n'); false when the link is gone. */
typedef bool (*bridge_send_fn_t)(const char *line);

void ble_bridge_init(bridge_send_fn_t send);

/* {"t":"api","id":N,"p":"/path","b":{...}} from a bonded, encrypted link. */
void ble_bridge_request(const cJSON *msg, uint16_t conn);

#endif
