#ifndef ECOS_WIRE_H
#define ECOS_WIRE_H

/* Versioned byte encoding, independent of ESP-IDF and native struct padding. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECOS_PORT 42178
#define ECOS_HEADER 17
#define ECOS_HELLO_SIZE 105
#define ECOS_FRAME_MAX (ECOS_HEADER + ECOS_HELLO_SIZE)
#define ECOS_HELLO 1
#define ECOS_CONFIRM 2
#define ECOS_SAVED 3
#define ECOS_COMMIT 4
#define ECOS_TRANSCRIPT_SIZE (8 + 12 + 2 * ECOS_HELLO_SIZE)

static inline void ecos_wire_transcript(uint8_t out[ECOS_TRANSCRIPT_SIZE], const uint8_t local_mac[6],
                                        const uint8_t peer_mac[6], const uint8_t local[ECOS_HELLO_SIZE],
                                        const uint8_t peer[ECOS_HELLO_SIZE])
{
    bool first = memcmp(local_mac, peer_mac, 6) < 0;
    memcpy(out, "EC-PAIR1", 8);
    memcpy(out + 8, first ? local_mac : peer_mac, 6);
    memcpy(out + 14, first ? peer_mac : local_mac, 6);
    memcpy(out + 20, first ? local : peer, ECOS_HELLO_SIZE);
    memcpy(out + 20 + ECOS_HELLO_SIZE, first ? peer : local, ECOS_HELLO_SIZE);
}

static inline bool ecos_wire_valid(const uint8_t *p, size_t n)
{
    return n >= ECOS_HEADER && memcmp(p, "EC01", 4) == 0 &&
           ((p[4] == ECOS_HELLO && n == ECOS_FRAME_MAX) ||
            ((p[4] == ECOS_CONFIRM || p[4] == ECOS_SAVED || p[4] == ECOS_COMMIT) && n == ECOS_HEADER + 32));
}

static inline void ecos_wire_header(uint8_t *p, uint8_t kind, const uint8_t sender[6], const uint8_t receiver[6])
{
    memcpy(p, "EC01", 4);
    p[4] = kind;
    memcpy(p + 5, sender, 6);
    memcpy(p + 11, receiver, 6);
}

static inline bool ecos_wire_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned difference = 0;
    for (size_t i = 0; i < n; i++)
    {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

#endif
