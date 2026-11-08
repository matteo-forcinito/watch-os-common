#ifndef SERVICES_ORBIT_STATE_H
#define SERVICES_ORBIT_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORBIT_STATE_KEY_MAX 48
#define ORBIT_STATE_VALUE_MAX 64

/* Initialize once during service startup. Store data lives in PSRAM; the
 * durable Lamport counter uses NVS and is never erased by this service. */
bool orbit_state_init(void);

/* Local writes commit a fresh Lamport counter before changing memory. */
bool orbit_state_put(const uint8_t *key, size_t key_len,
                     const uint8_t *value, size_t value_len);
bool orbit_state_delete(const uint8_t *key, size_t key_len);

/* Returns false for missing keys and tombstones. A present value that exceeds
 * value_capacity is rejected without truncation. */
bool orbit_state_get(const uint8_t *key, size_t key_len,
                     uint8_t *value, size_t value_capacity, size_t *value_len,
                     uint64_t *clock, uint8_t author[6]);

/* Parent Orbit JSON dispatcher delegates OS01 binary requests here. This only
 * mutates/reads the local store; it never makes another RPC or creates a task. */
size_t orbit_state_handle_rpc(const char *from_id, const uint8_t *request, size_t length,
                              uint8_t *reply, size_t max);

/* Call periodically from an existing service tick. Schedules at most one
 * transient worker, only while ECOS radio is already active. */
void orbit_state_tick(void);

#endif
