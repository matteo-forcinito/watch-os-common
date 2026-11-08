#ifndef ECOS_ORBIT_MODEL_H
#define ECOS_ORBIT_MODEL_H

/* Pure-C replicated Orbit model: caller-owned storage, no ESP-IDF/NVS calls. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Four routes for each of six peers plus self must leave space for channel
 * and house configuration. Entries and gossip snapshots live in PSRAM. */
#define ORBIT_MODEL_CAPACITY 48
#define ORBIT_MODEL_KEY_MAX 48
#define ORBIT_MODEL_VALUE_MAX 64
#define ORBIT_MODEL_MAC_SIZE 6
#define ORBIT_MODEL_WIRE_MAX (ORBIT_MODEL_WIRE_HEADER + ORBIT_MODEL_KEY_MAX + ORBIT_MODEL_VALUE_MAX)
#define ORBIT_MODEL_WIRE_VERSION 1
#define ORBIT_MODEL_WIRE_HEADER 19
#define ORBIT_MODEL_OP_PUT 1
#define ORBIT_MODEL_OP_DELETE 2

typedef struct
{
    bool used;
    bool tombstone;
    uint8_t key_len;
    uint8_t value_len;
    uint64_t clock;
    uint8_t author[ORBIT_MODEL_MAC_SIZE];
    uint8_t key[ORBIT_MODEL_KEY_MAX];
    uint8_t value[ORBIT_MODEL_VALUE_MAX];
} orbit_model_entry_t;

typedef struct
{
    orbit_model_entry_t *entries;
    size_t capacity;
    size_t count;
    uint64_t observed_clock;
} orbit_model_store_t;

typedef struct
{
    bool awake;
    bool companion;
    bool powered;
    bool battery_known;
    uint8_t battery;
    uint8_t name_len;
    uint8_t name[ORBIT_MODEL_KEY_MAX];
    uint8_t mac[ORBIT_MODEL_MAC_SIZE];
} orbit_model_node_t;

static inline bool orbit_model_store_valid(const orbit_model_store_t *store)
{
    return store != NULL && store->entries != NULL && store->capacity > 0 &&
           store->capacity <= ORBIT_MODEL_CAPACITY && store->count <= store->capacity;
}

static inline bool orbit_model_store_init(orbit_model_store_t *store, orbit_model_entry_t *entries,
                                          size_t capacity)
{
    if (store == NULL || entries == NULL || capacity == 0 || capacity > ORBIT_MODEL_CAPACITY)
        return false;
    memset(entries, 0, capacity * sizeof(*entries));
    store->entries = entries;
    store->capacity = capacity;
    store->count = 0;
    store->observed_clock = 0;
    return true;
}

static inline bool orbit_model_author_valid(const uint8_t author[ORBIT_MODEL_MAC_SIZE])
{
    if (author == NULL) return false;
    uint8_t bits = 0;
    for (size_t i = 0; i < ORBIT_MODEL_MAC_SIZE; ++i) bits |= author[i];
    return bits != 0;
}

static inline bool orbit_model_entry_valid(const orbit_model_entry_t *entry)
{
    return entry != NULL && entry->used && entry->key_len > 0 && entry->key_len <= ORBIT_MODEL_KEY_MAX &&
           entry->value_len <= ORBIT_MODEL_VALUE_MAX && entry->clock != 0 &&
           orbit_model_author_valid(entry->author) && (!entry->tombstone || entry->value_len == 0);
}

static inline int orbit_model_version_compare(const orbit_model_entry_t *left, const orbit_model_entry_t *right)
{
    if (left->clock < right->clock) return -1;
    if (left->clock > right->clock) return 1;
    int mac = memcmp(left->author, right->author, ORBIT_MODEL_MAC_SIZE);
    return mac < 0 ? -1 : mac > 0 ? 1 : 0;
}

static inline bool orbit_model_same_key(const orbit_model_entry_t *entry, const uint8_t *key, size_t key_len)
{
    return entry->used && entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0;
}

static inline bool orbit_model_same_version_payload(const orbit_model_entry_t *a, const orbit_model_entry_t *b)
{
    return a->tombstone == b->tombstone && a->key_len == b->key_len && a->value_len == b->value_len &&
           memcmp(a->key, b->key, a->key_len) == 0 && memcmp(a->value, b->value, a->value_len) == 0;
}

/* Observe clock before winner/capacity checks so a losing valid remote event
 * still advances the caller's Lamport clock. */
static inline bool orbit_model_merge(orbit_model_store_t *store, const orbit_model_entry_t *incoming)
{
    if (!orbit_model_store_valid(store) || !orbit_model_entry_valid(incoming)) return false;
    if (incoming->clock > store->observed_clock) store->observed_clock = incoming->clock;

    size_t free_slot = store->capacity;
    for (size_t i = 0; i < store->capacity; ++i)
    {
        orbit_model_entry_t *current = &store->entries[i];
        if (!current->used)
        {
            if (free_slot == store->capacity) free_slot = i;
            continue;
        }
        if (!orbit_model_same_key(current, incoming->key, incoming->key_len)) continue;

        int order = orbit_model_version_compare(incoming, current);
        if (order < 0) return true;
        if (order == 0)
        {
            /* A (clock, author) pair names one update. Reject conflicting
             * payloads instead of making arrival order affect convergence. */
            return orbit_model_same_version_payload(current, incoming);
        }
        *current = *incoming;
        return true;
    }
    if (free_slot == store->capacity) return false;
    store->entries[free_slot] = *incoming;
    store->count++;
    return true;
}

static inline const orbit_model_entry_t *orbit_model_get(const orbit_model_store_t *store,
                                                          const uint8_t *key, size_t key_len)
{
    if (!orbit_model_store_valid(store) || key == NULL || key_len == 0 || key_len > ORBIT_MODEL_KEY_MAX)
        return NULL;
    for (size_t i = 0; i < store->capacity; ++i)
        if (orbit_model_same_key(&store->entries[i], key, key_len)) return &store->entries[i];
    return NULL;
}

static inline bool orbit_model_next_clock(const orbit_model_store_t *store, uint64_t persisted_clock,
                                          uint64_t *next_clock)
{
    if (!orbit_model_store_valid(store) || next_clock == NULL) return false;
    uint64_t base = persisted_clock > store->observed_clock ? persisted_clock : store->observed_clock;
    if (base == UINT64_MAX) return false;
    *next_clock = base + 1;
    return true;
}

static inline bool orbit_model_local_update(orbit_model_store_t *store, const uint8_t *key, size_t key_len,
                                            const uint8_t *value, size_t value_len, bool tombstone,
                                            uint64_t persisted_next_clock,
                                            const uint8_t author[ORBIT_MODEL_MAC_SIZE])
{
    if (!orbit_model_store_valid(store) || key == NULL || key_len == 0 || key_len > ORBIT_MODEL_KEY_MAX ||
        value_len > ORBIT_MODEL_VALUE_MAX || (value_len != 0 && value == NULL) ||
        (tombstone && value_len != 0) || persisted_next_clock == 0 ||
        persisted_next_clock <= store->observed_clock || !orbit_model_author_valid(author))
        return false;
    orbit_model_entry_t update = {0};
    update.used = true;
    update.tombstone = tombstone;
    update.key_len = (uint8_t)key_len;
    update.value_len = (uint8_t)value_len;
    update.clock = persisted_next_clock;
    memcpy(update.author, author, ORBIT_MODEL_MAC_SIZE);
    memcpy(update.key, key, key_len);
    if (value_len != 0) memcpy(update.value, value, value_len);
    return orbit_model_merge(store, &update);
}

static inline bool orbit_model_put(orbit_model_store_t *store, const uint8_t *key, size_t key_len,
                                   const uint8_t *value, size_t value_len, uint64_t persisted_next_clock,
                                   const uint8_t author[ORBIT_MODEL_MAC_SIZE])
{
    return orbit_model_local_update(store, key, key_len, value, value_len, false,
                                    persisted_next_clock, author);
}

static inline bool orbit_model_delete(orbit_model_store_t *store, const uint8_t *key, size_t key_len,
                                      uint64_t persisted_next_clock,
                                      const uint8_t author[ORBIT_MODEL_MAC_SIZE])
{
    return orbit_model_local_update(store, key, key_len, NULL, 0, true,
                                    persisted_next_clock, author);
}

static inline void orbit_model_put64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static inline uint64_t orbit_model_get64(const uint8_t *in)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | in[i];
    return value;
}

/* [version, op, key_len, value_len, flags=0, clock_be64, author6, key, value]. */
static inline size_t orbit_model_encode(const orbit_model_entry_t *entry, uint8_t *out, size_t capacity)
{
    if (!orbit_model_entry_valid(entry) || out == NULL) return 0;
    size_t length = ORBIT_MODEL_WIRE_HEADER + entry->key_len + entry->value_len;
    if (length > ORBIT_MODEL_WIRE_MAX || capacity < length) return 0;
    out[0] = ORBIT_MODEL_WIRE_VERSION;
    out[1] = entry->tombstone ? ORBIT_MODEL_OP_DELETE : ORBIT_MODEL_OP_PUT;
    out[2] = entry->key_len;
    out[3] = entry->value_len;
    out[4] = 0;
    orbit_model_put64(out + 5, entry->clock);
    memcpy(out + 13, entry->author, ORBIT_MODEL_MAC_SIZE);
    memcpy(out + ORBIT_MODEL_WIRE_HEADER, entry->key, entry->key_len);
    memcpy(out + ORBIT_MODEL_WIRE_HEADER + entry->key_len, entry->value, entry->value_len);
    return length;
}

static inline bool orbit_model_decode(const uint8_t *wire, size_t length, orbit_model_entry_t *entry)
{
    if (wire == NULL || entry == NULL || length < ORBIT_MODEL_WIRE_HEADER || length > ORBIT_MODEL_WIRE_MAX ||
        wire[0] != ORBIT_MODEL_WIRE_VERSION || wire[4] != 0 || wire[2] == 0 ||
        wire[2] > ORBIT_MODEL_KEY_MAX || wire[3] > ORBIT_MODEL_VALUE_MAX ||
        length != ORBIT_MODEL_WIRE_HEADER + wire[2] + wire[3] ||
        (wire[1] != ORBIT_MODEL_OP_PUT && wire[1] != ORBIT_MODEL_OP_DELETE) ||
        (wire[1] == ORBIT_MODEL_OP_DELETE && wire[3] != 0))
        return false;
    orbit_model_entry_t decoded = {0};
    decoded.used = true;
    decoded.tombstone = wire[1] == ORBIT_MODEL_OP_DELETE;
    decoded.key_len = wire[2];
    decoded.value_len = wire[3];
    decoded.clock = orbit_model_get64(wire + 5);
    memcpy(decoded.author, wire + 13, ORBIT_MODEL_MAC_SIZE);
    memcpy(decoded.key, wire + ORBIT_MODEL_WIRE_HEADER, decoded.key_len);
    if (decoded.value_len != 0)
        memcpy(decoded.value, wire + ORBIT_MODEL_WIRE_HEADER + decoded.key_len, decoded.value_len);
    if (!orbit_model_entry_valid(&decoded)) return false;
    *entry = decoded;
    return true;
}

static inline int orbit_model_name_compare(const orbit_model_node_t *a, const orbit_model_node_t *b)
{
    size_t common = a->name_len < b->name_len ? a->name_len : b->name_len;
    int order = memcmp(a->name, b->name, common);
    if (order != 0) return order;
    return a->name_len < b->name_len ? -1 : a->name_len > b->name_len ? 1 : 0;
}

static inline bool orbit_model_node_better(const orbit_model_node_t *a, const orbit_model_node_t *b)
{
    if (a->companion != b->companion) return a->companion;
    if (a->powered != b->powered) return a->powered;
    bool a_battery = a->battery_known && a->battery <= 100;
    bool b_battery = b->battery_known && b->battery <= 100;
    if (a_battery != b_battery) return a_battery;
    if (a_battery && a->battery != b->battery) return a->battery > b->battery;
    int name_order = orbit_model_name_compare(a, b);
    if (name_order != 0) return name_order < 0;
    return memcmp(a->mac, b->mac, ORBIT_MODEL_MAC_SIZE) < 0;
}

/* Lexical name/MAC ties choose the smaller byte sequence. */
static inline bool orbit_model_coordinator(const orbit_model_node_t *nodes, size_t count,
                                           uint8_t coordinator_mac[ORBIT_MODEL_MAC_SIZE])
{
    if (nodes == NULL || coordinator_mac == NULL) return false;
    size_t best = count;
    for (size_t i = 0; i < count; ++i)
    {
        if (!nodes[i].awake || nodes[i].name_len > ORBIT_MODEL_KEY_MAX ||
            !orbit_model_author_valid(nodes[i].mac)) continue;
        if (best == count || orbit_model_node_better(&nodes[i], &nodes[best])) best = i;
    }
    if (best == count) return false;
    memcpy(coordinator_mac, nodes[best].mac, ORBIT_MODEL_MAC_SIZE);
    return true;
}

#endif
