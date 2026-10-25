/* Host tests exercise malformed RIFF input and mixer endpoints/headroom. */
#include "mix.h"
#include "wav.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static void wav_tests(void)
{
    unsigned char bytes[] = {'R',  'I',  'F', 'F', 40,   0,    0, 0, 'W', 'A', 'V', 'E',
                             'f',  'm',  't', ' ', 16,   0,    0, 0, 1,   0,   2,   0,
                             0x44, 0xac, 0,   0,   0x10, 0xb1, 2, 0, 4,   0,   16,  0,
                             'd',  'a',  't', 'a', 4,    0,    0, 0, 0,   0,   0,   0};
    solco_wav_t wav;
    FILE *f = tmpfile();
    assert(f);
    assert(fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes));
    assert(solco_wav_open(f, &wav));
    assert(wav.channels == 2 && wav.offset == 44 && wav.bytes == 4);
    bytes[24] = 0x80; /* Unsupported sample rate. */
    rewind(f);
    fwrite(bytes, 1, sizeof(bytes), f);
    assert(!solco_wav_open(f, &wav));
    bytes[24] = 0x44;
    bytes[40] = 255; /* Truncated data chunk. */
    rewind(f);
    fwrite(bytes, 1, sizeof(bytes), f);
    assert(!solco_wav_open(f, &wav));
    bytes[40] = 4;
    bytes[34] = 24; /* Unsupported precision. */
    rewind(f);
    fwrite(bytes, 1, sizeof(bytes), f);
    assert(!solco_wav_open(f, &wav));
    fclose(f);
}

int main(void)
{
    wav_tests();
    int16_t stereo[] = {32767, 32767, -32768, -32768, 32767, -32767, 1000, 0};
    assert(solco_fold_mono(stereo, 4) == 32768);
    assert(stereo[0] == 32767 && stereo[1] == -32768 && stereo[2] == 0 && stereo[3] == 500);
    int16_t a[512], b[512], out[512];
    for (int i = 0; i < 512; ++i)
    {
        a[i] = 32767;
        b[i] = -32767;
    }
    solco_mix_t mix = {0};
    for (int i = 0; i < 32; ++i)
    {
        solco_mix(&mix, a, b, out, 256, 0, 100, 1, 1);
    }
    assert(out[510] > 23160 && out[510] < 23172);
    for (int i = 0; i < 32; ++i)
    {
        solco_mix(&mix, a, b, out, 256, 1000, 100, 1, 1);
    }
    assert(out[510] < -23160 && out[510] > -23172);
    for (int i = 0; i < 512; ++i)
    {
        b[i] = 32767;
    }
    for (int i = 0; i < 32; ++i)
    {
        solco_mix(&mix, a, b, out, 256, 500, 100, 1, 1);
    }
    assert(out[510] > 32750 && mix.clipped == 0);
    for (int i = 0; i < 1000; ++i)
    {
        solco_mix(&mix, a, b, out, 256, (i * 137) % 1001, 100, 1, 1);
        assert(mix.clipped == 0);
    }
    for (int i = 0; i < 32; ++i)
    {
        solco_mix(&mix, a, b, out, 256, 500, 100, 0, 0);
    }
    assert(out[510] == 0);
    for (int i = 0; i < 32; ++i)
    {
        solco_mix(&mix, a, b, out, 256, 500, 0, 1, 1);
    }
    assert(out[510] == 0);
    int16_t bus[512];
    for (int i=0;i<512;i++) bus[i]=24000;
    mix.a=.5f; mix.b=.5f; mix.master=1; mix.studio=0;
    solco_mix_studio(&mix,a,b,bus,out,256,500,100,1,1,1);
    assert(mix.clipped==0);
    memset(&mix,0,sizeof(mix));
    for (int i=0;i<32;i++) solco_mix_studio(&mix,a,b,bus,out,256,500,100,1,1,1);
    assert(out[510]>28000 && out[510]<28400 && mix.clipped==0);
    for (int i=0;i<32;i++) solco_mix_studio(&mix,a,b,bus,out,256,500,0,1,1,1);
    assert(out[510]==0);
    puts("Solco: WAV validation and mixer tests passed");
    return 0;
}
