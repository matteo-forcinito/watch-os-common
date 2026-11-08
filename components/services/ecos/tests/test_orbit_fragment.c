#include "../ecos_orbit_fragment.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ecos_orbit_fragment_t state = {0};
    uint8_t input[ECOS_ORBIT_LARGE_MAX], packet[ECOS_ORBIT_DATA_MAX], previous[ECOS_ORBIT_DATA_MAX];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i ^ 0xa5);
    size_t previous_length = 0;
    for (size_t offset = 0; offset < sizeof(input);)
    {
        size_t n = ecos_orbit_fragment_pack(packet, sizeof(packet), 42, input, sizeof(input), offset);
        assert(n > ECOS_ORBIT_FRAGMENT_HEADER && n <= sizeof(packet));
        size_t next = offset + n - ECOS_ORBIT_FRAGMENT_HEADER;
        ecos_orbit_fragment_result_t result = ecos_orbit_fragment_accept(&state, packet, n);
        assert(result == (next == sizeof(input) ? ECOS_ORBIT_FRAGMENT_READY : ECOS_ORBIT_FRAGMENT_MORE));
        /* Delivery can repeat, but READY is emitted exactly once. */
        result = ecos_orbit_fragment_accept(&state, packet, n);
        assert(result == (next == sizeof(input) ? ECOS_ORBIT_FRAGMENT_REPLIED : ECOS_ORBIT_FRAGMENT_MORE));
        if (previous_length)
            assert(ecos_orbit_fragment_accept(&state, previous, previous_length) == ECOS_ORBIT_FRAGMENT_MORE);
        memcpy(previous, packet, n);
        previous_length = n;
        offset = next;
    }
    assert(state.complete && state.used == sizeof(input) && memcmp(state.data, input, sizeof(input)) == 0);
    packet[ECOS_ORBIT_FRAGMENT_HEADER] ^= 1;
    assert(ecos_orbit_fragment_accept(&state, packet, previous_length) == ECOS_ORBIT_FRAGMENT_INVALID);
    packet[ECOS_ORBIT_FRAGMENT_HEADER] ^= 1;
    /* A new transfer cannot start in the middle of the old one. */
    ecos_orbit_put_request_id(packet + 4, 43);
    assert(ecos_orbit_fragment_accept(&state, packet, previous_length) == ECOS_ORBIT_FRAGMENT_INVALID);
    size_t n = ecos_orbit_fragment_pack(packet, sizeof(packet), 43, input, 1, 0);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_READY);
    assert(state.total == 1 && state.used == 1 && state.reply_length == 0);
    packet[16] = 2;
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID);
    packet[16] = 0;
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID);
    assert(ecos_orbit_fragment_pack(packet, sizeof(packet), 0, input, 1, 0) == 0);
    assert(ecos_orbit_fragment_pack(packet, sizeof(packet), 1, input, sizeof(input) + 1, 0) == 0);
    assert(ecos_orbit_fragment_pack(packet, 17, 1, input, 1, 0) == 0);
    assert(ecos_orbit_fragment_ack(packet, sizeof(packet), 127) == ECOS_ORBIT_FRAGMENT_ACK);
    assert(ecos_orbit_fragment_ack_matches(packet, ECOS_ORBIT_FRAGMENT_ACK, 127));
    assert(!ecos_orbit_fragment_ack_matches(packet, ECOS_ORBIT_FRAGMENT_ACK, 128));
    assert(!ecos_orbit_fragment_ack_matches(packet, ECOS_ORBIT_FRAGMENT_ACK - 1, 127));
    memset(&state, 0, sizeof(state));
    n = ecos_orbit_fragment_pack(packet, sizeof(packet), 99, input, sizeof(input), 127);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID && state.id == 0);
    n = ecos_orbit_fragment_pack(packet, sizeof(packet), 99, input, sizeof(input), 0);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_MORE && state.used == 127);
    n = ecos_orbit_fragment_pack(packet, sizeof(packet), 99, input, sizeof(input), 128);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID && state.used == 127);
    n = ecos_orbit_fragment_pack(packet, sizeof(packet), 99, input, sizeof(input), 126);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID && state.used == 127);
    n = ecos_orbit_fragment_pack(packet, sizeof(packet), 99, input, sizeof(input), 0);
    packet[ECOS_ORBIT_FRAGMENT_HEADER] ^= 1;
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID && state.used == 127);
    packet[ECOS_ORBIT_FRAGMENT_HEADER] ^= 1;
    ecos_orbit_fragment_put16(packet + 12, ECOS_ORBIT_LARGE_MAX + 1);
    assert(ecos_orbit_fragment_accept(&state, packet, n) == ECOS_ORBIT_FRAGMENT_INVALID && state.used == 127);
    puts("Orbit fragmentation tests passed");
    return 0;
}
