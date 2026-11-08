#pragma once

/* Independently decodable 15 ms voice blocks. EL02 supplies authentication,
 * sender identity and replay protection; this codec never touches the radio. */
#include "core/adpcm.h"
#include "ecos_orbit_wire.h"
#include <stdbool.h>
#include <string.h>

#define ECOS_AUDIO_RATE 16000
#define ECOS_AUDIO_SAMPLES 240
#define ECOS_AUDIO_HEADER 22
#define ECOS_AUDIO_FRAME_MAX (ECOS_AUDIO_HEADER + ECOS_AUDIO_SAMPLES / 2)

static inline uint16_t ecos_audio_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline void ecos_audio_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint32_t ecos_audio_get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline size_t ecos_audio_encode(uint8_t *out, size_t capacity, uint64_t session, uint32_t offset,
                                      ima_adpcm_t *state, const int16_t *pcm, size_t count)
{
    if (out == NULL || state == NULL || pcm == NULL || state->index > 88 || session == 0 || count == 0 ||
        count > ECOS_AUDIO_SAMPLES || (count & 1) || capacity < ECOS_AUDIO_HEADER + count / 2 ||
        offset > UINT32_MAX - count)
        return 0;
    memcpy(out, "OA01", 4);
    ecos_orbit_put_request_id(out + 4, session);
    out[12] = (uint8_t)(offset >> 24);
    out[13] = (uint8_t)(offset >> 16);
    out[14] = (uint8_t)(offset >> 8);
    out[15] = (uint8_t)offset;
    ecos_audio_put16(out + 16, (uint16_t)count);
    ecos_audio_put16(out + 18, (uint16_t)state->predictor);
    out[20] = state->index;
    out[21] = 0;
    ima_adpcm_encode(state, pcm, count, out + ECOS_AUDIO_HEADER);
    return ECOS_AUDIO_HEADER + count / 2;
}

static inline bool ecos_audio_decode(const uint8_t *packet, size_t length, int16_t *pcm, size_t capacity,
                                     uint64_t *session, uint32_t *offset, size_t *samples)
{
    if (packet == NULL || pcm == NULL || session == NULL || offset == NULL || samples == NULL ||
        length <= ECOS_AUDIO_HEADER || length > ECOS_AUDIO_FRAME_MAX || memcmp(packet, "OA01", 4) ||
        packet[20] > 88 || packet[21] != 0)
        return false;
    size_t count = ecos_audio_get16(packet + 16);
    uint64_t id = ecos_orbit_get_request_id(packet + 4);
    uint32_t at = ecos_audio_get32(packet + 12);
    if (id == 0 || count == 0 || count > ECOS_AUDIO_SAMPLES || (count & 1) || count > capacity ||
        length != ECOS_AUDIO_HEADER + count / 2 || at > UINT32_MAX - count)
        return false;
    unsigned predictor = ecos_audio_get16(packet + 18);
    ima_adpcm_t state = {.predictor = (int16_t)(predictor > INT16_MAX ? (int)predictor - 65536 : (int)predictor),
                          .index = packet[20]};
    ima_adpcm_decode(&state, packet + ECOS_AUDIO_HEADER, count, pcm);
    *session = id;
    *offset = at;
    *samples = count;
    return true;
}

/* The start/stop control RPC chooses a session; stale streams cannot replace it. */
static inline bool ecos_audio_advance(uint64_t expected, uint32_t *next, uint64_t session,
                                      uint32_t offset, size_t count)
{
    if (next == NULL || expected == 0 || session != expected || count == 0 || count > ECOS_AUDIO_SAMPLES ||
        (count & 1) || offset < *next || offset > UINT32_MAX - count)
        return false;
    *next = offset + (uint32_t)count;
    return true;
}
