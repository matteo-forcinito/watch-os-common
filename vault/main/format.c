/* Bounded format parsing and constant-time comparison; no platform dependency. */
#include "format.h"
#include <string.h>

static const uint8_t s_magic[8] = {'W', 'O', 'S', 'V', 'L', 'T', '1', 0};

void vault_wipe(void *memory, size_t length)
{
    volatile uint8_t *cursor = memory;
    while (length--)
        *cursor++ = 0;
}

bool vault_equal(const void *left, const void *right, size_t length)
{
    const uint8_t *a = left, *b = right;
    uint8_t different = 0;
    for (size_t i = 0; i < length; i++)
        different |= a[i] ^ b[i];
    return different == 0;
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

bool vault_header_decode(const uint8_t *file, size_t size, vault_header_t *header)
{
    if (file == NULL || header == NULL || size < VAULT_HEADER_SIZE + VAULT_TAG_SIZE ||
        !vault_equal(file, s_magic, 8) || file[8] != 1 || file[9] != 0 ||
        file[10] != VAULT_HEADER_SIZE || file[11] != 0 || file[12] != 1 || file[13] || file[14] ||
        file[15] || read32(file + 60))
        return false;
    uint32_t iterations = read32(file + 16), length = read32(file + 56);
    if (iterations < VAULT_KDF_ITERATIONS || iterations > VAULT_KDF_MAX_ITERATIONS || length == 0 ||
        length > VAULT_MAX_PAYLOAD || size != VAULT_HEADER_SIZE + VAULT_TAG_SIZE + (size_t)length)
        return false;
    header->iterations = iterations;
    header->generation = read32(file + 20) | (uint64_t)read32(file + 24) << 32;
    header->payload_size = length;
    memcpy(header->salt, file + 28, 16);
    memcpy(header->nonce, file + 44, 12);
    return true;
}

bool vault_header_encode(uint8_t output[VAULT_HEADER_SIZE], const vault_header_t *header)
{
    if (output == NULL || header == NULL || header->iterations < VAULT_KDF_ITERATIONS ||
        header->iterations > VAULT_KDF_MAX_ITERATIONS || header->payload_size == 0 ||
        header->payload_size > VAULT_MAX_PAYLOAD)
        return false;
    memset(output, 0, VAULT_HEADER_SIZE);
    memcpy(output, s_magic, 8);
    output[8] = 1;
    output[10] = VAULT_HEADER_SIZE;
    output[12] = 1;
    write32(output + 16, header->iterations);
    write32(output + 20, (uint32_t)header->generation);
    write32(output + 24, (uint32_t)(header->generation >> 32));
    memcpy(output + 28, header->salt, 16);
    memcpy(output + 44, header->nonce, 12);
    write32(output + 56, header->payload_size);
    return true;
}

bool vault_base32_decode(const char *text, uint8_t *output, size_t capacity, size_t *length)
{
    if (text == NULL || output == NULL || length == NULL)
        return false;
    uint32_t bits = 0;
    unsigned available = 0;
    size_t count = 0, symbols = 0, padding = 0;
    bool ended = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        unsigned char c = *p;
        if (c == ' ' || c == '-')
            continue;
        if (c == '=')
        {
            ended = true;
            padding++;
            continue;
        }
        if (ended)
            goto invalid;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        unsigned value;
        if (c >= 'A' && c <= 'Z')
            value = c - 'A';
        else if (c >= '2' && c <= '7')
            value = c - '2' + 26;
        else
            goto invalid;
        symbols++;
        bits = (bits << 5) | value;
        available += 5;
        if (available >= 8)
        {
            available -= 8;
            if (count == capacity)
                goto invalid;
            output[count++] = (uint8_t)(bits >> available);
        }
    }
    unsigned remainder = symbols % 8;
    if (count == 0 || remainder == 1 || remainder == 3 || remainder == 6 ||
        (available && (bits & ((1U << available) - 1))) ||
        (padding && (padding != (8 - remainder) % 8 || remainder == 0)))
        goto invalid;
    *length = count;
    return true;
invalid:
    vault_wipe(output, capacity);
    *length = 0;
    return false;
}

bool vault_text_valid(const char *text, size_t capacity, bool empty_allowed)
{
    if (text == NULL || capacity == 0)
        return false;
    size_t length = 0;
    while (length < capacity && text[length])
        length++;
    if (length == capacity || (!empty_allowed && length == 0))
        return false;
    for (size_t i = 0; i < length;)
    {
        uint32_t cp = (unsigned char)text[i++];
        if (cp < 32 || cp == 127)
            return false;
        if (cp < 128)
            continue;
        unsigned more;
        uint32_t minimum;
        if (cp >= 0xC2 && cp <= 0xDF)
        {
            cp &= 31;
            more = 1;
            minimum = 128;
        }
        else if (cp >= 0xE0 && cp <= 0xEF)
        {
            cp &= 15;
            more = 2;
            minimum = 0x800;
        }
        else if (cp >= 0xF0 && cp <= 0xF4)
        {
            cp &= 7;
            more = 3;
            minimum = 0x10000;
        }
        else
            return false;
        if (length - i < more)
            return false;
        while (more--)
        {
            unsigned c = (unsigned char)text[i++];
            if ((c & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp >= 0x80 && cp <= 0x9F))
            return false;
    }
    return true;
}
