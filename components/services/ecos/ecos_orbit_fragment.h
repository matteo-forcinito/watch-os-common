#ifndef ECOS_ORBIT_FRAGMENT_H
#define ECOS_ORBIT_FRAGMENT_H

/* Bounded, ordered RPC reassembly. The owning peer session lives in PSRAM.
 * The transport authenticates and replay-checks each chunk before this code.
 * Completion is marked BEFORE calling the application: a duplicate final chunk
 * can retrieve its reply, but can never execute the application a second time. */
#include "ecos_orbit_wire.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECOS_ORBIT_LARGE_MAX 1024
#define ECOS_ORBIT_FRAGMENT_HEADER 17
#define ECOS_ORBIT_FRAGMENT_DATA (ECOS_ORBIT_DATA_MAX - ECOS_ORBIT_FRAGMENT_HEADER)
#define ECOS_ORBIT_FRAGMENT_ACK 7

typedef struct
{
    uint64_t id;
    uint16_t total, used;
    bool complete;
    size_t reply_length;
    uint8_t data[ECOS_ORBIT_LARGE_MAX];
    uint8_t reply[ECOS_ORBIT_DATA_MAX];
} ecos_orbit_fragment_t;

typedef enum
{
    ECOS_ORBIT_FRAGMENT_INVALID,
    ECOS_ORBIT_FRAGMENT_MORE,
    ECOS_ORBIT_FRAGMENT_READY,
    ECOS_ORBIT_FRAGMENT_REPLIED,
} ecos_orbit_fragment_result_t;

static inline uint16_t ecos_orbit_fragment_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline void ecos_orbit_fragment_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline size_t ecos_orbit_fragment_pack(uint8_t *out, size_t capacity, uint64_t id,
                                             const uint8_t *request, size_t total, size_t offset)
{
    if (out == NULL || request == NULL || id == 0 || total == 0 || total > ECOS_ORBIT_LARGE_MAX || offset >= total)
        return 0;
    size_t count = total - offset;
    if (count > ECOS_ORBIT_FRAGMENT_DATA) count = ECOS_ORBIT_FRAGMENT_DATA;
    if (capacity < ECOS_ORBIT_FRAGMENT_HEADER + count) return 0;
    memcpy(out, "OF01", 4);
    ecos_orbit_put_request_id(out + 4, id);
    ecos_orbit_fragment_put16(out + 12, (uint16_t)total);
    ecos_orbit_fragment_put16(out + 14, (uint16_t)offset);
    out[16] = offset + count == total ? 1 : 0;
    memcpy(out + ECOS_ORBIT_FRAGMENT_HEADER, request + offset, count);
    return ECOS_ORBIT_FRAGMENT_HEADER + count;
}

static inline ecos_orbit_fragment_result_t ecos_orbit_fragment_accept(ecos_orbit_fragment_t *state,
                                                                     const uint8_t *packet, size_t length)
{
    if (state == NULL || packet == NULL || length <= ECOS_ORBIT_FRAGMENT_HEADER || length > ECOS_ORBIT_DATA_MAX ||
        memcmp(packet, "OF01", 4) != 0 || packet[16] > 1)
        return ECOS_ORBIT_FRAGMENT_INVALID;
    uint64_t id = ecos_orbit_get_request_id(packet + 4);
    size_t total = ecos_orbit_fragment_get16(packet + 12);
    size_t offset = ecos_orbit_fragment_get16(packet + 14);
    size_t count = length - ECOS_ORBIT_FRAGMENT_HEADER;
    bool final = packet[16] != 0;
    if (id == 0 || total == 0 || total > ECOS_ORBIT_LARGE_MAX || offset >= total || count > total - offset ||
        final != (offset + count == total))
        return ECOS_ORBIT_FRAGMENT_INVALID;
    if (id != state->id)
    {
        if (offset != 0) return ECOS_ORBIT_FRAGMENT_INVALID;
        state->id = id;
        state->total = (uint16_t)total;
        state->used = 0;
        state->complete = false;
        state->reply_length = 0;
    }
    if (total != state->total || offset > state->used) return ECOS_ORBIT_FRAGMENT_INVALID;
    if (offset < state->used || state->complete)
    {
        /* Reject altered retransmissions and partial overlap. */
        if (offset + count > state->used || memcmp(state->data + offset, packet + ECOS_ORBIT_FRAGMENT_HEADER, count))
            return ECOS_ORBIT_FRAGMENT_INVALID;
        return final && state->complete ? ECOS_ORBIT_FRAGMENT_REPLIED : ECOS_ORBIT_FRAGMENT_MORE;
    }
    memcpy(state->data + offset, packet + ECOS_ORBIT_FRAGMENT_HEADER, count);
    state->used = (uint16_t)(offset + count);
    if (!final) return ECOS_ORBIT_FRAGMENT_MORE;
    state->complete = true;
    return ECOS_ORBIT_FRAGMENT_READY;
}

static inline size_t ecos_orbit_fragment_ack(uint8_t *out, size_t capacity, uint16_t next)
{
    if (out == NULL || capacity < ECOS_ORBIT_FRAGMENT_ACK || next > ECOS_ORBIT_LARGE_MAX) return 0;
    memcpy(out, "OF01", 4);
    out[4] = 0;
    ecos_orbit_fragment_put16(out + 5, next);
    return ECOS_ORBIT_FRAGMENT_ACK;
}

static inline bool ecos_orbit_fragment_ack_matches(const uint8_t *reply, size_t length, size_t next)
{
    return reply != NULL && length == ECOS_ORBIT_FRAGMENT_ACK && memcmp(reply, "OF01", 4) == 0 && reply[4] == 0 &&
           ecos_orbit_fragment_get16(reply + 5) == next;
}

#endif
