#include "../ecos_audio_wire.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    int16_t input[ECOS_AUDIO_SAMPLES], output[ECOS_AUDIO_SAMPLES], reference[ECOS_AUDIO_SAMPLES];
    uint8_t packet[ECOS_AUDIO_FRAME_MAX + 1];
    ima_adpcm_t encoder = {0};
    uint64_t session;
    uint32_t offset, next = 0;
    size_t samples;
    for (unsigned block = 0; block < 20; ++block)
    {
        for (unsigned i = 0; i < ECOS_AUDIO_SAMPLES; ++i)
            input[i] = (int16_t)(24000 * sin(2 * 3.14159265 * 1000 * (block * ECOS_AUDIO_SAMPLES + i) / ECOS_AUDIO_RATE));
        ima_adpcm_t start = encoder;
        size_t n = ecos_audio_encode(packet, sizeof(packet), 7, block * ECOS_AUDIO_SAMPLES,
                                      &encoder, input, ECOS_AUDIO_SAMPLES);
        assert(n == ECOS_AUDIO_FRAME_MAX && n <= ECOS_ORBIT_DATA_MAX);
        /* Drop arbitrary packets; each surviving block carries its own state. */
        if (block % 3 == 1) continue;
        assert(ecos_audio_decode(packet, n, output, ECOS_AUDIO_SAMPLES, &session, &offset, &samples));
        ima_adpcm_decode(&start, packet + ECOS_AUDIO_HEADER, ECOS_AUDIO_SAMPLES, reference);
        assert(memcmp(output, reference, sizeof(output)) == 0 && start.predictor == encoder.predictor);
        assert(ecos_audio_advance(7, &next, session, offset, samples));
        assert(!ecos_audio_advance(7, &next, session, offset, samples));
        if (block > 3)
        {
            double signal = 0, error = 0;
            for (size_t i = 0; i < samples; ++i)
            {
                signal += (double)input[i] * input[i];
                double delta = (double)input[i] - output[i];
                error += delta * delta;
            }
            assert(10 * log10(signal / (error + 1e-9)) > 20);
        }
    }
    size_t n = ecos_audio_encode(packet, sizeof(packet), 7, next, &encoder, input, ECOS_AUDIO_SAMPLES);
    assert(n > 0);
    packet[20] = 89;
    assert(!ecos_audio_decode(packet, n, output, ECOS_AUDIO_SAMPLES, &session, &offset, &samples));
    packet[20] = 0;
    packet[21] = 1;
    assert(!ecos_audio_decode(packet, n, output, ECOS_AUDIO_SAMPLES, &session, &offset, &samples));
    packet[21] = 0;
    assert(!ecos_audio_decode(packet, n - 1, output, ECOS_AUDIO_SAMPLES, &session, &offset, &samples));
    assert(!ecos_audio_decode(packet, n + 1, output, ECOS_AUDIO_SAMPLES, &session, &offset, &samples));
    assert(!ecos_audio_decode(packet, n, output, ECOS_AUDIO_SAMPLES - 1, &session, &offset, &samples));
    assert(ecos_audio_encode(packet, sizeof(packet), 0, 0, &encoder, input, ECOS_AUDIO_SAMPLES) == 0);
    assert(ecos_audio_encode(packet, sizeof(packet), 7, UINT32_MAX, &encoder, input, ECOS_AUDIO_SAMPLES) == 0);
    assert(ecos_audio_encode(packet, sizeof(packet), 7, 0, &encoder, input, ECOS_AUDIO_SAMPLES - 1) == 0);
    assert(!ecos_audio_advance(8, &next, 7, next, ECOS_AUDIO_SAMPLES));
    puts("Orbit ADPCM wire tests passed: loss resync, SNR, ordering and bounds");
    return 0;
}
