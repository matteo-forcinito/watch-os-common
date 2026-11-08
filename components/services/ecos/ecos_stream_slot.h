#pragma once

/* One lossy mailbox, owned by the family worker and stored in PSRAM. External
 * synchronization protects calls; no allocation, radio operation or callback
 * occurs here. A newer packet replaces one that has not been transmitted. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECOS_STREAM_DATA_MAX 142
typedef struct
{
    bool active;
    uint8_t mac[6];
    uint8_t length;
    uint8_t data[ECOS_STREAM_DATA_MAX];
} ecos_stream_slot_t;

static inline bool ecos_stream_open(ecos_stream_slot_t *slot, const uint8_t mac[6])
{
    if (slot == NULL || mac == NULL || slot->active) return false;
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i) any |= mac[i];
    if (any == 0 || (mac[0] & 1)) return false;
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, 6);
    slot->active = true;
    return true;
}

static inline bool ecos_stream_matches(const ecos_stream_slot_t *slot, const uint8_t mac[6])
{
    return slot != NULL && mac != NULL && slot->active && memcmp(slot->mac, mac, 6) == 0;
}

static inline bool ecos_stream_put(ecos_stream_slot_t *slot, const uint8_t mac[6], const uint8_t *data, size_t length)
{
    if (!ecos_stream_matches(slot, mac) || data == NULL || length == 0 || length > ECOS_STREAM_DATA_MAX) return false;
    memcpy(slot->data, data, length);
    slot->length = (uint8_t)length;
    return true;
}

static inline size_t ecos_stream_take(ecos_stream_slot_t *slot, uint8_t mac[6], uint8_t *data, size_t capacity)
{
    if (slot == NULL || !slot->active || mac == NULL || data == NULL || slot->length == 0 || capacity < slot->length)
        return 0;
    size_t length = slot->length;
    memcpy(mac, slot->mac, 6);
    memcpy(data, slot->data, length);
    slot->length = 0;
    return length;
}

static inline void ecos_stream_close(ecos_stream_slot_t *slot, const uint8_t mac[6])
{
    if (ecos_stream_matches(slot, mac)) memset(slot, 0, sizeof(*slot));
}
