/* Actual production codec, executed in an isolated RISC-V emulator. */
#include "ecos_wire.h"
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
    return 0;
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile("call ecos_tests\n ebreak\n");
}
