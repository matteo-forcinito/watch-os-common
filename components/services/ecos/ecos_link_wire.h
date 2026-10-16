#ifndef ECOS_LINK_WIRE_H
#define ECOS_LINK_WIRE_H

/* Famiglia frames between paired devices (ESP-NOW). Pure C, no ESP-IDF:
 * explicit byte layout, never a C struct on the air.
 *
 *   0  "EL02"
 *   4  type
 *   5  sender MAC (6)
 *  11  receiver MAC (6)
 *  17  durable boot epoch (8) + sequence (4) -> GCM nonce (12)
 *  29  ciphertext (payload)
 *  ..  GCM tag (16)
 *
 * Bytes 0..28 are authenticated (AAD). The epoch is committed before sending;
 * the sequence survives task restarts and must never wrap. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECOS_LINK_HEADER 29
#define ECOS_LINK_TAG 16
#define ECOS_LINK_PAYLOAD_MAX 160
#define ECOS_LINK_FRAME_MAX (ECOS_LINK_HEADER + ECOS_LINK_PAYLOAD_MAX + ECOS_LINK_TAG)

#define ECOS_LINK_STATUS_REQ 1   /* "here is my status, send me yours" */
#define ECOS_LINK_STATUS 2
#define ECOS_LINK_RING 3         /* payload byte: 1 ring, 0 stop */
#define ECOS_LINK_ACCESS_REQ 5
#define ECOS_LINK_ACCESS_REPLY 6
#define ECOS_LINK_MESSAGE 4      /* payload: bounded text bytes, length from frame */

/* Every payload starts with the sender's clock (unix seconds, 0 = unknown). */
#define ECOS_LINK_TIME_SIZE 4

#define ECOS_LINK_TEXT_MAX 96

/* Status flags */
#define ECOS_LINK_F_CHARGING (1 << 0)
#define ECOS_LINK_F_USB (1 << 1)
#define ECOS_LINK_F_LISTENING (1 << 2)
#define ECOS_LINK_F_WIFI (1 << 3)
#define ECOS_LINK_F_RINGING (1 << 4)
#define ECOS_LINK_F_RECOVERY (1 << 5)

typedef struct
{
    char name[24];
    char product[32];
    char version[16];
    int8_t battery;        /* -1 unknown */
    uint8_t flags;
    uint8_t ip[4];         /* 0.0.0.0 = not on a network */
    char ssid[33];
    uint32_t uptime_min;
    uint16_t ram_kb;       /* free internal RAM */
} ecos_link_status_t;

#define ECOS_LINK_STATUS_SIZE (24 + 32 + 16 + 1 + 1 + 4 + 33 + 4 + 2)

static inline void ecos_link_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint32_t ecos_link_get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void ecos_link_put64(uint8_t *p, uint64_t v)
{
    ecos_link_put32(p, (uint32_t)(v >> 32));
    ecos_link_put32(p + 4, (uint32_t)v);
}

static inline uint64_t ecos_link_get64(const uint8_t *p)
{
    return (uint64_t)ecos_link_get32(p) << 32 | ecos_link_get32(p + 4);
}

static inline bool ecos_link_newer(uint64_t boot, uint32_t seq, uint64_t last_boot, uint32_t last_seq)
{
    return boot != 0 && seq != 0 && (boot > last_boot || (boot == last_boot && seq > last_seq));
}

static inline void ecos_link_header(uint8_t *p, uint8_t type, const uint8_t sender[6], const uint8_t receiver[6],
                                    uint64_t boot, uint32_t seq)
{
    memcpy(p, "EL02", 4);
    p[4] = type;
    memcpy(p + 5, sender, 6);
    memcpy(p + 11, receiver, 6);
    ecos_link_put64(p + 17, boot);
    ecos_link_put32(p + 25, seq);
}

/* Shape check only (the tag is checked by the caller). */
static inline bool ecos_link_valid(const uint8_t *p, size_t n)
{
    return n >= ECOS_LINK_HEADER + ECOS_LINK_TIME_SIZE + ECOS_LINK_TAG && n <= ECOS_LINK_FRAME_MAX &&
           memcmp(p, "EL02", 4) == 0 && p[4] >= ECOS_LINK_STATUS_REQ && p[4] <= ECOS_LINK_ACCESS_REPLY;
}

/* Fixed-size text field: copies at most size-1 bytes, always terminated. */
static inline void ecos_link_text(char *out, const uint8_t *in, size_t size)
{
    memcpy(out, in, size);
    out[size - 1] = 0;
    for (size_t i = 0; out[i] != 0; i++)
    {
        if ((unsigned char)out[i] < 32)
        {
            out[i] = ' ';   /* display only: no control characters */
        }
    }
}

static inline void ecos_link_put_status(uint8_t *p, const ecos_link_status_t *s)
{
    memset(p, 0, ECOS_LINK_STATUS_SIZE);
    strncpy((char *)p, s->name, 23);
    strncpy((char *)p + 24, s->product, 31);
    strncpy((char *)p + 56, s->version, 15);
    p[72] = (uint8_t)s->battery;
    p[73] = s->flags;
    memcpy(p + 74, s->ip, 4);
    strncpy((char *)p + 78, s->ssid, 32);
    ecos_link_put32(p + 111, s->uptime_min);
    p[115] = (uint8_t)(s->ram_kb >> 8);
    p[116] = (uint8_t)s->ram_kb;
}

static inline void ecos_link_get_status(const uint8_t *p, ecos_link_status_t *s)
{
    ecos_link_text(s->name, p, sizeof(s->name));
    ecos_link_text(s->product, p + 24, sizeof(s->product));
    ecos_link_text(s->version, p + 56, sizeof(s->version));
    s->battery = (int8_t)p[72];
    if (s->battery > 100 || s->battery < -1)
    {
        s->battery = -1;
    }
    s->flags = p[73];
    memcpy(s->ip, p + 74, 4);
    ecos_link_text(s->ssid, p + 78, sizeof(s->ssid));
    s->uptime_min = ecos_link_get32(p + 111);
    s->ram_kb = (uint16_t)(p[115] << 8 | p[116]);
}

#endif
