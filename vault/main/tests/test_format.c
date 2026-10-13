#include "format.h"
#include <string.h>
#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
            return __LINE__;                                                                       \
    } while (0)
#ifdef SENTINEL_EMULATOR
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
void *memcpy(void *to, const void *from, size_t n)
{
    unsigned char *a = to;
    const unsigned char *b = from;
    for (size_t i = 0; i < n; i++)
        a[i] = b[i];
    return to;
}
void *memset(void *to, int value, size_t n)
{
    unsigned char *a = to;
    for (size_t i = 0; i < n; i++)
        a[i] = (unsigned char)value;
    return to;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}
#endif
int test_packet(void)
{
    vault_header_t header = {
        .iterations = 600000, .generation = UINT64_C(0x1020304050607080), .payload_size = 32};
    vault_header_t decoded;
    uint8_t file[112] = {0}, modified[112];
    for (unsigned i = 0; i < 16; i++)
        header.salt[i] = i;
    for (unsigned i = 0; i < 12; i++)
        header.nonce[i] = 16 + i;
    CHECK(vault_header_encode(file, &header));
    CHECK(vault_header_decode(file, sizeof(file), &decoded));
    CHECK(decoded.iterations == header.iterations && decoded.generation == header.generation &&
          decoded.payload_size == 32);
    CHECK(vault_equal(decoded.salt, header.salt, 16));
    CHECK(vault_equal(decoded.nonce, header.nonce, 12));
    for (unsigned i = 0; i < 16; i++)
    {
        memcpy(modified, file, 112);
        modified[i] ^= 1;
        CHECK(!vault_header_decode(modified, 112, &decoded));
    }
    for (unsigned i = 60; i < 64; i++)
    {
        memcpy(modified, file, 112);
        modified[i] = 1;
        CHECK(!vault_header_decode(modified, 112, &decoded));
    }
    CHECK(!vault_header_decode(file, 111, &decoded));
    CHECK(!vault_header_decode(file, 0, &decoded));
    CHECK(!vault_header_decode(NULL, 112, &decoded));
    CHECK(!vault_header_decode(file, 112, NULL));
    header.iterations = 599999;
    CHECK(!vault_header_encode(file, &header));
    header.iterations = 2000001;
    CHECK(!vault_header_encode(file, &header));
    header.iterations = 600000;
    header.payload_size = VAULT_MAX_PAYLOAD + 1;
    CHECK(!vault_header_encode(file, &header));
    header.payload_size = 0;
    CHECK(!vault_header_encode(file, &header));
    uint8_t base[64];
    size_t length;
    CHECK(vault_base32_decode("MZXW6YTBOI======", base, 64, &length));
    CHECK(length == 6 && memcmp(base, "foobar", 6) == 0);
    CHECK(vault_base32_decode("mzxw6ytboi", base, 64, &length));
    CHECK(length == 6);
    CHECK(vault_base32_decode("MY", base, 64, &length));
    CHECK(length == 1 && base[0] == 'f');
    CHECK(!vault_base32_decode("MZ", base, 64, &length));
    CHECK(length == 0);
    CHECK(!vault_base32_decode("M", base, 64, &length));
    CHECK(!vault_base32_decode("MY=====", base, 64, &length));
    CHECK(!vault_base32_decode("MY======A", base, 64, &length));
    CHECK(!vault_base32_decode("MY0", base, 64, &length));
    CHECK(!vault_base32_decode("MZXW6YTBOI", base, 5, &length));
    CHECK(length == 0);
    CHECK(!vault_base32_decode("", base, 64, &length));
    CHECK(vault_text_valid("account@example.invalid", 128, false));
    CHECK(vault_text_valid("", 1, true));
    CHECK(!vault_text_valid("", 1, false));
    CHECK(!vault_text_valid("test", 4, false));
    CHECK(vault_text_valid("Caff\xc3\xa8", 8, false));
    CHECK(vault_text_valid("\xf0\x9f\x8c\x90", 8, false));
    CHECK(!vault_text_valid("\xc0\x80", 8, false));
    CHECK(!vault_text_valid("\xed\xa0\x80", 8, false));
    CHECK(!vault_text_valid("\xf4\x90\x80\x80", 8, false));
    CHECK(!vault_text_valid("\xc2\x85", 8, false));
    CHECK(!vault_text_valid("a\tb", 8, false));
    CHECK(!vault_text_valid("\xf0\x9f", 8, false));
    memset(base, 0xA5, 64);
    vault_wipe(base, 64);
    for (unsigned i = 0; i < 64; i++)
        CHECK(base[i] == 0);
    CHECK(vault_equal("same", "same", 4));
    CHECK(!vault_equal("same", "sAme", 4));
    return 0;
}
#ifdef SENTINEL_EMULATOR
__asm__(".global _start\n_start:\ncall test_packet\nebreak\nj .\n");
#else
int main(void)
{
    return test_packet();
}
#endif
