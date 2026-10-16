#ifndef ECOS_ACCESS_WIRE_H
#define ECOS_ACCESS_WIRE_H
/* Optional EL02 extension: correlated, bounded replies; never a persistent token. */
#include "ecos_link_wire.h"
#define ECOS_ACCESS_WEB 1
#define ECOS_ACCESS_RESOURCES 2
#define ECOS_ACCESS_OK 0
#define ECOS_ACCESS_DENIED 1
#define ECOS_ACCESS_UNAVAILABLE 2
#define ECOS_ACCESS_RECOVERY 1
#define ECOS_ACCESS_IMU 2
#define ECOS_ACCESS_IMU_VALID 4
#define ECOS_ACCESS_CHARGING 8
#define ECOS_ACCESS_USB 16
#define ECOS_ACCESS_WIFI 32
#define ECOS_ACCESS_SD 64
#define ECOS_ACCESS_WEB_READY 128
#define ECOS_ACCESS_REQUEST_SIZE 9
#define ECOS_ACCESS_REPLY_SIZE 42

typedef struct
{
    uint64_t request;
    uint8_t operation, result, flags;
    char code[7];
    uint8_t ip[4];
    uint16_t battery_mv, ram_kb;
    int8_t battery;
    int16_t acceleration_mg[3], angular_dps[3];
    uint32_t sample_age_ms;
} ecos_access_wire_t;

static inline bool ecos_access_request_valid(const uint8_t *p, size_t n)
{
    return n == ECOS_ACCESS_REQUEST_SIZE && ecos_link_get64(p) != 0 &&
           (p[8] == ECOS_ACCESS_WEB || p[8] == ECOS_ACCESS_RESOURCES);
}

static inline void ecos_access_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint16_t ecos_access_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline void ecos_access_encode(uint8_t *p, const ecos_access_wire_t *s)
{
    memset(p, 0, ECOS_ACCESS_REPLY_SIZE);
    ecos_link_put64(p, s->request);
    p[8] = s->operation; p[9] = s->result; p[10] = s->flags;
    memcpy(p + 11, s->code, 6);
    memcpy(p + 17, s->ip, 4);
    ecos_access_put16(p + 21, s->battery_mv);
    p[23] = (uint8_t)s->battery;
    ecos_access_put16(p + 24, s->ram_kb);
    for (int i = 0; i < 3; i++)
    {
        ecos_access_put16(p + 26 + i * 2, (uint16_t)s->acceleration_mg[i]);
        ecos_access_put16(p + 32 + i * 2, (uint16_t)s->angular_dps[i]);
    }
    ecos_link_put32(p + 38, s->sample_age_ms);
}

static inline bool ecos_access_decode(const uint8_t *p, size_t n, ecos_access_wire_t *out)
{
    if (n != ECOS_ACCESS_REPLY_SIZE || ecos_link_get64(p) == 0 ||
        (p[8] != ECOS_ACCESS_WEB && p[8] != ECOS_ACCESS_RESOURCES) ||
        p[9] > ECOS_ACCESS_UNAVAILABLE)
    {
        return false;
    }
    bool code = p[8] == ECOS_ACCESS_WEB && p[9] == ECOS_ACCESS_OK && !(p[10] & ECOS_ACCESS_RECOVERY);
    for (int i = 0; i < 6; i++)
    {
        if (code ? (p[11 + i] < '0' || p[11 + i] > '9') : p[11 + i] != 0)
        {
            return false;
        }
    }
    if ((p[10] & ECOS_ACCESS_IMU_VALID) && !(p[10] & ECOS_ACCESS_IMU))
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->request = ecos_link_get64(p);
    out->operation = p[8]; out->result = p[9]; out->flags = p[10];
    memcpy(out->code, p + 11, 6);
    memcpy(out->ip, p + 17, 4);
    out->battery_mv = ecos_access_get16(p + 21);
    out->battery = p[23] == 255 ? -1 : (p[23] <= 100 ? (int8_t)p[23] : -1);
    out->ram_kb = ecos_access_get16(p + 24);
    for (int i = 0; i < 3; i++)
    {
        out->acceleration_mg[i] = (int16_t)ecos_access_get16(p + 26 + i * 2);
        out->angular_dps[i] = (int16_t)ecos_access_get16(p + 32 + i * 2);
    }
    out->sample_age_ms = ecos_link_get32(p + 38);
    return true;
}
#endif
