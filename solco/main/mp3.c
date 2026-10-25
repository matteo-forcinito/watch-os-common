/* Preparation runs outside the real-time audio task. Source audio is never modified. */
#include "mp3.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define malloc(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define calloc(n, size) heap_caps_calloc((n), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define realloc(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../third_party/minimp3/minimp3_ex.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define PHASES 128
#define TAPS 32
#define RATE 44100
typedef struct
{
    FILE *file;
    uint64_t frames, limit;
    int16_t pcm[2048];
    size_t count;
} writer_t;

static size_t read_bytes(void *buffer, size_t count, void *context)
{
    size_t total = 0;
    while (total < count)
    {
        size_t chunk = count - total < 8192 ? count - total : 8192;
        size_t got = fread((unsigned char *)buffer + total, 1, chunk, context);
        total += got;
        if (got < chunk)
        {
            break;
        }
    }
    return total;
}
static int seek_bytes(uint64_t offset, void *context)
{
    return offset > LONG_MAX ? -1 : fseek(context, (long)offset, SEEK_SET);
}
static void u32(unsigned char *p, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}
static bool flush_pcm(writer_t *w)
{
    if (w->frames * 4 + 44 > w->limit || w->frames * 4 > INT32_MAX - 44)
    {
        return false;
    }
    bool ok = !w->count || fwrite(w->pcm, sizeof(int16_t), w->count, w->file) == w->count;
    w->count = 0;
    return ok;
}
static bool emit(writer_t *w, float left, float right)
{
    float samples[2] = {left, right};
    for (int ch = 0; ch < 2; ++ch)
    {
        float value = samples[ch];
        w->pcm[w->count++] = value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
    }
    ++w->frames;
    return w->count < 2048 || flush_pcm(w);
}

bool solco_mp3_decode(FILE *input, FILE *output, uint64_t byte_limit, solco_prepare_progress_fn progress,
                      void *context, char *error, size_t error_size)
{
    const char *failure = "MP3 non valido";
    mp3dec_ex_t *decoder = calloc(1, sizeof(*decoder));
    mp3d_sample_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(*pcm));
    writer_t *writer = calloc(1, sizeof(*writer));
    float *coefficients = NULL;
    float history[128][2] = {{0}};
    uint64_t source_frames = 0, output_position = 0;
    long input_size = 0;
    bool opened = false, ok = false;
    if (!decoder || !pcm || !writer)
    {
        failure = "Memoria MP3 insufficiente";
        goto done;
    }
    if (fseek(input, 0, SEEK_END) || (input_size = ftell(input)) <= 0)
    {
        goto done;
    }
    rewind(input);
    mp3dec_io_t io = {.read = read_bytes, .read_data = input, .seek = seek_bytes, .seek_data = input};
    int result = mp3dec_ex_open_cb(decoder, &io, MP3D_DO_NOT_SCAN);
    opened = true; /* close also frees partially initialized decoder allocations. */
    if (result || decoder->info.layer != 3 || decoder->info.hz < 8000 || decoder->info.hz > 48000 ||
        (decoder->info.channels != 1 && decoder->info.channels != 2))
    {
        failure = "MP3 non valido o non supportato";
        goto done;
    }
    const int rate = decoder->info.hz, channels = decoder->info.channels;
    writer->file = output;
    writer->limit = byte_limit;
    unsigned char header[44] = {'R', 'I', 'F', 'F', 0,  0, 0,   0,   'W', 'A',  'V',  'E', 'f', 'm',  't',
                                ' ', 16,  0,   0,   0,  1, 0,   2,   0,   0x44, 0xac, 0,   0,   0x10, 0xb1,
                                2,   0,   4,   0,   16, 0, 'd', 'a', 't', 'a',  0,    0,   0,   0};
    if (fwrite(header, 1, 44, output) != 44)
    {
        failure = "Impossibile scrivere cache";
        goto done;
    }
    if (rate != RATE)
    {
        coefficients = malloc(PHASES * TAPS * sizeof(*coefficients));
        if (!coefficients)
        {
            failure = "Memoria resampling insufficiente";
            goto done;
        }
        const double cutoff = 0.94 * (rate > RATE ? (double)RATE / rate : 1.0);
        for (int phase = 0; phase < PHASES; ++phase)
        {
            double sum = 0;
            for (int tap = 0; tap < TAPS; ++tap)
            {
                double x = tap - 15 - (double)phase / PHASES;
                double window =
                    0.42 + 0.5 * cos(3.141592653589793 * x / 16) + 0.08 * cos(6.283185307179586 * x / 16);
                double value =
                    fabs(x) < 1e-10 ? cutoff : sin(3.141592653589793 * cutoff * x) / (3.141592653589793 * x);
                coefficients[phase * TAPS + tap] = (float)(value * window);
                sum += value * window;
            }
            for (int tap = 0; tap < TAPS; ++tap)
            {
                coefficients[phase * TAPS + tap] /= (float)sum;
            }
        }
    }
    for (;;)
    {
        size_t count = mp3dec_ex_read(decoder, pcm, MINIMP3_MAX_SAMPLES_PER_FRAME);
        if (!count)
        {
            break;
        }
        if (count % channels)
        {
            goto done;
        }
        for (size_t i = 0; i < count / channels; ++i)
        {
            float left = pcm[i * channels], right = pcm[i * channels + channels - 1];
            history[source_frames % 128][0] = left;
            history[source_frames % 128][1] = right;
            ++source_frames;
            if (rate == RATE)
            {
                if (!emit(writer, left, right))
                {
                    failure = "Spazio cache esaurito";
                    goto done;
                }
            }
            else
            {
                while (output_position / RATE + 16 < source_frames)
                {
                    int64_t center = (int64_t)(output_position / RATE);
                    int phase = (int)((output_position % RATE) * PHASES / RATE);
                    float value[2] = {0};
                    for (int tap = 0; tap < TAPS; ++tap)
                    {
                        int64_t at = center + tap - 15;
                        if (at < 0)
                        {
                            continue;
                        }
                        float weight = coefficients[phase * TAPS + tap];
                        value[0] += history[at % 128][0] * weight;
                        value[1] += history[at % 128][1] * weight;
                    }
                    if (!emit(writer, value[0], value[1]))
                    {
                        failure = "Spazio cache esaurito";
                        goto done;
                    }
                    output_position += rate;
                }
            }
        }
        if (progress && !progress((int)(decoder->offset * 100 / (uint64_t)input_size), context))
        {
            failure = "Preparazione annullata";
            goto done;
        }
    }
    if (decoder->last_error || ferror(input) || !source_frames)
    {
        failure = "MP3 incompleto o errore microSD";
        goto done;
    }
    if (coefficients)
    {
        uint64_t end = source_frames * RATE;
        while (output_position < end)
        {
            int64_t center = (int64_t)(output_position / RATE);
            int phase = (int)((output_position % RATE) * PHASES / RATE);
            float value[2] = {0};
            for (int tap = 0; tap < TAPS; ++tap)
            {
                int64_t at = center + tap - 15;
                if (at < 0 || (uint64_t)at >= source_frames)
                {
                    continue;
                }
                float weight = coefficients[phase * TAPS + tap];
                value[0] += history[at % 128][0] * weight;
                value[1] += history[at % 128][1] * weight;
            }
            if (!emit(writer, value[0], value[1]))
            {
                failure = "Spazio cache esaurito";
                goto done;
            }
            output_position += rate;
        }
    }
    if (!flush_pcm(writer))
    {
        failure = "Spazio cache esaurito";
        goto done;
    }
    u32(header + 4, (uint32_t)(writer->frames * 4 + 36));
    u32(header + 40, (uint32_t)(writer->frames * 4));
    if (fseek(output, 0, SEEK_SET) || fwrite(header, 1, 44, output) != 44 || fflush(output))
    {
        failure = "Errore scrittura cache";
        goto done;
    }
    ok = true;
done:
    if (opened)
    {
        mp3dec_ex_close(decoder);
    }
    free(decoder);
    free(pcm);
    free(writer);
    free(coefficients);
    if (error_size)
    {
        snprintf(error, error_size, "%s", ok ? "" : failure);
    }
    return ok;
}
