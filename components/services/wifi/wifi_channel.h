#ifndef WATCH_WIFI_CHANNEL_H
#define WATCH_WIFI_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure policy state. The owner bytes are an opaque, stable four-byte tag. */
#define WIFI_CHANNEL_OWNER_SIZE 4
#define WIFI_CHANNEL_RESERVATIONS 4
#define WIFI_CHANNEL_MIN 1
#define WIFI_CHANNEL_MAX 13

typedef struct
{
    uint8_t owner[WIFI_CHANNEL_OWNER_SIZE];
    uint8_t channel;
    bool used;
} wifi_channel_reservation_t;

typedef struct
{
    wifi_channel_reservation_t reservations[WIFI_CHANNEL_RESERVATIONS];
} wifi_channel_policy_t;

static inline bool wifi_channel_valid(uint8_t channel)
{
    return channel >= WIFI_CHANNEL_MIN && channel <= WIFI_CHANNEL_MAX;
}

static inline bool wifi_channel_owner_valid(
    const uint8_t owner[WIFI_CHANNEL_OWNER_SIZE])
{
    if (owner == NULL)
    {
        return false;
    }
    uint8_t any = 0;
    for (unsigned i = 0; i < WIFI_CHANNEL_OWNER_SIZE; ++i)
    {
        any |= owner[i];
    }
    return any != 0;
}

static inline uint8_t wifi_channel_pinned(const wifi_channel_policy_t *policy)
{
    if (policy == NULL)
    {
        return 0;
    }
    for (unsigned i = 0; i < WIFI_CHANNEL_RESERVATIONS; ++i)
    {
        if (policy->reservations[i].used)
        {
            return policy->reservations[i].channel;
        }
    }
    return 0;
}

static inline bool wifi_channel_owner_has_reservation(
    const wifi_channel_policy_t *policy,
    const uint8_t owner[WIFI_CHANNEL_OWNER_SIZE])
{
    if (policy == NULL || !wifi_channel_owner_valid(owner))
    {
        return false;
    }
    for (unsigned i = 0; i < WIFI_CHANNEL_RESERVATIONS; ++i)
    {
        const wifi_channel_reservation_t *entry = &policy->reservations[i];
        if (!entry->used)
        {
            continue;
        }
        bool same = true;
        for (unsigned j = 0; j < WIFI_CHANNEL_OWNER_SIZE; ++j)
        {
            if (entry->owner[j] != owner[j])
            {
                same = false;
                break;
            }
        }
        if (same)
        {
            return true;
        }
    }
    return false;
}

static inline bool wifi_channel_allowed(const wifi_channel_policy_t *policy,
                                       uint8_t target, bool network_bound,
                                       uint8_t current_channel)
{
    if (!wifi_channel_valid(target))
    {
        return false;
    }
    if (network_bound)
    {
        return wifi_channel_valid(current_channel) && target == current_channel;
    }
    uint8_t pinned = wifi_channel_pinned(policy);
    return pinned == 0 || target == pinned;
}

/* policy must be zero-initialized before first use. */
static inline bool wifi_channel_reserve(
    wifi_channel_policy_t *policy,
    const uint8_t owner[WIFI_CHANNEL_OWNER_SIZE], uint8_t channel,
    bool network_bound, uint8_t current_channel)
{
    if (policy == NULL || !wifi_channel_owner_valid(owner) ||
        !wifi_channel_valid(channel) ||
        (network_bound &&
         (!wifi_channel_valid(current_channel) || channel != current_channel)))
    {
        return false;
    }

    unsigned free_slot = WIFI_CHANNEL_RESERVATIONS;
    unsigned owner_slot = WIFI_CHANNEL_RESERVATIONS;
    unsigned used = 0;
    uint8_t pinned = 0;
    for (unsigned i = 0; i < WIFI_CHANNEL_RESERVATIONS; ++i)
    {
        const wifi_channel_reservation_t *entry = &policy->reservations[i];
        if (!entry->used)
        {
            if (free_slot == WIFI_CHANNEL_RESERVATIONS)
            {
                free_slot = i;
            }
            continue;
        }
        ++used;
        pinned = entry->channel;
        if (owner_slot == WIFI_CHANNEL_RESERVATIONS)
        {
            bool same = true;
            for (unsigned j = 0; j < WIFI_CHANNEL_OWNER_SIZE; ++j)
            {
                if (entry->owner[j] != owner[j])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                owner_slot = i;
            }
        }
    }

    if (owner_slot != WIFI_CHANNEL_RESERVATIONS)
    {
        wifi_channel_reservation_t *entry = &policy->reservations[owner_slot];
        if (entry->channel == channel)
        {
            return true;
        }
        /* A sole owner may repin, provided the caller's network permits it. */
        if (used != 1)
        {
            return false;
        }
        entry->channel = channel;
        return true;
    }

    if (used != 0 && pinned != channel)
    {
        return false;
    }
    if (free_slot == WIFI_CHANNEL_RESERVATIONS)
    {
        return false;
    }
    wifi_channel_reservation_t *entry = &policy->reservations[free_slot];
    for (unsigned j = 0; j < WIFI_CHANNEL_OWNER_SIZE; ++j)
    {
        entry->owner[j] = owner[j];
    }
    entry->channel = channel;
    entry->used = true;
    return true;
}
/* Releasing an absent but valid owner succeeds, making cleanup idempotent. */
static inline bool wifi_channel_release(
    wifi_channel_policy_t *policy,
    const uint8_t owner[WIFI_CHANNEL_OWNER_SIZE])
{
    if (policy == NULL || !wifi_channel_owner_valid(owner))
    {
        return false;
    }
    for (unsigned i = 0; i < WIFI_CHANNEL_RESERVATIONS; ++i)
    {
        wifi_channel_reservation_t *entry = &policy->reservations[i];
        if (!entry->used)
        {
            continue;
        }
        bool same = true;
        for (unsigned j = 0; j < WIFI_CHANNEL_OWNER_SIZE; ++j)
        {
            if (entry->owner[j] != owner[j])
            {
                same = false;
                break;
            }
        }
        if (!same)
        {
            continue;
        }
        entry->used = false;
        entry->channel = 0;
        for (unsigned j = 0; j < WIFI_CHANNEL_OWNER_SIZE; ++j)
        {
            entry->owner[j] = 0;
        }
        break;
    }
    return true;
}

#endif
