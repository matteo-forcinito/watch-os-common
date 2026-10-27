#include "vinyl.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
static double input_hz = 220;
static bool tone(void *unused, int64_t frame, int16_t stereo[2])
{
    (void)unused;
    stereo[0] = (int16_t)(12000 * sin(2 * PI * input_hz * frame / 44100));
    stereo[1] = -stereo[0];
    return true;
}
int main(void)
{
    const int rates[] = {-131072,-98304,-65536,-32768,32768,65536,98304,131072};
    setvbuf(stdout, NULL, _IONBF, 0);
    solco_keylock_t *state = calloc(1, sizeof(*state));
    assert(state);
    const double frequencies[] = {40,55,80,110,220,440};
    for (int f=0; f<6; ++f)
    for (int r = 0; r < 8; ++r)
    {
        input_hz=frequencies[f];
        memset(state,0,sizeof(*state));
        solco_vinyl_t v = {0};
        v.keylock = state; v.keylock_enabled = true; v.play_speed = SOLCO_RATE_FROM_Q16(rates[r]);
        v.speed = v.play_speed; v.cursor = (int64_t)44100 * 30 * SOLCO_SAMPLE_ONE;
        int crossings=0, previous=0, peak=0;
        for (int block = 0; block < 800; ++block)
        {
            int16_t out[512]; bool missing;
            assert(solco_vinyl_render(&v,out,256,true,44100*60,false,tone,NULL,&missing));
            assert(!missing);
            for (int i=0; i<256; ++i)
            {
                int a=out[i*2], b=out[i*2+1];
                assert(abs(a+b)<=2);
                if (block>=100)
                {
                    if (previous<0 && a>=0) ++crossings;
                    if (abs(a)>peak) peak=abs(a);
                }
                previous=a;
            }
        }
        double frequency=crossings*44100.0/(700*256);
        printf("input=%.0fHz rate=%d pitch=%.3fHz peak=%d\n",input_hz,rates[r],frequency,peak);
        assert(fabs(frequency-input_hz)<3);
        assert(peak>10000 && peak<=12002);
        int64_t expected=(int64_t)44100*30*SOLCO_SAMPLE_ONE+SOLCO_RATE_FROM_Q16(rates[r])*800*256;
        assert(llabs(v.cursor-expected)<SOLCO_SAMPLE_ONE);
        /* Touch takes priority; release returns to the selected rate. */
        solco_vinyl_touch(&v,0,0,44100*60,false);
        solco_vinyl_touch(&v,1,2205,44100*60,false);
        bool missing; int16_t out[512];
        assert(solco_vinyl_render(&v,out,256,false,44100*60,false,tone,NULL,&missing));
        assert(v.held && !missing);
        solco_vinyl_touch(&v,2,0,44100*60,false);
        v.keylock_enabled=false;
        for(int i=0;i<10;++i)solco_vinyl_render(&v,out,256,true,44100*60,false,tone,NULL,&missing);
        assert(state->wet<0.001f);
    }
    memset(state,0,sizeof(*state));
    input_hz=220;
    int16_t previous[2]={0}, current[2];
    for(int i=0;i<1400;++i)
    {
        assert(solco_keylock_sample(state,44100+i,1,44100*60,0,44100*60,false,tone,NULL,current));
        previous[0]=current[0];
    }
    assert(solco_keylock_sample(state,45500,-1,44100*60,44000,50000,true,tone,NULL,current));
    assert(abs(current[0]-previous[0])<1000);
    free(state);
    puts("PASS keylock: eight rates, fixed pitch, anti-phase stereo, timing, scratch and bypass");
    return 0;
}
