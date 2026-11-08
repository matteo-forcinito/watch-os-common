#ifndef LINK_INTERCOM_H
#define LINK_INTERCOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Task-only, transient half-duplex voice link over paired EL02.
 * Start/stop are nonblocking; true from start means a worker was queued. */
bool link_intercom_start(const char *peer_id, int seconds);
void link_intercom_stop(void);
bool link_intercom_busy(void);

/* Authenticated Orbit RPC callback. Does not consume json or block on RPC. */
bool link_intercom_control(const char *from_id, const cJSON *json);
/* EL02 stream callback; bounded, nonblocking and safe on the family worker. */
void link_intercom_receive(const char *from_id, const uint8_t *data, size_t length);
/* Register the transient stream callback once during app startup. */
void link_intercom_init(void);

#endif
