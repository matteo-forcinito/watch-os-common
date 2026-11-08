#ifndef ECOS_ORBIT_WIRE_H
#define ECOS_ORBIT_WIRE_H

/* Pure-C envelope for opaque Orbit RPC data carried by ECOS_LINK_ORBIT*.
 * The request ID is an unsigned 64-bit value in network byte order. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECOS_ORBIT_REQUEST_ID_SIZE 8
#define ECOS_ORBIT_DATA_MIN 1
#define ECOS_ORBIT_DATA_MAX 144
#define ECOS_ORBIT_ENVELOPE_MIN (ECOS_ORBIT_REQUEST_ID_SIZE + ECOS_ORBIT_DATA_MIN)
#define ECOS_ORBIT_ENVELOPE_MAX (ECOS_ORBIT_REQUEST_ID_SIZE + ECOS_ORBIT_DATA_MAX)

static inline uint64_t ecos_orbit_get_request_id(const uint8_t *p)
{
    uint64_t id = 0;
    for (size_t i = 0; i < ECOS_ORBIT_REQUEST_ID_SIZE; ++i)
        id = (id << 8) | p[i];
    return id;
}

static inline void ecos_orbit_put_request_id(uint8_t *p, uint64_t id)
{
    for (size_t i = 0; i < ECOS_ORBIT_REQUEST_ID_SIZE; ++i)
        p[i] = (uint8_t)(id >> (56 - 8 * i));
}

/* Envelope bytes are [request ID:8][opaque data:1..144]. */
static inline bool ecos_orbit_envelope_valid(const uint8_t *p, size_t n)
{
    return p != NULL && n >= ECOS_ORBIT_ENVELOPE_MIN && n <= ECOS_ORBIT_ENVELOPE_MAX &&
           ecos_orbit_get_request_id(p) != 0;
}

static inline bool ecos_orbit_pack(uint8_t *out, size_t capacity, uint64_t request_id,
                                   const uint8_t *data, size_t data_size)
{
    if (out == NULL || request_id == 0 || data == NULL || data_size < ECOS_ORBIT_DATA_MIN ||
        data_size > ECOS_ORBIT_DATA_MAX || capacity < ECOS_ORBIT_REQUEST_ID_SIZE + data_size)
        return false;
    ecos_orbit_put_request_id(out, request_id);
    for (size_t i = 0; i < data_size; ++i)
        out[ECOS_ORBIT_REQUEST_ID_SIZE + i] = data[i];
    return true;
}

static inline bool ecos_orbit_unpack(const uint8_t *envelope, size_t envelope_size,
                                     uint64_t *request_id, const uint8_t **data, size_t *data_size)
{
    if (request_id == NULL || data == NULL || data_size == NULL ||
        !ecos_orbit_envelope_valid(envelope, envelope_size))
        return false;
    *request_id = ecos_orbit_get_request_id(envelope);
    *data = envelope + ECOS_ORBIT_REQUEST_ID_SIZE;
    *data_size = envelope_size - ECOS_ORBIT_REQUEST_ID_SIZE;
    return true;
}

static inline bool ecos_orbit_ids_match(uint64_t request_id, uint64_t reply_id)
{
    return request_id != 0 && request_id == reply_id;
}

/* A retransmitted ciphertext has the same epoch and sequence. Keep only its
 * answer, never a command to execute again. The owning session supplies RAM. */
typedef struct
{
    uint64_t boot;
    uint32_t sequence;
    size_t length;
    uint8_t envelope[ECOS_ORBIT_ENVELOPE_MAX];
} ecos_orbit_cache_t;

static inline bool ecos_orbit_cache_store(ecos_orbit_cache_t *cache, uint64_t boot, uint32_t sequence,
                                          const uint8_t *envelope, size_t length)
{
    if (cache == NULL || boot == 0 || sequence == 0 || !ecos_orbit_envelope_valid(envelope, length))
        return false;
    cache->boot = boot;
    cache->sequence = sequence;
    cache->length = length;
    for (size_t i = 0; i < length; ++i) cache->envelope[i] = envelope[i];
    return true;
}

static inline bool ecos_orbit_cache_matches(const ecos_orbit_cache_t *cache, uint64_t boot, uint32_t sequence)
{
    return cache != NULL && boot != 0 && sequence != 0 && cache->boot == boot && cache->sequence == sequence &&
           ecos_orbit_envelope_valid(cache->envelope, cache->length);
}

#endif
