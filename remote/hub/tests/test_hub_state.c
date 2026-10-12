#include "hub_state.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static hub_state_t s_state;
static uint8_t s_packet[HUB_PACKET_SIZE];
static hub_report_t s_report;

static void packet(uint8_t id, uint32_t sequence, uint64_t session)
{
    for (unsigned i = 0; i < sizeof(s_packet); i++)
    {
        s_packet[i] = 0;
    }
    s_packet[0] = 'R';
    s_packet[1] = 'H';
    s_packet[2] = 1;
    s_packet[3] = id;
    for (unsigned i = 0; i < 4; i++)
    {
        s_packet[4 + i] = (uint8_t)(sequence >> (i * 8));
    }
    for (unsigned i = 0; i < 8; i++)
    {
        s_packet[8 + i] = (uint8_t)(session >> (i * 8));
    }
}

static hub_result_t receive(unsigned node, uint32_t now)
{
    return hub_state_receive(&s_state, node, s_packet, sizeof(s_packet), now);
}

int hub_tests(void)
{
    hub_state_init(&s_state);
    CHECK(sizeof(hub_state_t) < 512);
    CHECK(!hub_state_authorize(&s_state, HUB_NODES, 1));
    CHECK(!hub_state_authorize(&s_state, 0, 0));
    packet(1, 0, 11);
    CHECK(receive(0, 0) == HUB_UNAUTHORIZED);
    CHECK(hub_state_authorize(&s_state, 0, 11));
    CHECK(hub_state_authorize(&s_state, 1, 22));

    /* Union of modifiers and keys; another peer's release is not global. */
    s_packet[16] = 2;
    s_packet[18] = 4;
    CHECK(receive(0, 100) == HUB_ACCEPTED);
    packet(1, 1, 22);
    s_packet[16] = 1;
    s_packet[18] = 4;
    s_packet[19] = 5;
    CHECK(receive(1, 200) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 1, &s_report));
    CHECK(s_report.data[0] == 3 && s_report.data[2] == 4 && s_report.data[3] == 5 && s_report.data[4] == 0);
    CHECK(hub_state_commit(&s_state, &s_report));
    CHECK(!hub_state_commit(&s_state, &s_report));
    hub_state_revoke(&s_state, 1);
    CHECK(hub_state_snapshot(&s_state, 1, &s_report));
    CHECK(s_report.data[0] == 2 && s_report.data[2] == 4 && s_report.data[3] == 0);
    CHECK(receive(1, 210) == HUB_UNAUTHORIZED);

    /* Expired sessions cannot be revived by a queued packet. */
    hub_state_expire(&s_state, 600);
    CHECK(hub_state_snapshot(&s_state, 1, &s_report));
    CHECK(s_report.data[0] == 0 && s_report.data[2] == 0);
    packet(1, 2, 11);
    CHECK(receive(0, 601) == HUB_UNAUTHORIZED);
    CHECK(hub_state_authorize(&s_state, 0, 33));
    CHECK(receive(0, 602) == HUB_UNAUTHORIZED);

    /* Sequence wraps; duplicate/backward packets cannot extend liveness. */
    packet(1, UINT32_MAX - 1, 33);
    CHECK(receive(0, UINT32_MAX - 100) == HUB_ACCEPTED);
    packet(1, 0, 33);
    CHECK(receive(0, UINT32_MAX - 90) == HUB_ACCEPTED);
    CHECK(receive(0, 100) == HUB_DUPLICATE);
    packet(1, UINT32_MAX, 33);
    CHECK(receive(0, 101) == HUB_DUPLICATE);
    hub_state_expire(&s_state, 408);
    CHECK(s_state.nodes[0].authorized);
    hub_state_expire(&s_state, 409);
    CHECK(!s_state.nodes[0].authorized);

    /* Malformed inputs rejected before touching the sequence or liveness. */
    CHECK(hub_state_authorize(&s_state, 0, 44));
    packet(1, 1, 44);
    s_packet[17] = 1;
    CHECK(receive(0, 1) == HUB_INVALID);
    s_packet[17] = 0;
    s_packet[18] = 1;
    CHECK(receive(0, 1) == HUB_INVALID);
    s_packet[18] = 0x66;
    CHECK(receive(0, 1) == HUB_INVALID);
    s_packet[18] = 4;
    CHECK(hub_state_receive(&s_state, 0, s_packet, 23, 1) == HUB_INVALID);
    CHECK(hub_state_receive(&s_state, 0, NULL, 24, 1) == HUB_INVALID);
    CHECK(receive(HUB_NODES, 1) == HUB_INVALID);
    CHECK(receive(0, 1) == HUB_ACCEPTED);
    packet(2, 2, 44);
    s_packet[17] = 4;
    CHECK(receive(0, 2) == HUB_INVALID);
    s_packet[17] = 0;
    s_packet[18] = 1;
    CHECK(receive(0, 2) == HUB_INVALID);
    packet(3, 2, 44);
    s_packet[16] = 32;
    CHECK(receive(0, 2) == HUB_INVALID);
    s_packet[16] = 0;
    s_packet[17] = 128;
    CHECK(receive(0, 2) == HUB_INVALID);

    /* Seven distinct keys produce HID ErrorRollOver; six restore normally. */
    CHECK(hub_state_authorize(&s_state, 1, 55));
    packet(1, 3, 44);
    for (unsigned i = 0; i < 6; i++)
    {
        s_packet[18 + i] = (uint8_t)(4 + i);
    }
    CHECK(receive(0, 10) == HUB_ACCEPTED);
    packet(1, 1, 55);
    s_packet[18] = 10;
    CHECK(receive(1, 10) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 1, &s_report));
    for (unsigned i = 2; i < 8; i++)
    {
        CHECK(s_report.data[i] == 1);
    }
    hub_state_revoke(&s_state, 1);
    CHECK(!hub_state_commit(&s_state, &s_report)); /* Stale snapshot. */
    CHECK(hub_state_snapshot(&s_state, 1, &s_report));
    CHECK(s_report.data[2] == 4 && s_report.data[7] == 9);

    /* Relative motion survives failed sends and drains exactly once. */
    packet(3, 4, 44);
    s_packet[16] = 1;
    s_packet[17] = 127;
    s_packet[19] = (uint8_t)-100;
    s_packet[20] = 12;
    CHECK(receive(0, 20) == HUB_ACCEPTED);
    CHECK(receive(0, 21) == HUB_DUPLICATE);
    packet(3, 5, 44);
    s_packet[16] = 1;
    s_packet[17] = 127;
    CHECK(receive(0, 21) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 3, &s_report));
    CHECK(s_report.data[0] == 1 && s_report.data[1] == 127 && (int8_t)s_report.data[3] == -100 && s_report.data[4] == 12);
    CHECK(s_state.nodes[0].motion[0] == 254);
    CHECK(hub_state_commit(&s_state, &s_report));
    CHECK(!hub_state_commit(&s_state, &s_report));
    CHECK(hub_state_snapshot(&s_state, 3, &s_report));
    CHECK(s_report.data[1] == 127 && s_report.data[3] == 0 && s_report.data[4] == 0);
    CHECK(hub_state_commit(&s_state, &s_report));
    CHECK(!hub_state_snapshot(&s_state, 3, &s_report));

    /* Revocation preserves other peers' pending motion and mouse buttons. */
    CHECK(hub_state_authorize(&s_state, 1, 66));
    packet(3, 6, 44);
    s_packet[16] = 1;
    s_packet[17] = 70;
    CHECK(receive(0, 30) == HUB_ACCEPTED);
    packet(3, 1, 66);
    s_packet[16] = 2;
    s_packet[17] = 80;
    CHECK(receive(1, 30) == HUB_ACCEPTED);
    hub_state_revoke(&s_state, 0);
    CHECK(hub_state_snapshot(&s_state, 3, &s_report));
    CHECK(s_report.data[0] == 2 && s_report.data[1] == 80);
    CHECK(hub_state_commit(&s_state, &s_report));

    /* Opposite movement cancels; no endless zero-report drain. */
    CHECK(hub_state_authorize(&s_state, 0, 77));
    packet(3, 1, 77);
    s_packet[17] = (uint8_t)-80;
    CHECK(receive(0, 40) == HUB_ACCEPTED);
    packet(3, 2, 66);
    s_packet[17] = 80;
    CHECK(receive(1, 40) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 3, &s_report));
    CHECK(s_report.data[1] == 0);
    CHECK(hub_state_commit(&s_state, &s_report));
    CHECK(!hub_state_snapshot(&s_state, 3, &s_report));

    /* Bounded backlog and observable saturation. */
    for (uint32_t sequence = 3; sequence < 400; sequence++)
    {
        packet(3, sequence, 66);
        s_packet[17] = 127;
        CHECK(receive(1, 50) == HUB_ACCEPTED);
    }
    CHECK(s_state.motion_overflows > 0 && s_state.nodes[1].motion[0] == 32767);
    unsigned drained = 0;
    while (hub_state_snapshot(&s_state, 3, &s_report))
    {
        drained += s_report.data[1];
        CHECK(hub_state_commit(&s_state, &s_report));
        CHECK(drained <= 32767);
    }
    CHECK(drained == 32767);

    /* One media usage and one gamepad: deterministic active-slot ownership. */
    packet(2, 2, 77);
    s_packet[16] = 0xcd;
    CHECK(receive(0, 60) == HUB_ACCEPTED);
    packet(2, 400, 66);
    s_packet[16] = 0xe9;
    CHECK(receive(1, 60) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 2, &s_report));
    CHECK(s_report.data[0] == 0xcd);
    packet(4, 3, 77);
    s_packet[16] = 30;
    s_packet[18] = 1;
    CHECK(receive(0, 70) == HUB_ACCEPTED);
    packet(4, 401, 66);
    s_packet[16] = 60;
    CHECK(receive(1, 70) == HUB_ACCEPTED);
    CHECK(hub_state_snapshot(&s_state, 4, &s_report));
    CHECK(s_report.data[0] == 30 && s_report.data[2] == 1);
    hub_state_revoke(&s_state, 0);
    CHECK(hub_state_snapshot(&s_state, 2, &s_report));
    CHECK(s_report.data[0] == 0xe9);
    CHECK(hub_state_snapshot(&s_state, 4, &s_report));
    CHECK(s_report.data[0] == 60 && s_report.data[2] == 0);

    hub_state_disconnect(&s_state);
    for (uint8_t id = 1; id <= 4; id++)
    {
        CHECK(hub_state_snapshot(&s_state, id, &s_report));
        for (unsigned i = 0; i < 8; i++)
        {
            CHECK(s_report.data[i] == 0);
        }
    }
    CHECK(receive(1, 80) == HUB_UNAUTHORIZED);
    /* Property check: mixed motion from five nodes drains to its exact sum. */
    uint32_t random = 1234567;
    for (unsigned round = 0; round < 256; round++)
    {
        hub_state_init(&s_state);
        int32_t expected[4] = {0};
        for (unsigned node = 0; node < HUB_NODES; node++)
        {
            CHECK(hub_state_authorize(&s_state, node, node + 1));
            for (unsigned sequence = 1; sequence <= 8; sequence++)
            {
                packet(3, sequence, node + 1);
                for (unsigned axis = 0; axis < 4; axis++)
                {
                    random = random * 1664525u + 1013904223u;
                    int8_t delta = (int8_t)((random >> 16) % 255 - 127);
                    s_packet[17 + axis] = (uint8_t)delta;
                    expected[axis] += delta;
                }
                CHECK(receive(node, 100) == HUB_ACCEPTED);
                CHECK(receive(node, 101) == HUB_DUPLICATE);
            }
        }
        int32_t actual[4] = {0};
        unsigned chunks = 0;
        while (hub_state_snapshot(&s_state, 3, &s_report))
        {
            for (unsigned axis = 0; axis < 4; axis++)
            {
                actual[axis] += (int8_t)s_report.data[1 + axis];
            }
            CHECK(hub_state_commit(&s_state, &s_report));
            CHECK(++chunks <= 40);
        }
        for (unsigned axis = 0; axis < 4; axis++)
        {
            CHECK(actual[axis] == expected[axis]);
        }
    }
    return 0;
}

/* Minimal emulator entry: return value in a0, then a breakpoint. */
#ifdef HUB_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("call hub_tests\n ebreak\n");
}
#else
int main(void)
{
    return hub_tests();
}
#endif
