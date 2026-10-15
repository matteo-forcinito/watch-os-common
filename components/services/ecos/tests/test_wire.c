/* Actual production codec, executed in an isolated RISC-V emulator. */
#include "ecos_wire.h"
#include "ecos_link_wire.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

void *memset(void *to, int value, size_t size)
{
    uint8_t *p = to;
    for (size_t i = 0; i < size; i++) p[i] = (uint8_t)value;
    return to;
}

void *memcpy(void *restrict to, const void *restrict from, size_t size)
{
    uint8_t *a = to;
    const uint8_t *b = from;
    for (size_t i = 0; i < size; i++) a[i] = b[i];
    return to;
}

int memcmp(const void *a, const void *b, size_t size)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < size; i++)
    {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

char *strncpy(char *restrict to, const char *restrict from, size_t size)
{
    size_t i = 0;
    for (; i < size && from[i] != 0; i++)
    {
        to[i] = from[i];
    }
    for (; i < size; i++)
    {
        to[i] = 0;
    }
    return to;
}

int ecos_tests(void)
{
    uint8_t a[6] = {2, 0, 0, 0, 0, 1}, b[6] = {2, 0, 0, 0, 0, 2};
    uint8_t frame[ECOS_FRAME_MAX + 1] = {0};
    for (uint8_t type = 0; type < 10; type++)
    {
        ecos_wire_header(frame, type, a, b);
        for (size_t size = 0; size <= sizeof(frame); size++)
        {
            bool expected = (type == ECOS_HELLO && size == ECOS_FRAME_MAX) ||
                            ((type == ECOS_CONFIRM || type == ECOS_SAVED || type == ECOS_COMMIT) && size == ECOS_HEADER + 32);
            CHECK(ecos_wire_valid(frame, size) == expected);
        }
    }
    ecos_wire_header(frame, ECOS_HELLO, a, b);
    CHECK(memcmp(frame + 5, a, 6) == 0);
    CHECK(memcmp(frame + 11, b, 6) == 0);
    for (unsigned i = 0; i < 4; i++)
    {
        frame[i] ^= 1;
        CHECK(!ecos_wire_valid(frame, ECOS_FRAME_MAX));
        frame[i] ^= 1;
    }
    uint8_t hello_a[ECOS_HELLO_SIZE], hello_b[ECOS_HELLO_SIZE];
    for (unsigned i = 0; i < ECOS_HELLO_SIZE; i++)
    {
        hello_a[i] = (uint8_t)i;
        hello_b[i] = (uint8_t)(i + 1);
    }
    uint8_t t1[ECOS_TRANSCRIPT_SIZE], t2[ECOS_TRANSCRIPT_SIZE];
    ecos_wire_transcript(t1, a, b, hello_a, hello_b);
    ecos_wire_transcript(t2, b, a, hello_b, hello_a);
    CHECK(ecos_wire_equal(t1, t2, sizeof(t1)));
    CHECK(memcmp(t1, "EC-PAIR1", 8) == 0);
    /* Every identity, nonce, public-key and displayed-name byte is bound. */
    for (unsigned i = 0; i < ECOS_HELLO_SIZE; i++)
    {
        hello_b[i] ^= 1;
        ecos_wire_transcript(t2, a, b, hello_a, hello_b);
        CHECK(!ecos_wire_equal(t1, t2, sizeof(t1)));
        hello_b[i] ^= 1;
    }
    for (unsigned i = 0; i < 6; i++)
    {
        b[i] ^= 4;
        ecos_wire_transcript(t2, a, b, hello_a, hello_b);
        CHECK(!ecos_wire_equal(t1, t2, sizeof(t1)));
        b[i] ^= 4;
    }
    for (unsigned i = 0; i < sizeof(t1); i++)
    {
        memcpy(t2, t1, sizeof(t1));
        t2[i] ^= 1;
        CHECK(!ecos_wire_equal(t1, t2, sizeof(t1)));
    }
    uint8_t link[ECOS_LINK_FRAME_MAX + 1] = {0};
    uint64_t epoch = UINT64_C(0x1122334455667788);
    ecos_link_header(link, ECOS_LINK_RING, a, b, epoch, UINT32_C(0x99aabbcc));
    CHECK(ecos_link_get64(link + 17) == epoch);
    CHECK(ecos_link_get32(link + 25) == UINT32_C(0x99aabbcc));
    CHECK(link[17] == 0x11 && link[28] == 0xcc);
    for (unsigned type = 0; type <= 5; type++)
    {
        link[4] = (uint8_t)type;
        for (size_t n = 0; n <= sizeof(link); n++)
        {
            bool valid = type >= 1 && type <= 4 && n >= ECOS_LINK_HEADER + ECOS_LINK_TIME_SIZE + ECOS_LINK_TAG &&
                         n <= ECOS_LINK_FRAME_MAX;
            CHECK(ecos_link_valid(link, n) == valid);
        }
    }
    link[4] = ECOS_LINK_RING;
    link[3] = '1';
    CHECK(!ecos_link_valid(link, ECOS_LINK_FRAME_MAX));
    CHECK(!ecos_link_newer(0, 1, 0, 0));
    CHECK(!ecos_link_newer(1, 0, 0, 0));
    CHECK(ecos_link_newer(1, 1, 0, 0));
    CHECK(!ecos_link_newer(10, 9, 10, 9));
    CHECK(!ecos_link_newer(10, 8, 10, 9));
    CHECK(ecos_link_newer(10, 10, 10, 9));
    CHECK(!ecos_link_newer(9, UINT32_MAX, 10, 9));
    CHECK(ecos_link_newer(11, 1, 10, UINT32_MAX));
    CHECK(!ecos_link_newer(1, 1, UINT64_MAX, UINT32_MAX));
    CHECK(!ecos_link_newer(10, 1, 10, UINT32_MAX));
    ecos_link_status_t status = {.name = "Cilipu", .product = "AMOLED", .version = "test", .battery = 73,
                                .flags = ECOS_LINK_F_USB, .ip = {192, 168, 1, 9}, .ssid = "home",
                                .uptime_min = UINT32_C(0x12345678), .ram_kb = 123};
    ecos_link_put_status(link, &status);
    ecos_link_status_t decoded;
    ecos_link_get_status(link, &decoded);
    CHECK(memcmp(decoded.name, status.name, 7) == 0);
    CHECK(decoded.battery == 73 && decoded.flags == ECOS_LINK_F_USB && decoded.ram_kb == 123);
    CHECK(decoded.uptime_min == UINT32_C(0x12345678) && decoded.ip[3] == 9);
    link[0] = '\n';
    link[72] = 101;
    ecos_link_get_status(link, &decoded);
    CHECK(decoded.name[0] == ' ' && decoded.battery == -1);
    return 0;
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile("call ecos_tests\n ebreak\n");
}
