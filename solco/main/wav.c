#include "wav.h"
#include <limits.h>
#include <string.h>

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t u16(const unsigned char *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

bool solco_wav_open(FILE *file, solco_wav_t *wav)
{
    unsigned char header[16];
    bool format = false;
    memset(wav, 0, sizeof(*wav));
    if (fseek(file, 0, SEEK_END) != 0)
    {
        return false;
    }
    long length = ftell(file);
    if (length < 12 || fseek(file, 0, SEEK_SET) != 0 || fread(header, 1, 12, file) != 12 ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
    {
        return false;
    }
    uint64_t end = (uint64_t)u32(header + 4) + 8;
    if (end > (uint64_t)length || end < 12)
    {
        return false;
    }
    for (uint64_t pos = 12; pos + 8 <= end;)
    {
        if (pos > LONG_MAX || fseek(file, (long)pos, SEEK_SET) != 0 || fread(header, 1, 8, file) != 8)
        {
            return false;
        }
        uint32_t size = u32(header + 4);
        uint64_t next = pos + 8 + size + (size & 1);
        if (next > end || next > LONG_MAX)
        {
            return false;
        }
        if (!memcmp(header, "fmt ", 4))
        {
            if (format || size < 16 || fread(header, 1, 16, file) != 16 || u16(header) != 1 ||
                (u16(header + 2) != 1 && u16(header + 2) != 2) || u32(header + 4) != 44100 ||
                u16(header + 14) != 16 || u16(header + 12) != u16(header + 2) * 2 ||
                u32(header + 8) != 44100U * u16(header + 2) * 2)
            {
                return false;
            }
            wav->channels = u16(header + 2);
            format = true;
        }
        else if (!memcmp(header, "data", 4))
        {
            if (!format || !size || size % (wav->channels * 2))
            {
                return false;
            }
            wav->offset = (uint32_t)(pos + 8);
            wav->bytes = size;
            return true;
        }
        pos = next;
    }
    return false;
}
