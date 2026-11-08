#pragma once

/* Read-only ECOS v1 credential layout, shared by the standalone Remote app.
 * An ECOS record does not grant HID access; only an explicit Remote grant does. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HID_ECOS_PEERS 6
#define HID_ECOS_RECORD 62
#define HID_ECOS_STORE (2 + HID_ECOS_PEERS * HID_ECOS_RECORD)

static inline bool hid_ecos_mac(const uint8_t mac[6])
{
    if (mac == NULL || (mac[0] & 1) != 0) return false;
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i) any |= mac[i];
    return any != 0;
}

static inline bool hid_ecos_store(const uint8_t *store, size_t length)
{
    if (store == NULL || length != HID_ECOS_STORE || store[0] != 1 || store[1] > HID_ECOS_PEERS)
        return false;
    for (unsigned i = 0; i < store[1]; ++i)
    {
        const uint8_t *record = store + 2 + i * HID_ECOS_RECORD;
        if (!hid_ecos_mac(record)) return false;
        uint8_t any = 0;
        for (unsigned j = 0; j < 32; ++j) any |= record[30 + j];
        if (any == 0) return false;
        for (unsigned j = 0; j < i; ++j)
            if (memcmp(record, store + 2 + j * HID_ECOS_RECORD, 6) == 0) return false;
    }
    return true;
}

static inline const uint8_t *hid_ecos_find(const uint8_t store[HID_ECOS_STORE], const uint8_t mac[6])
{
    if (!hid_ecos_store(store, HID_ECOS_STORE) || !hid_ecos_mac(mac)) return NULL;
    for (unsigned i = 0; i < store[1]; ++i)
    {
        const uint8_t *record = store + 2 + i * HID_ECOS_RECORD;
        if (memcmp(record, mac, 6) == 0) return record;
    }
    return NULL;
}

/* Same context in both directions, separate from EL02 and Solco keys.
 * Roles do not enter the key; RH02 already binds both MACs in AEAD AAD. */
static inline bool hid_ecos_context(const uint8_t self[6], const uint8_t peer[6], uint8_t context[24])
{
    if (!hid_ecos_mac(self) || !hid_ecos_mac(peer) || memcmp(self, peer, 6) == 0) return false;
    memcpy(context, "RHUB-ECOS-v1", 12);
    bool first = memcmp(self, peer, 6) < 0;
    memcpy(context + 12, first ? self : peer, 6);
    memcpy(context + 18, first ? peer : self, 6);
    return true;
}
