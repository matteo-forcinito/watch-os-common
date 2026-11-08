#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../orbit_model.h"

static const uint8_t mac_a[6] = {0x10, 0, 0, 0, 0, 1};
static const uint8_t mac_b[6] = {0x10, 0, 0, 0, 0, 2};
static const uint8_t key[] = {'k'};

static orbit_model_entry_t make_entry(uint64_t clock, const uint8_t *author,
                                      const uint8_t *value, size_t value_len, bool tombstone)
{
    orbit_model_entry_t e = {0};
    e.used = true;
    e.tombstone = tombstone;
    e.key_len = sizeof(key);
    e.value_len = (uint8_t)value_len;
    e.clock = clock;
    memcpy(e.author, author, 6);
    memcpy(e.key, key, sizeof(key));
    if (value_len) memcpy(e.value, value, value_len);
    return e;
}

static void test_convergence_and_tombstones(void)
{
    static const uint8_t va[] = {'A'}, vb[] = {'B'};
    orbit_model_entry_t updates[] = {
        make_entry(7, mac_a, va, sizeof(va), false),
        make_entry(7, mac_b, vb, sizeof(vb), false),
        make_entry(8, mac_a, NULL, 0, true),
    };
    const size_t permutations[][3] = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}, {1, 0, 2}, {0, 2, 1}, {2, 1, 0}};
    orbit_model_entry_t reference = {0};
    for (size_t p = 0; p < sizeof(permutations) / sizeof(permutations[0]); ++p)
    {
        orbit_model_entry_t entries[ORBIT_MODEL_CAPACITY];
        orbit_model_store_t store;
        assert(orbit_model_store_init(&store, entries, ORBIT_MODEL_CAPACITY));
        for (size_t i = 0; i < 3; ++i) assert(orbit_model_merge(&store, &updates[permutations[p][i]]));
        const orbit_model_entry_t *winner = orbit_model_get(&store, key, sizeof(key));
        assert(winner && winner->tombstone && winner->clock == 8);
        assert(store.count == 1 && store.observed_clock == 8);
        if (p == 0) reference = *winner;
        else assert(orbit_model_same_version_payload(&reference, winner));
    }

    orbit_model_entry_t a_first[ORBIT_MODEL_CAPACITY], b_first[ORBIT_MODEL_CAPACITY];
    orbit_model_store_t sa, sb;
    assert(orbit_model_store_init(&sa, a_first, ORBIT_MODEL_CAPACITY));
    assert(orbit_model_store_init(&sb, b_first, ORBIT_MODEL_CAPACITY));
    assert(orbit_model_merge(&sa, &updates[0]) && orbit_model_merge(&sa, &updates[1]));
    assert(orbit_model_merge(&sb, &updates[1]) && orbit_model_merge(&sb, &updates[0]));
    const orbit_model_entry_t *wa = orbit_model_get(&sa, key, sizeof(key));
    const orbit_model_entry_t *wb = orbit_model_get(&sb, key, sizeof(key));
    assert(wa && wb && wa->clock == 7 && wb->clock == 7);
    assert(memcmp(wa->author, mac_b, 6) == 0 && memcmp(wb->author, mac_b, 6) == 0);
    assert(wa->value_len == 1 && wa->value[0] == 'B');
}

static void test_validation_capacity_and_clock(void)
{
    orbit_model_entry_t entries[1];
    orbit_model_store_t store;
    assert(orbit_model_store_init(&store, entries, 1));
    static const uint8_t value[] = {'v'};
    orbit_model_entry_t first = make_entry(3, mac_a, value, 1, false);
    orbit_model_entry_t second = make_entry(9, mac_b, value, 1, false);
    second.key[0] = 'x';
    assert(orbit_model_merge(&store, &first));
    assert(!orbit_model_merge(&store, &second));
    assert(store.count == 1 && store.observed_clock == 9);
    uint64_t next = 0;
    assert(orbit_model_next_clock(&store, 4, &next) && next == 10);
    assert(!orbit_model_put(&store, key, sizeof(key), value, 1, 9, mac_a));
    assert(orbit_model_put(&store, key, sizeof(key), value, 1, 10, mac_a));
    assert(store.count == 1 && store.observed_clock == 10);
    assert(orbit_model_delete(&store, key, sizeof(key), 11, mac_a));
    assert(store.count == 1);
    const orbit_model_entry_t *tombstone = orbit_model_get(&store, key, sizeof(key));
    assert(tombstone && tombstone->tombstone && tombstone->value_len == 0);

    orbit_model_entry_t invalid = make_entry(12, mac_a, value, 1, false);
    invalid.clock = 0; assert(!orbit_model_merge(&store, &invalid));
    invalid = make_entry(12, mac_a, value, 1, false);
    memset(invalid.author, 0, sizeof(invalid.author)); assert(!orbit_model_merge(&store, &invalid));
    invalid = make_entry(12, mac_a, value, 1, false); invalid.key_len = 0;
    assert(!orbit_model_merge(&store, &invalid));
    invalid = make_entry(12, mac_a, value, 1, false); invalid.key_len = ORBIT_MODEL_KEY_MAX + 1;
    assert(!orbit_model_merge(&store, &invalid));
    invalid = make_entry(12, mac_a, value, 1, false); invalid.value_len = ORBIT_MODEL_VALUE_MAX + 1;
    assert(!orbit_model_merge(&store, &invalid));
    invalid = make_entry(12, mac_a, value, 1, true);
    assert(!orbit_model_merge(&store, &invalid));

    orbit_model_entry_t max_entry = make_entry(UINT64_MAX, mac_b, value, 1, false);
    assert(orbit_model_merge(&store, &max_entry));
    assert(!orbit_model_next_clock(&store, UINT64_MAX - 1, &next));
    assert(!orbit_model_put(&store, key, sizeof(key), value, 1, UINT64_MAX, mac_a));
}

static void test_codec(void)
{
    uint8_t max_key[ORBIT_MODEL_KEY_MAX], max_value[ORBIT_MODEL_VALUE_MAX];
    memset(max_key, 'K', sizeof(max_key));
    memset(max_value, 'V', sizeof(max_value));
    orbit_model_entry_t original = {0};
    original.used = true;
    original.key_len = sizeof(max_key);
    original.value_len = sizeof(max_value);
    original.clock = UINT64_C(0x0102030405060708);
    memcpy(original.author, mac_b, 6);
    memcpy(original.key, max_key, sizeof(max_key));
    memcpy(original.value, max_value, sizeof(max_value));
    uint8_t wire[ORBIT_MODEL_WIRE_MAX];
    size_t length = orbit_model_encode(&original, wire, sizeof(wire));
    assert(length == ORBIT_MODEL_WIRE_HEADER + sizeof(max_key) + sizeof(max_value));
    assert(length == 131 && length <= ORBIT_MODEL_WIRE_MAX);
    assert(orbit_model_encode(&original, wire, length - 1) == 0);
    orbit_model_entry_t decoded = {0};
    assert(orbit_model_decode(wire, length, &decoded));
    assert(orbit_model_same_version_payload(&original, &decoded));
    assert(original.clock == decoded.clock && memcmp(original.author, decoded.author, 6) == 0);

    uint8_t bad[ORBIT_MODEL_WIRE_MAX];
    memcpy(bad, wire, length); bad[0]++;
    assert(!orbit_model_decode(bad, length, &decoded));
    memcpy(bad, wire, length); bad[1] = 99;
    assert(!orbit_model_decode(bad, length, &decoded));
    memcpy(bad, wire, length); bad[4] = 1;
    assert(!orbit_model_decode(bad, length, &decoded));
    memcpy(bad, wire, length); bad[2] = ORBIT_MODEL_KEY_MAX + 1;
    assert(!orbit_model_decode(bad, length, &decoded));
    memcpy(bad, wire, length); bad[3] = ORBIT_MODEL_VALUE_MAX + 1;
    assert(!orbit_model_decode(bad, length, &decoded));
    assert(!orbit_model_decode(wire, length - 1, &decoded));
    memcpy(bad, wire, length); bad[5] = 0; bad[6] = 0; bad[7] = 0; bad[8] = 0;
    bad[9] = 0; bad[10] = 0; bad[11] = 0; bad[12] = 0;
    assert(!orbit_model_decode(bad, length, &decoded));
    memcpy(bad, wire, length); memset(bad + 13, 0, 6);
    assert(!orbit_model_decode(bad, length, &decoded));

    orbit_model_entry_t deletion = make_entry(4, mac_a, NULL, 0, true);
    length = orbit_model_encode(&deletion, wire, sizeof(wire));
    assert(length == ORBIT_MODEL_WIRE_HEADER + 1);
    assert(orbit_model_decode(wire, length, &decoded) && decoded.tombstone);
    wire[1] = ORBIT_MODEL_OP_DELETE; wire[3] = 1;
    assert(!orbit_model_decode(wire, length, &decoded));
    wire[3] = 0;
    assert(!orbit_model_decode(wire, length + 1, &decoded));
}

static orbit_model_node_t node(uint8_t id, const char *name, bool awake, bool companion,
                               bool powered, bool battery_known, uint8_t battery)
{
    orbit_model_node_t n = {0};
    n.awake = awake; n.companion = companion; n.powered = powered;
    n.battery_known = battery_known; n.battery = battery;
    n.name_len = (uint8_t)strlen(name); memcpy(n.name, name, n.name_len);
    n.mac[5] = id;
    return n;
}

static void test_coordinator(void)
{
    orbit_model_node_t nodes[] = {
        node(4, "alpha", true, false, false, true, 100),
        node(3, "zeta", true, true, false, false, 0),
        node(2, "awake", false, true, true, true, 100),
        node(1, "powered", true, false, true, false, 0),
    };
    uint8_t selected[6];
    assert(orbit_model_coordinator(nodes, 4, selected) && selected[5] == 3);
    nodes[1].awake = false;
    assert(orbit_model_coordinator(nodes, 4, selected) && selected[5] == 1);
    nodes[3].powered = false;
    assert(orbit_model_coordinator(nodes, 4, selected) && selected[5] == 4);

    orbit_model_node_t ties[] = {
        node(2, "same", true, false, false, true, 50),
        node(1, "same", true, false, false, true, 50),
        node(3, "same", true, false, false, true, 49),
    };
    assert(orbit_model_coordinator(ties, 3, selected) && selected[5] == 1);
    orbit_model_node_t asleep[] = {node(1, "off", false, false, false, false, 0)};
    assert(!orbit_model_coordinator(asleep, 1, selected));
    orbit_model_node_t invalid_mac[] = {node(0, "invalid", true, true, true, true, 100)};
    assert(!orbit_model_coordinator(invalid_mac, 1, selected));
    orbit_model_node_t unknown[] = {
        node(1, "unknown", true, false, false, false, 0),
        node(2, "known", true, false, false, true, 0),
    };
    assert(orbit_model_coordinator(unknown, 2, selected) && selected[5] == 2);
}

static void test_full_family_routes_leave_house_capacity(void)
{
    orbit_model_entry_t entries[ORBIT_MODEL_CAPACITY];
    orbit_model_store_t store;
    assert(orbit_model_store_init(&store, entries, ORBIT_MODEL_CAPACITY));
    const uint8_t value[] = {1};
    uint64_t clock = 0;
    for (unsigned node = 0; node < 7; ++node)
    {
        for (unsigned type = 0; type < 4; ++type)
        {
            char route[48];
            int length = snprintf(route, sizeof(route), "ecos/route/0200000000%02x/%u", node, type);
            assert(orbit_model_put(&store, (const uint8_t *)route, (size_t)length,
                                   value, sizeof(value), ++clock, mac_a));
        }
    }
    const uint8_t channel[] = "ecos/channel";
    assert(orbit_model_put(&store, channel, sizeof(channel) - 1, value, sizeof(value), ++clock, mac_a));
    assert(store.count == 29 && store.capacity - store.count >= 16);
}

int main(void)
{
    test_convergence_and_tombstones();
    test_validation_capacity_and_clock();
    test_codec();
    test_coordinator();
    test_full_family_routes_leave_house_capacity();
    puts("ORBIT model tests passed");
    return 0;
}
