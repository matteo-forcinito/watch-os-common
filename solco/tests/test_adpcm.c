/* IMA ADPCM: round trip SNR on music-like signals and self-contained blocks. */
#include "adpcm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define N 256

static double snr(const int16_t *a, const int16_t *b, int n)
{
    double s = 0, e = 0;
    for (int i = 0; i < n; ++i)
    {
        s += (double)a[i] * a[i];
        e += (double)(a[i] - b[i]) * (a[i] - b[i]);
    }
    return 10 * log10(s / (e + 1e-9));
}

int main(void)
{
    const double freq[] = {60, 440, 3000, 9000};
    for (int f = 0; f < 4; ++f)
    {
        int16_t pcm[N * 40], out[N * 40];
        for (int i = 0; i < N * 40; ++i)
            pcm[i] = (int16_t)(28000 * sin(2 * 3.14159265 * freq[f] * i / 44100.0));
        solco_adpcm_t enc = {0}, dec;
        for (int b = 0; b < 40; ++b)
        {
            uint8_t code[N / 2];
            solco_adpcm_t start = enc; /* state carried in the packet header */
            solco_adpcm_encode(&enc, pcm + b * N, N, code);
            dec = start;
            solco_adpcm_decode(&dec, code, N, out + b * N);
            assert(dec.predictor == enc.predictor && dec.index == enc.index);
        }
        double r = snr(pcm + N * 4, out + N * 4, N * 36); /* after the step adapts */
        printf("%.0f Hz: SNR %.1f dB\n", freq[f], r);
        assert(r > (freq[f] > 5000 ? 12 : 20));
    }
    puts("PASS adpcm: SNR, encoder/decoder state agreement, independent blocks");
    return 0;
}
