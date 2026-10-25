#include "orbi/pokemon_pack.h"
#include <string.h>

enum { HEADER = 20, ENTRY = 12, MAX_ENTRIES = 302, MAX_BYTES = 4 * 1024 * 1024 };
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool read_at(FILE *f, uint32_t offset, void *data, size_t size)
{
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(data, 1, size, f) == size;
}
void pokemon_pack_close(pokemon_pack_t *p)
{
    if (p && p->file) fclose(p->file);
    if (p) memset(p, 0, sizeof(*p));
}
bool pokemon_pack_open(pokemon_pack_t *p, const char *path)
{
    if (!p || !path) return false;
    /* Pass an empty/closed handle; don't silently leak an existing file. */
    if (p->file) return false;
    memset(p, 0, sizeof(*p));
    p->file = fopen(path, "rb");
    if (!p->file) return false;
    uint8_t h[HEADER], b[256];
    if (fseek(p->file, 0, SEEK_END)) goto fail;
    long length = ftell(p->file);
    if (length < HEADER || length > MAX_BYTES || !read_at(p->file, 0, h, sizeof(h))) goto fail;
    p->bytes = (uint32_t)length;
    p->count = u16(h + 6);
    p->data_offset = u32(h + 12);
    if (memcmp(h, "PKP1", 4) || u16(h + 4) != 1 || !p->count || p->count > MAX_ENTRIES ||
        u32(h + 8) != HEADER || p->data_offset != HEADER + ENTRY * (uint32_t)p->count ||
        p->data_offset > p->bytes) goto fail;
    uint32_t crc = UINT32_MAX;
    for (uint32_t pos = HEADER; pos < p->bytes;)
    {
        size_t n = p->bytes - pos;
        if (n > sizeof(b)) n = sizeof(b);
        if (!read_at(p->file, pos, b, n)) goto fail;
        for (size_t i = 0; i < n; i++)
        {
            crc ^= b[i];
            for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        pos += (uint32_t)n;
    }
    if ((crc ^ UINT32_MAX) != u32(h + 16)) goto fail;
    uint32_t previous_key = 0, end = p->data_offset;
    for (uint32_t i = 0; i < p->count; i++)
    {
        if (!read_at(p->file, HEADER + i * ENTRY, b, ENTRY)) goto fail;
        uint32_t key = (uint32_t)u16(b) * 2 + b[2], offset = u32(b + 4);
        unsigned colors = u16(b + 8), frames = b[3];
        if (!u16(b) || u16(b) > 151 || b[2] > 1 || key <= previous_key ||
            !frames || frames > 4 || !colors || colors > 255 || u16(b + 10) || offset != end) goto fail;
        uint32_t size = colors * 2 + frames * 1600;
        if (offset > p->bytes || size > p->bytes - offset) goto fail;
        previous_key = key;
        end = offset + size;
    }
    if (end != p->bytes) goto fail;
    return true;
fail:
    pokemon_pack_close(p);
    return false;
}
bool pokemon_pack_read(pokemon_pack_t *p, uint16_t dex, bool shiny, uint32_t frame, uint32_t out[1024])
{
    if (!p || !p->file || !out) return false;
    uint8_t entry[ENTRY], palette[510], pixels[1600];
    for (uint32_t i = 0; i < p->count; i++)
    {
        if (!read_at(p->file, HEADER + i * ENTRY, entry, ENTRY)) return false;
        if (u16(entry) != dex || entry[2] != (unsigned)shiny) continue;
        unsigned colors = u16(entry + 8), frames = entry[3];
        if (!frames || frames > 4 || !colors || colors > 255) return false;
        uint32_t offset = u32(entry + 4), bytes = colors * 2 + frames * 1600;
        if (offset < p->data_offset || offset > p->bytes || bytes > p->bytes - offset ||
            !read_at(p->file, offset, palette, colors * 2) ||
            !read_at(p->file, offset + colors * 2 + (frame % frames) * 1600, pixels, sizeof(pixels))) return false;
        for (unsigned j = 0; j < sizeof(pixels); j++)
            if (pixels[j] != 255 && pixels[j] >= colors) return false;
        for (unsigned y = 0; y < 32; y++) for (unsigned x = 0; x < 32; x++)
        {
            unsigned index = pixels[(y * 40 / 32) * 40 + x * 40 / 32];
            uint32_t color = 0;
            if (index != 255)
            {
                unsigned rgb = u16(palette + index * 2);
                color = 0xFF000000u | ((rgb >> 11) * 255 / 31) << 16 |
                        (((rgb >> 5) & 63) * 255 / 63) << 8 | (rgb & 31) * 255 / 31;
            }
            out[y * 32 + x] = color;
        }
        return true;
    }
    return false;
}
